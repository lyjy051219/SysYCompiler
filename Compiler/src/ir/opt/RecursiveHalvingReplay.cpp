#include "ir/opt/RecursiveHalvingReplay.hpp"

#include "ir/opt/DCE.hpp"
#include "lib/IRUtils.hpp"
#include "lib/TypeSystem.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 判断块是否按指定整数常量分出相等边和不等边。
bool RecursiveHalvingReplayPass::matchEqualityDispatch(
    BasicBlock *BB, Value *Compared, int64_t Constant,
    BasicBlock *&EqualSuccessor, BasicBlock *&OtherSuccessor) {
    EqualSuccessor = nullptr;
    OtherSuccessor = nullptr;
    if (!BB || !Compared || BB->size() != 2) return false;
    auto *Branch = BB->getBranchTerminator();
    if (!BasicBlock::isCondBr(Branch)) return false;
    auto *Compare = dynamic_cast<BinaryInst *>(Branch->getCondition());
    if (!Compare || Compare->getParent() != BB ||
        (Compare->getOp() != BinaryInst::E &&
         Compare->getOp() != BinaryInst::NE) ||
        !Compare->hasExactlyOneUse()) {
        return false;
    }
    Value *Left = nullptr;
    Value *Right = nullptr;
    if (!Compare->getOperands(Left, Right)) return false;
    Value *ConstantValue = nullptr;
    if (Left == Compared) ConstantValue = Right;
    else if (Right == Compared) ConstantValue = Left;
    else return false;
    int64_t Actual = 0;
    const int Width = Compared->integerBitWidth(0);
    if (Width != 32 || !ConstantValue ||
        !ConstantValue->tryGetInt64ConstAtWidth(32, Actual) ||
        Actual != Constant) {
        return false;
    }
    if (Compare->getOp() == BinaryInst::E) {
        EqualSuccessor = Branch->getTrueSuccessor();
        OtherSuccessor = Branch->getFalseSuccessor();
    } else {
        EqualSuccessor = Branch->getFalseSuccessor();
        OtherSuccessor = Branch->getTrueSuccessor();
    }
    return EqualSuccessor && OtherSuccessor &&
           EqualSuccessor != OtherSuccessor;
}

// 识别 sdiv x,2 或其对负数保持向零舍入的偏置右移规范式。
bool RecursiveHalvingReplayPass::matchSignedHalving(
    Value *V, Value *Source, BasicBlock *ExpectedBlock,
    std::vector<Instruction *> &Slice) {
    Slice.clear();
    if (!V || !Source || !ExpectedBlock ||
        !Source->getIRType() ||
        !Source->getIRType()->hasIntegerBitWidth(32)) {
        return false;
    }
    auto IsConst = [](Value *Candidate, int64_t Expected) {
        int64_t Actual = 0;
        return Candidate &&
               Candidate->tryGetInt64ConstAtWidth(32, Actual) &&
               Actual == Expected;
    };
    auto *Quotient = dynamic_cast<BinaryInst *>(V);
    Value *Left = nullptr;
    Value *Right = nullptr;
    if (!Quotient || Quotient->getParent() != ExpectedBlock ||
        !Quotient->getOperands(Left, Right)) {
        return false;
    }
    if (Quotient->getOp() == BinaryInst::DIV && Left == Source &&
        IsConst(Right, 2)) {
        Slice.push_back(Quotient);
        return true;
    }
    if (Quotient->getOp() != BinaryInst::ASHR ||
        !IsConst(Right, 1)) {
        return false;
    }
    auto *Biased = dynamic_cast<BinaryInst *>(Left);
    Value *AddLeft = nullptr;
    Value *AddRight = nullptr;
    if (!Biased || Biased->getParent() != ExpectedBlock ||
        Biased->getOp() != BinaryInst::ADD ||
        !Biased->getOperands(AddLeft, AddRight)) {
        return false;
    }
    Value *BiasValue = nullptr;
    if (AddLeft == Source) BiasValue = AddRight;
    else if (AddRight == Source) BiasValue = AddLeft;
    else return false;
    auto *Bias = dynamic_cast<BinaryInst *>(BiasValue);
    Value *BiasLeft = nullptr;
    Value *BiasRight = nullptr;
    if (!Bias || Bias->getParent() != ExpectedBlock ||
        Bias->getOp() != BinaryInst::AND ||
        !Bias->getOperands(BiasLeft, BiasRight)) {
        return false;
    }
    Value *SignValue = nullptr;
    if (IsConst(BiasLeft, 1)) SignValue = BiasRight;
    else if (IsConst(BiasRight, 1)) SignValue = BiasLeft;
    else return false;
    auto *Sign = dynamic_cast<BinaryInst *>(SignValue);
    Value *SignLeft = nullptr;
    Value *SignRight = nullptr;
    if (!Sign || Sign->getParent() != ExpectedBlock ||
        Sign->getOp() != BinaryInst::ASHR ||
        !Sign->getOperands(SignLeft, SignRight) ||
        SignLeft != Source || !IsConst(SignRight, 31)) {
        return false;
    }
    Slice = {Sign, Bias, Biased, Quotient};
    return true;
}

// 从只含 PHI 和 ret 的共享出口按指定前驱解析实际返回值。
bool RecursiveHalvingReplayPass::resolveSharedExitValue(
    BasicBlock *Exit, BasicBlock *Pred, Value *&Result) {
    Result = nullptr;
    if (!Exit || !Pred || Pred->countEdgesToSucc(Exit) != 1) return false;
    auto *Return = dynamic_cast<RetInst *>(Exit->back());
    if (!Return || !Return->getReturnValue()) return false;
    for (Instruction *I : *Exit) {
        if (I == Return) continue;
        if (!dynamic_cast<PhiInst *>(I)) return false;
    }
    Value *Current = Return->getReturnValue();
    std::unordered_set<Value *> Seen;
    while (auto *Phi = dynamic_cast<PhiInst *>(Current)) {
        if (Phi->getParent() != Exit ||
            Phi->countIncomingFromPred(Pred) != 1 ||
            !Seen.insert(Phi).second) {
            return false;
        }
        Current = Phi->getIncomingFromPred(Pred);
        if (!Current) return false;
    }
    if (auto *I = dynamic_cast<Instruction *>(Current)) {
        if (I->getParent() == Exit) return false;
    }
    Result = Current;
    return true;
}

// 解析一条至多经过三个直线块的返回路径，并保存需在新块克隆的计算。
bool RecursiveHalvingReplayPass::resolveReturnPath(
    BasicBlock *Start, BasicBlock *IncomingPred, ReturnPath &Path) {
    Path = ReturnPath{};
    if (!Start || !IncomingPred) return false;
    Value *SharedResult = nullptr;
    if (resolveSharedExitValue(Start, IncomingPred, SharedResult)) {
        Path.Result = SharedResult;
        Path.SharedExit = Start;
        return true;
    }
    BasicBlock *Current = Start;
    BasicBlock *Pred = IncomingPred;
    for (unsigned Depth = 0; Depth < 3; ++Depth) {
        if (!Current || Current == Pred ||
            Pred->countEdgesToSucc(Current) != 1) {
            return false;
        }
        auto Preds = Current->getPredecessors();
        if (Preds.size() != 1 || Preds.front() != Pred ||
            !Current->collectPhiNodes().empty()) {
            return false;
        }
        Instruction *Terminator = Current->getTerminator();
        if (!Terminator) return false;
        Path.Blocks.push_back(Current);
        for (Instruction *I : *Current) {
            if (I == Terminator) continue;
            Path.Slice.push_back(I);
        }
        if (auto *Return = dynamic_cast<RetInst *>(Terminator)) {
            Path.Result = Return->getReturnValue();
            return Path.Result != nullptr;
        }
        auto *Branch = dynamic_cast<BrInst *>(Terminator);
        if (!BasicBlock::isUncondBr(Branch)) return false;
        BasicBlock *Next = Branch->getSuccessor(0);
        if (!Next) return false;
        if (resolveSharedExitValue(Next, Current, SharedResult)) {
            Path.Result = SharedResult;
            Path.SharedExit = Next;
            return true;
        }
        Pred = Current;
        Current = Next;
    }
    return false;
}

// 递归收集分支条件在 self-call 之后的纯值依赖闭包。
bool RecursiveHalvingReplayPass::collectSelectorDependency(
    Value *V, BasicBlock *BB,
    const std::unordered_set<Instruction *> &Suffix,
    std::unordered_set<Instruction *> &Selected,
    std::unordered_set<Instruction *> &Visiting) {
    if (!V || Value::isUndef(V)) return false;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return V->isConst() || V->isParam();
    if (I->getParent() != BB || !Suffix.count(I)) return false;
    if (Selected.count(I)) return true;
    if (!Visiting.insert(I).second) return false;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        if (!collectSelectorDependency(I->getOperand(Index), BB, Suffix,
                                       Selected, Visiting)) {
            Visiting.erase(I);
            return false;
        }
    }
    Visiting.erase(I);
    Selected.insert(I);
    return true;
}

// 将 call 后直线区间拆成可提前的 selector 和保持原顺序的 common slice。
bool RecursiveHalvingReplayPass::collectSelectorSlice(
    CallInst *SelfCall, BrInst *Branch,
    std::vector<Instruction *> &Selector,
    std::vector<Instruction *> &Common) {
    Selector.clear();
    Common.clear();
    if (!SelfCall || !Branch || !BasicBlock::isCondBr(Branch) ||
        SelfCall->getParent() != Branch->getParent() ||
        Branch != SelfCall->getParent()->back()) {
        return false;
    }
    BasicBlock *BB = SelfCall->getParent();
    std::vector<Instruction *> OrderedSuffix;
    std::unordered_set<Instruction *> Suffix;
    for (Instruction *I = SelfCall->getNext(); I && I != Branch;
         I = I->getNext()) {
        OrderedSuffix.push_back(I);
        Suffix.insert(I);
    }
    if (OrderedSuffix.empty()) return false;
    std::unordered_set<Instruction *> Selected;
    std::unordered_set<Instruction *> Visiting;
    if (!collectSelectorDependency(Branch->getCondition(), BB, Suffix,
                                   Selected, Visiting)) {
        return false;
    }
    for (Instruction *I : OrderedSuffix) {
        if (Selected.count(I)) Selector.push_back(I);
        else Common.push_back(I);
    }
    if (Selector.empty() || Common.empty()) return false;
    for (Instruction *I : Selector) {
        for (Use *U : I->getUses()) {
            auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
            if (!UserI || (UserI != Branch && !Selected.count(UserI)))
                return false;
        }
    }
    for (Instruction *I : Common) {
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            auto *OperandI = dynamic_cast<Instruction *>(I->getOperand(Index));
            if (OperandI && Selected.count(OperandI)) return false;
        }
    }
    return validateStraightLineSlice(Selector) &&
           validateStraightLineSlice(Common);
}

// 判断整数指令对任意已定义输入都不会故障或产生 poison。
bool RecursiveHalvingReplayPass::isAlwaysDefinedIntegerInstruction(
    Instruction *I) {
    auto *Binary = dynamic_cast<BinaryInst *>(I);
    if (!Binary) return false;
    Value *Left = nullptr;
    Value *Right = nullptr;
    if (!Binary->getOperands(Left, Right) || !Left || !Right ||
        !Left->getIRType() || !Left->getIRType()->isInt() ||
        !Right->getIRType() || !Right->getIRType()->isInt() ||
        valueMayDependOnUndef(Left) || valueMayDependOnUndef(Right)) {
        return false;
    }
    switch (Binary->getOp()) {
    case BinaryInst::ADD:
    case BinaryInst::SUB:
    case BinaryInst::MUL:
    case BinaryInst::AND:
    case BinaryInst::OR:
    case BinaryInst::XOR:
    case BinaryInst::E:
    case BinaryInst::NE:
    case BinaryInst::GE:
    case BinaryInst::L:
    case BinaryInst::LE:
    case BinaryInst::G:
        return true;
    case BinaryInst::SHL:
    case BinaryInst::LSHR:
    case BinaryInst::ASHR: {
        const int Width = Left->integerBitWidth(0);
        int64_t Amount = 0;
        return Width > 0 && Right->tryGetInt64ConstAtWidth(Width, Amount) &&
               Amount >= 0 && Amount < Width;
    }
    case BinaryInst::DIV:
    case BinaryInst::MOD:
    case BinaryInst::UDIV:
    case BinaryInst::UREM: {
        const int Width = Left->integerBitWidth(0);
        int64_t Divisor = 0;
        if (Width <= 0 ||
            !Right->tryGetInt64ConstAtWidth(Width, Divisor) ||
            Divisor == 0) {
            return false;
        }
        if ((Binary->getOp() == BinaryInst::DIV ||
             Binary->getOp() == BinaryInst::MOD) &&
            Divisor == -1) {
            return false;
        }
        return true;
    }
    default:
        return false;
    }
}

// 检查待克隆直线区间只含有界且总是已定义的整数计算。
bool RecursiveHalvingReplayPass::validateStraightLineSlice(
    const std::vector<Instruction *> &Slice) {
    for (Instruction *I : Slice)
        if (!isAlwaysDefinedIntegerInstruction(I)) return false;
    return true;
}

// 有界判断值闭包是否依赖指定 SSA 值。
bool RecursiveHalvingReplayPass::dependsOnValue(
    Value *Root, Value *Needle, std::unordered_set<Value *> &Visiting,
    Value *StopAt, unsigned Depth) {
    if (!Root || !Needle || Depth > 64) return true;
    if (Root == Needle) return true;
    if (Root == StopAt) return false;
    auto *I = dynamic_cast<Instruction *>(Root);
    if (!I) return false;
    if (!Visiting.insert(I).second) return true;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        if (dependsOnValue(I->getOperand(Index), Needle, Visiting,
                           StopAt, Depth + 1)) {
            Visiting.erase(I);
            return true;
        }
    }
    Visiting.erase(I);
    return false;
}

// 收集严格的单自调用折半递归计划，任何未建模结构都整体拒绝。
bool RecursiveHalvingReplayPass::collectPlan(Function &F, Plan &P) {
    P = Plan{};
    if (F.hasReservedSignature() || !F.front() ||
        !F.getIRType() || !F.getIRType()->hasIntegerBitWidth(32) ||
        F.getParamCount() == 0 || F.getParamCount() > 4 ||
        F.size() > 10 || !F.hasOnlyDirectCallUses()) {
        return false;
    }
    for (const auto &ParamValue : F.getParams()) {
        if (!ParamValue || !ParamValue->getIRType() ||
            !ParamValue->getIRType()->hasIntegerBitWidth(32)) {
            return false;
        }
    }
    std::size_t InstructionCount = 0;
    std::vector<CallInst *> Calls;
    for (BasicBlock *BB : F) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            ++InstructionCount;
            if (auto *Call = dynamic_cast<CallInst *>(I)) Calls.push_back(Call);
        }
    }
    if (InstructionCount > 32 || Calls.size() != 1 ||
        Calls.front()->getDirectCallee() != &F ||
        Calls.front()->getArgCount() != F.getParamCount()) {
        return false;
    }
    CallInst *SelfCall = Calls.front();
    BasicBlock *RecursiveBlock = SelfCall->getParent();
    if (!RecursiveBlock || SelfCall->getIRType() != F.getIRType())
        return false;

    bool FoundRecursiveArg = false;
    std::vector<Instruction *> HalfSlice;
    unsigned RecursiveIndex = 0;
    for (unsigned Index = 0; Index < F.getParamCount(); ++Index) {
        std::vector<Instruction *> CandidateHalf;
        Param *ParamValue = F.getParam(Index);
        if (!matchSignedHalving(SelfCall->getArg(Index), ParamValue,
                                RecursiveBlock, CandidateHalf)) {
            continue;
        }
        bool OtherArgsForwarded = true;
        for (unsigned Other = 0; Other < F.getParamCount(); ++Other) {
            if (Other == Index) continue;
            if (SelfCall->getArg(Other) != F.getParam(Other)) {
                OtherArgsForwarded = false;
                break;
            }
        }
        if (!OtherArgsForwarded || FoundRecursiveArg) return false;
        FoundRecursiveArg = true;
        RecursiveIndex = Index;
        HalfSlice = std::move(CandidateHalf);
    }
    if (!FoundRecursiveArg || !validateStraightLineSlice(HalfSlice))
        return false;
    std::vector<Instruction *> BeforeCall;
    for (Instruction *I = RecursiveBlock->front(); I && I != SelfCall;
         I = I->getNext()) {
        BeforeCall.push_back(I);
    }
    if (BeforeCall != HalfSlice) return false;

    Param *RecursiveParam = F.getParam(RecursiveIndex);
    BasicBlock *BaseZeroStart = nullptr;
    BasicBlock *SecondTest = nullptr;
    if (!matchEqualityDispatch(F.front(), RecursiveParam, 0,
                               BaseZeroStart, SecondTest)) {
        return false;
    }
    BasicBlock *BaseOneStart = nullptr;
    BasicBlock *NonBaseStart = nullptr;
    if (!matchEqualityDispatch(SecondTest, RecursiveParam, 1,
                               BaseOneStart, NonBaseStart) ||
        NonBaseStart != RecursiveBlock) {
        return false;
    }
    auto EntryPreds = F.front()->getPredecessors();
    auto SecondPreds = SecondTest->getPredecessors();
    auto RecursivePreds = RecursiveBlock->getPredecessors();
    if (!EntryPreds.empty() || SecondPreds.size() != 1 ||
        SecondPreds.front() != F.front() || RecursivePreds.size() != 1 ||
        RecursivePreds.front() != SecondTest) {
        return false;
    }

    ReturnPath BaseZero;
    ReturnPath BaseOne;
    if (!resolveReturnPath(BaseZeroStart, F.front(), BaseZero) ||
        !resolveReturnPath(BaseOneStart, SecondTest, BaseOne)) {
        return false;
    }
    auto *ContinuationBranch = RecursiveBlock->getBranchTerminator();
    if (!BasicBlock::isCondBr(ContinuationBranch)) return false;
    std::vector<Instruction *> Selector;
    std::vector<Instruction *> Common;
    if (!collectSelectorSlice(SelfCall, ContinuationBranch,
                              Selector, Common)) {
        return false;
    }
    ReturnPath TrueArm;
    ReturnPath FalseArm;
    if (!resolveReturnPath(ContinuationBranch->getTrueSuccessor(),
                           RecursiveBlock, TrueArm) ||
        !resolveReturnPath(ContinuationBranch->getFalseSuccessor(),
                           RecursiveBlock, FalseArm)) {
        return false;
    }
    if (Selector.size() > 4 || Common.size() > 6 ||
        BaseZero.Slice.size() + BaseOne.Slice.size() > 4 ||
        TrueArm.Slice.size() + FalseArm.Slice.size() > 6 ||
        !validateStraightLineSlice(BaseZero.Slice) ||
        !validateStraightLineSlice(BaseOne.Slice) ||
        !validateStraightLineSlice(TrueArm.Slice) ||
        !validateStraightLineSlice(FalseArm.Slice)) {
        return false;
    }
    for (Value *Root : {BaseZero.Result, BaseOne.Result,
                        TrueArm.Result, FalseArm.Result}) {
        if (!Root || !Root->getIRType() ||
            !Root->getIRType()->hasIntegerBitWidth(32)) {
            return false;
        }
        std::unordered_set<Value *> Visiting;
        Value *StopAt = (Root == BaseZero.Result || Root == BaseOne.Result)
                            ? nullptr
                            : SelfCall;
        if (dependsOnValue(Root, RecursiveParam, Visiting, StopAt))
            return false;
    }
    for (Instruction *I : Common) {
        std::unordered_set<Value *> Visiting;
        if (dependsOnValue(I, RecursiveParam, Visiting, SelfCall))
            return false;
    }

    std::unordered_set<BasicBlock *> Covered{F.front(), SecondTest,
                                             RecursiveBlock};
    std::unordered_set<BasicBlock *> SharedExits;
    auto AddPath = [&](const ReturnPath &Path) {
        for (BasicBlock *BB : Path.Blocks) {
            if (!BB || !Covered.insert(BB).second) return false;
        }
        if (Path.SharedExit) SharedExits.insert(Path.SharedExit);
        return true;
    };
    if (!AddPath(BaseZero) || !AddPath(BaseOne) ||
        !AddPath(TrueArm) || !AddPath(FalseArm)) {
        return false;
    }
    Covered.insert(SharedExits.begin(), SharedExits.end());
    if (Covered.size() != static_cast<std::size_t>(F.size())) return false;

    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            if (I == SelfCall || dynamic_cast<BrInst *>(I) ||
                dynamic_cast<RetInst *>(I)) {
                continue;
            }
            if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
                if (!SharedExits.count(Phi->getParent())) return false;
                continue;
            }
            if (!isAlwaysDefinedIntegerInstruction(I)) return false;
        }
    }
    bool HasExternalCall = false;
    for (CallInst *Call : F.getDirectCallSites()) {
        if (!Call || !Call->getParentFunction()) return false;
        if (Call->getParentFunction() != &F) HasExternalCall = true;
        for (unsigned Index = 0; Index < Call->getArgCount(); ++Index) {
            if (!Call->getArg(Index) ||
                valueMayDependOnUndef(Call->getArg(Index))) {
                return false;
            }
        }
    }
    if (!HasExternalCall) return false;

    P.Target = &F;
    P.RecursiveParam = RecursiveParam;
    P.RecursiveArgIndex = RecursiveIndex;
    P.SelfCall = SelfCall;
    P.Entry = F.front();
    P.SecondTest = SecondTest;
    P.RecursiveBlock = RecursiveBlock;
    P.HalfSlice = std::move(HalfSlice);
    P.SelectorSlice = std::move(Selector);
    P.CommonSlice = std::move(Common);
    P.BaseZero = std::move(BaseZero);
    P.BaseOne = std::move(BaseOne);
    P.TrueArm = std::move(TrueArm);
    P.FalseArm = std::move(FalseArm);
    return true;
}

// 按克隆映射取得新值，禁止新 CFG 继续引用旧函数体中的指令。
Value *RecursiveHalvingReplayPass::mapClonedValue(
    Value *V, const std::unordered_map<Value *, Value *> &VMap,
    Function *OldFunction) {
    if (!V) return nullptr;
    auto It = VMap.find(V);
    if (It != VMap.end()) return It->second;
    if (auto *I = dynamic_cast<Instruction *>(V)) {
        BasicBlock *BB = I->getParent();
        if (BB && BB->getParent() == OldFunction) return nullptr;
    }
    return V;
}

// 按原顺序克隆一段整数计算，并在每步拒绝遗留的旧体引用。
bool RecursiveHalvingReplayPass::cloneSlice(
    const std::vector<Instruction *> &Slice, BasicBlock *Destination,
    std::unordered_map<Value *, Value *> &VMap, Function *OldFunction) {
    if (!Destination || !OldFunction) return false;
    for (Instruction *OldI : Slice) {
        if (!OldI) return false;
        Instruction *NewI = OldI->clone(VMap);
        if (!NewI) return false;
        bool Valid = true;
        for (std::size_t Index = 0; Index < NewI->getNumOperands(); ++Index) {
            auto *OperandI = dynamic_cast<Instruction *>(NewI->getOperand(Index));
            BasicBlock *OperandBB = OperandI ? OperandI->getParent() : nullptr;
            if (OperandBB && OperandBB->getParent() == OldFunction) {
                Valid = false;
                break;
            }
        }
        if (!Valid) {
            NewI->clearOperands();
            delete NewI;
            return false;
        }
        Destination->pushBack(NewI);
        VMap[OldI] = NewI;
    }
    return true;
}

// 回滚尚未接入函数的整组新块，先断开全部 use 以避免析构级联。
void RecursiveHalvingReplayPass::rollbackBlocks(
    std::vector<BasicBlock *> &Blocks) {
    for (BasicBlock *BB : Blocks) {
        if (!BB) continue;
        for (Instruction *I : *BB)
            if (I) I->clearOperands();
    }
    for (BasicBlock *BB : Blocks) {
        if (!BB) continue;
        BB->clear();
        delete BB;
    }
    Blocks.clear();
}

// 在完全独立的新 CFG 中建立下降和回放循环，验证成功后一次性替换旧函数体。
bool RecursiveHalvingReplayPass::buildReplacement(
    Function &F, const Plan &P) {
    if (P.Target != &F || !P.RecursiveParam || !P.SelfCall) return false;
    std::vector<BasicBlock *> Blocks;
    Blocks.reserve(12);
    for (unsigned Index = 0; Index < 12; ++Index)
        Blocks.push_back(new BasicBlock());
    BasicBlock *NewEntry = Blocks[0];
    BasicBlock *DescZero = Blocks[1];
    BasicBlock *DescOne = Blocks[2];
    BasicBlock *Push = Blocks[3];
    BasicBlock *BaseZero = Blocks[4];
    BasicBlock *BaseOne = Blocks[5];
    BasicBlock *Replay = Blocks[6];
    BasicBlock *ReplayBody = Blocks[7];
    BasicBlock *TrueArm = Blocks[8];
    BasicBlock *FalseArm = Blocks[9];
    BasicBlock *Latch = Blocks[10];
    BasicBlock *Exit = Blocks[11];
    auto Fail = [&]() {
        rollbackBlocks(Blocks);
        return false;
    };

    auto I32 = INTType::getInt32Ty();
    Value *Zero32 = ConstInt::newConstIntForType(0, I32);
    Value *One32 = ConstInt::newConstIntForType(1, I32);

    NewEntry->pushBack(new BrInst(DescZero));
    auto *Current = new PhiInst(I32);
    auto *DecisionBits = new PhiInst(I32);
    auto *Depth = new PhiInst(I32);
    DescZero->pushBack(Current);
    DescZero->pushBack(DecisionBits);
    DescZero->pushBack(Depth);
    auto *IsZero = new BinaryInst(Current, BinaryInst::E, Zero32);
    DescZero->pushBack(IsZero);
    DescZero->pushBack(new BrInst(IsZero, BaseZero, DescOne));
    auto *IsOne = new BinaryInst(Current, BinaryInst::E, One32);
    DescOne->pushBack(IsOne);
    DescOne->pushBack(new BrInst(IsOne, BaseOne, Push));
    Current->addIncoming(P.RecursiveParam, NewEntry);
    DecisionBits->addIncoming(Zero32, NewEntry);
    Depth->addIncoming(Zero32, NewEntry);

    std::unordered_map<Value *, Value *> DescMap;
    DescMap[P.RecursiveParam] = Current;
    if (!cloneSlice(P.HalfSlice, Push, DescMap, &F) ||
        !cloneSlice(P.SelectorSlice, Push, DescMap, &F)) {
        return Fail();
    }
    Value *NextCurrent = mapClonedValue(
        P.SelfCall->getArg(P.RecursiveArgIndex), DescMap, &F);
    auto *OldContinuationBranch = P.RecursiveBlock->getBranchTerminator();
    Value *Take = mapClonedValue(
        OldContinuationBranch ? OldContinuationBranch->getCondition() : nullptr,
        DescMap, &F);
    if (!NextCurrent || !Take || !Take->getIRType() ||
        !Take->getIRType()->isBool()) {
        return Fail();
    }
    auto *TakeWide = new ZextInst(Take, I32);
    auto *ShiftedPath =
        new BinaryInst(DecisionBits, BinaryInst::SHL, One32);
    auto *NextPath = new BinaryInst(ShiftedPath, BinaryInst::OR, TakeWide);
    auto *NextDepth = new BinaryInst(Depth, BinaryInst::ADD, One32);
    Push->pushBack(TakeWide);
    Push->pushBack(ShiftedPath);
    Push->pushBack(NextPath);
    Push->pushBack(NextDepth);
    Push->pushBack(new BrInst(DescZero));
    Current->addIncoming(NextCurrent, Push);
    DecisionBits->addIncoming(NextPath, Push);
    Depth->addIncoming(NextDepth, Push);

    std::unordered_map<Value *, Value *> BaseZeroMap;
    BaseZeroMap[P.RecursiveParam] = Current;
    if (!cloneSlice(P.BaseZero.Slice, BaseZero, BaseZeroMap, &F))
        return Fail();
    Value *BaseZeroResult = mapClonedValue(
        P.BaseZero.Result, BaseZeroMap, &F);
    if (!BaseZeroResult) return Fail();
    BaseZero->pushBack(new BrInst(Replay));

    std::unordered_map<Value *, Value *> BaseOneMap;
    BaseOneMap[P.RecursiveParam] = Current;
    if (!cloneSlice(P.BaseOne.Slice, BaseOne, BaseOneMap, &F))
        return Fail();
    Value *BaseOneResult = mapClonedValue(P.BaseOne.Result, BaseOneMap, &F);
    if (!BaseOneResult) return Fail();
    BaseOne->pushBack(new BrInst(Replay));

    auto *ReplayResult = new PhiInst(I32);
    auto *ReplayBits = new PhiInst(I32);
    auto *ReplayDepth = new PhiInst(I32);
    Replay->pushBack(ReplayResult);
    Replay->pushBack(ReplayBits);
    Replay->pushBack(ReplayDepth);
    auto *Done = new BinaryInst(ReplayDepth, BinaryInst::E, Zero32);
    Replay->pushBack(Done);
    Replay->pushBack(new BrInst(Done, Exit, ReplayBody));

    std::unordered_map<Value *, Value *> CommonMap;
    CommonMap[P.SelfCall] = ReplayResult;
    if (!cloneSlice(P.CommonSlice, ReplayBody, CommonMap, &F))
        return Fail();
    auto *LowBit = new BinaryInst(ReplayBits, BinaryInst::AND, One32);
    auto *ReplayTake = new BinaryInst(LowBit, BinaryInst::NE, Zero32);
    auto *PoppedPath = new BinaryInst(ReplayBits, BinaryInst::LSHR, One32);
    auto *PoppedDepth =
        new BinaryInst(ReplayDepth, BinaryInst::SUB, One32);
    ReplayBody->pushBack(LowBit);
    ReplayBody->pushBack(ReplayTake);
    ReplayBody->pushBack(PoppedPath);
    ReplayBody->pushBack(PoppedDepth);
    ReplayBody->pushBack(new BrInst(ReplayTake, TrueArm, FalseArm));

    std::unordered_map<Value *, Value *> TrueMap = CommonMap;
    if (!cloneSlice(P.TrueArm.Slice, TrueArm, TrueMap, &F)) return Fail();
    Value *TrueResult = mapClonedValue(P.TrueArm.Result, TrueMap, &F);
    if (!TrueResult) return Fail();
    TrueArm->pushBack(new BrInst(Latch));

    std::unordered_map<Value *, Value *> FalseMap = CommonMap;
    if (!cloneSlice(P.FalseArm.Slice, FalseArm, FalseMap, &F)) return Fail();
    Value *FalseResult = mapClonedValue(P.FalseArm.Result, FalseMap, &F);
    if (!FalseResult) return Fail();
    FalseArm->pushBack(new BrInst(Latch));

    auto *NextResult = new PhiInst(I32);
    Latch->pushBack(NextResult);
    NextResult->addIncoming(TrueResult, TrueArm);
    NextResult->addIncoming(FalseResult, FalseArm);
    Latch->pushBack(new BrInst(Replay));

    ReplayResult->addIncoming(BaseZeroResult, BaseZero);
    ReplayResult->addIncoming(BaseOneResult, BaseOne);
    ReplayResult->addIncoming(NextResult, Latch);
    ReplayBits->addIncoming(DecisionBits, BaseZero);
    ReplayBits->addIncoming(DecisionBits, BaseOne);
    ReplayBits->addIncoming(PoppedPath, Latch);
    ReplayDepth->addIncoming(Depth, BaseZero);
    ReplayDepth->addIncoming(Depth, BaseOne);
    ReplayDepth->addIncoming(PoppedDepth, Latch);
    Exit->pushBack(new RetInst(ReplayResult));

    for (BasicBlock *BB : Blocks) {
        if (!BB || !BB->getTerminator()) return Fail();
        for (Instruction *I : *BB) {
            for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
                Value *Operand = I->getOperand(Index);
                if (auto *OldI = dynamic_cast<Instruction *>(Operand)) {
                    BasicBlock *OldBB = OldI->getParent();
                    if (OldBB && OldBB->getParent() == &F) return Fail();
                }
                if (auto *OldBB = dynamic_cast<BasicBlock *>(Operand)) {
                    if (OldBB->getParent() == &F) return Fail();
                }
            }
        }
    }

    auto OldBlocks = snapshotPointers(F);
    F.pushFront(NewEntry);
    for (std::size_t Index = 1; Index < Blocks.size(); ++Index)
        F.pushBlock(Blocks[Index]);
    Blocks.clear();
    for (BasicBlock *BB : OldBlocks) {
        if (!BB) continue;
        for (Instruction *I : *BB)
            if (I) I->clearOperands();
    }
    for (BasicBlock *BB : OldBlocks) {
        if (!BB) continue;
        BB->clear();
        delete BB;
    }
    return true;
}

// 晚期单次运行：未满足全部结构与安全门时保持函数完全不变。
PreservationStatus RecursiveHalvingReplayPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    (void)FAM;
    Plan P;
    if (!collectPlan(F, P) || !buildReplacement(F, P))
        return PreservationStatus::all();
    return PreservationStatus::none();
}
