#include "ir/opt/GVNPRE.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "lib/TraverseTools.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <tuple>
#include <utility>

bool ExprKey::operator==(const ExprKey &RHS) const {
    return Opcode == RHS.Opcode && Args == RHS.Args;
}

std::size_t ExprKeyHash::operator()(const ExprKey &K) const {
    std::size_t H = static_cast<std::size_t>(K.Opcode);
    for (int V : K.Args) {
        auto X = static_cast<std::size_t>(static_cast<std::uint32_t>(V));
        H ^= X + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
    }
    return H;
}

bool PhiTranslateKey::operator==(const PhiTranslateKey &RHS) const {
    return Num == RHS.Num && From == RHS.From && ToPred == RHS.ToPred;
}

std::size_t PhiTranslateKeyHash::operator()(const PhiTranslateKey &K) const {
    auto A = static_cast<std::size_t>(K.Num);
    auto B = reinterpret_cast<std::uintptr_t>(K.From);
    auto C = reinterpret_cast<std::uintptr_t>(K.ToPred);
    std::size_t H = A;
    H ^= B + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
    H ^= C + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
    return H;
}

GVNPRESolver::GVNPRESolver(Function &F,
                           FunctionAnalysisPassManager &FAM,
                           DominatorTree &DT,
                           LoopInfo &LI,
                           BasicAAResult &AA,
                           bool EnablePRE)
    : F(F), FAM(FAM), DT(DT), LI(LI), AA(AA), EnablePRE(EnablePRE) {}

bool GVNPRESolver::run() {
    if (isFunctionTooLargeForGVNPRE()) {
        return false;
    }

    bool AnyChanged = false;
    AnyChanged |= runGVNFixedPoint();
    if (EnablePRE) {
        AnyChanged |= runPREFixedPoint();
    }
    return AnyChanged;
}

bool GVNPRESolver::isFunctionTooLargeForGVNPRE() const {
    int Total = 0;
    for (BasicBlock *BB : F) {
        if (!BB) continue;
        int BlockCount = 0;
        for (Instruction *I : *BB) {
            (void)I;
            ++BlockCount;
            ++Total;
            if (BlockCount > kMaxBlockInsts || Total > kMaxFunctionInsts) {
                return true;
            }
        }
    }
    return false;
}

bool GVNPRESolver::runGVNFixedPoint() {
    bool AnyChanged = false;
    for (int Round = 0; Round < kMaxGVNRounds; ++Round) {
        clearIterationState();
        rebuildRPOAndReachability();
        bool Changed = runGVN();
        AnyChanged |= Changed;
        if (!Changed) {
            break;
        }
    }
    return AnyChanged;
}

bool GVNPRESolver::runPREFixedPoint() {
    bool AnyChanged = false;
    for (int Round = 0; Round < kMaxPRERounds; ++Round) {
        clearIterationState();
        rebuildRPOAndReachability();
        buildTablesForPRE();
        buildAvailOut();
        bool Changed = runPRE();
        AnyChanged |= Changed;
        if (!Changed) {
            break;
        }
    }
    return AnyChanged;
}

void GVNPRESolver::clearIterationState() {
    NextValueNum = 1;
    ValueNum.clear();
    ExpressionTable.clear();
    LeaderTable.clear();
    NumToExpr.clear();
    NumToValue.clear();
    DeadBlocks.clear();
    PhiTranslateTable.clear();
    AvailOut.clear();
}

bool GVNPRESolver::hasResultValue(Instruction *I) {
    return I && I->getIRType() && I->getIRType()->getEnum() != IR_VOID;
}

void GVNPRESolver::rebuildRPOAndReachability() {
    ReachableBlocks.clear();
    RPO.clear();

    BasicBlock *Entry = F.front();
    if (!Entry) return;

    F.collectReachableBlocks(ReachableBlocks);
    std::vector<BasicBlock *> Roots{Entry};
    RPO = TraverseTools::buildRPOFromRoots<BasicBlock>(Roots);
}

void GVNPRESolver::buildTablesForPRE() {
    for (BasicBlock *BB : RPO) {
        if (isDeadBlock(BB)) continue;

        for (Instruction *I : *BB) {
            if (!I) continue;

            processBranchForDeadBlocks(I);

            if (!hasResultValue(I)) continue;
            bool Fresh = false;
            int Num = getOrAssignValueNum(I, Fresh);
            if (Num > 0) {
                addRecord(Num, I, BB);
            }
        }
    }
}

// Build AVAIL_OUT: top-down dominator tree traversal.
// Each block inherits its dominator's AVAIL_OUT, then adds its own defs.
void GVNPRESolver::buildAvailOut() {
    BasicBlock *Entry = F.front();
    if (!Entry) return;

    // Collect domtree pre-order (entry first, then children)
    // Use iterative DFS on domtree
    std::vector<BasicBlock *> DomOrder;
    {
        std::vector<BasicBlock *> Stack{Entry};
        while (!Stack.empty()) {
            BasicBlock *BB = Stack.back();
            Stack.pop_back();
            if (!BB || isDeadBlock(BB)) continue;
            DomOrder.push_back(BB);

            for (BasicBlock *Child : DT.getChildren(BB)) {
                if (Child && !isDeadBlock(Child)) Stack.push_back(Child);
            }
        }
    }

    for (BasicBlock *BB : DomOrder) {
        auto &Avail = AvailOut[BB];

        // Inherit from idom
        BasicBlock *IDom = DT.getIDom(BB);
        if (IDom && !isDeadBlock(IDom)) {
            auto It = AvailOut.find(IDom);
            if (It != AvailOut.end()) {
                Avail = It->second; // copy idom's AVAIL_OUT
            }
        }

        // Add/override with definitions in BB
        for (Instruction *I : *BB) {
            if (!I || !hasResultValue(I)) continue;
            auto VIt = ValueNum.find(I);
            if (VIt == ValueNum.end()) continue;
            int Num = VIt->second;
            if (Num <= 0) continue;
            // Only insert if not already available (leader wins by domtree order)
            // Since we process top-down, the first definition dominates all uses.
            // Skip ALLOCA/BR/RET as they don't produce useful value leaders.
            if (I->instType == Instruction::ALLOCA ||
                I->instType == Instruction::RET   ||
                I->instType == Instruction::BR)
                continue;
            auto &Entry2 = Avail[Num];
            if (!Entry2.Inst) {
                Entry2 = {I, BB};
            }
        }
    }
}

bool GVNPRESolver::isDeadBlock(BasicBlock *BB) const {
    return !BB || DeadBlocks.count(BB) || ReachableBlocks.count(BB) == 0;
}

void GVNPRESolver::addRecord(int Num, Instruction *I, BasicBlock *BB) {
    if (!I || !BB || Num <= 0) return;
    auto &Vec = LeaderTable[Num];
    for (const auto &R : Vec) {
        if (R.Inst == I) return;
    }
    Vec.push_back({Num, I, BB});
    NumToValue[Num] = I;
}

void GVNPRESolver::forgetInstruction(Instruction *I) {
    if (!I) return;

    auto VNIt = ValueNum.find(I);
    int Num = (VNIt == ValueNum.end()) ? 0 : VNIt->second;

    if (VNIt != ValueNum.end()) {
        ValueNum.erase(VNIt);
    }

    if (Num <= 0) return;

    auto LTIt = LeaderTable.find(Num);
    if (LTIt != LeaderTable.end()) {
        auto &Vec = LTIt->second;
        Vec.erase(std::remove_if(Vec.begin(), Vec.end(),
                                 [&](const InstRecord &R) { return R.Inst == I; }),
                  Vec.end());
        if (Vec.empty()) LeaderTable.erase(LTIt);
    }

    auto NVIt = NumToValue.find(Num);
    if (NVIt != NumToValue.end() && NVIt->second == I) {
        NumToValue.erase(NVIt);
    }
}

bool GVNPRESolver::isBeforeInSameBlock(Instruction *A, Instruction *B) const {
    if (!A || !B) return false;
    BasicBlock *BB = A->getParent();
    if (!BB || BB != B->getParent()) return false;
    for (Instruction *I : *BB) {
        if (I == A) return true;
        if (I == B) return false;
    }
    return false;
}

Instruction *GVNPRESolver::findLeader(int Num, BasicBlock *UseBB, Instruction *UseI) const {
    if (!UseBB) return nullptr;

    auto It = LeaderTable.find(Num);
    if (It == LeaderTable.end()) return nullptr;

    Instruction *Best = nullptr;
    for (const InstRecord &R : It->second) {
        if (!R.Inst || !R.BB || isDeadBlock(R.BB)) continue;
        if (!R.Inst || !R.Inst->isInBlock(R.BB)) continue;
        if (!DT.dominates(R.BB, UseBB)) continue;

        if (UseI && R.BB == UseBB && !isBeforeInSameBlock(R.Inst, UseI)) {
            continue;
        }
        Best = R.Inst;
        if (R.Inst->isConst()) return Best;
    }
    return Best;
}

// O(1) lookup using AVAIL_OUT (domtree-propagated).
// PredBB: the block we want the value to be available in.
Instruction *GVNPRESolver::findAvailLeader(int Num, BasicBlock *PredBB) const {
    if (!PredBB) return nullptr;
    auto AOIt = AvailOut.find(PredBB);
    if (AOIt == AvailOut.end()) return nullptr;
    auto EntIt = AOIt->second.find(Num);
    if (EntIt == AOIt->second.end()) return nullptr;
    Instruction *Inst = EntIt->second.Inst;
    if (!Inst || !Inst->isInBlock(Inst->getParent())) return nullptr;
    return Inst;
}

int GVNPRESolver::allocateFreshNum(Value *V) {
    const int N = NextValueNum++;
    ValueNum[V] = N;
    NumToValue.emplace(N, V);
    return N;
}

int GVNPRESolver::getOrAssignValueNum(Value *V, bool &IsFresh) {
    if (!V) {
        IsFresh = false;
        return 0;
    }

    auto It = ValueNum.find(V);
    if (It != ValueNum.end()) {
        IsFresh = false;
        return It->second;
    }

    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) {
        IsFresh = true;
        return allocateFreshNum(V);
    }

    if (I->instType == Instruction::LOAD || I->instType == Instruction::STORE ||
        I->instType == Instruction::PHI || I->instType == Instruction::ALLOCA ||
        I->instType == Instruction::RET || I->instType == Instruction::BR) {
        IsFresh = true;
        return allocateFreshNum(V);
    }

    if (auto *CI = dynamic_cast<CallInst *>(I)) {
        ModRefInfo MRI = getCallModRef(CI);
        if (MRI != ModRefInfo::NoModRef) {
            IsFresh = true;
            return allocateFreshNum(V);
        }
    }

    ExprKey K;
    if (!buildExpressionKey(I, K)) {
        IsFresh = true;
        return allocateFreshNum(V);
    }

    auto ExprIt = ExpressionTable.find(K);
    if (ExprIt != ExpressionTable.end()) {
        ValueNum[V] = ExprIt->second;
        NumToValue.emplace(ExprIt->second, V);
        IsFresh = false;
        return ExprIt->second;
    }

    const int NewNum = NextValueNum++;
    ExpressionTable[K] = NewNum;
    NumToExpr[NewNum] = K;
    ValueNum[V] = NewNum;
    NumToValue.emplace(NewNum, V);
    IsFresh = true;
    return NewNum;
}

bool GVNPRESolver::buildExpressionKey(Instruction *I, ExprKey &K) {
    if (!I) return false;

    auto appendOperands = [&](Instruction *Inst, std::vector<int> &Args) {
        for (size_t OperandIndex = 0;
             OperandIndex < Inst->getNumOperands(); ++OperandIndex) {
            bool Fresh = false;
            Args.push_back(getOrAssignValueNum(
                Inst->getOperand(OperandIndex), Fresh));
        }
    };

    K.Args.clear();
    K.Opcode = (static_cast<unsigned>(I->instType) << 8);

    if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
        Value *LHS = nullptr;
        Value *RHS = nullptr;
        if (!BI->getOperands(LHS, RHS)) return false;

        bool F0 = false;
        bool F1 = false;
        int A0 = getOrAssignValueNum(LHS, F0);
        int A1 = getOrAssignValueNum(RHS, F1);

        if (BI->isCommutativeOp(BI->getOp()) && A0 > A1) {
            std::swap(A0, A1);
        }

        if (BinaryInst::isCompareOp(BI->getOp())) {
            auto Pred = BI->getOp();
            if (A0 > A1) {
                std::swap(A0, A1);
                Pred = BinaryInst::reversePredicate(Pred);
            }
            K.Opcode = (static_cast<unsigned>(I->instType) << 8) |
                       static_cast<unsigned>(Pred);
        }

        K.Args.push_back(A0);
        K.Args.push_back(A1);
        return true;
    }

    if (I->instType == Instruction::GEP) {
        appendOperands(I, K.Args);
        return true;
    }

    if (I->instType == Instruction::CALL) {
        auto *CI = dynamic_cast<CallInst *>(I);
        if (!CI) return false;
        if (getCallModRef(CI) != ModRefInfo::NoModRef) {
            return false;
        }
        appendOperands(I, K.Args);
        return true;
    }

    if (I->instType == Instruction::SELECT || I->instType == Instruction::TRUNC ||
        I->instType == Instruction::ZEXT   || I->instType == Instruction::SEXT   ||
        I->instType == Instruction::FPTOSI || I->instType == Instruction::SITOFP ||
        I->instType == Instruction::BITCAST) {
        appendOperands(I, K.Args);
        return true;
    }

    return false;
}

ModRefInfo GVNPRESolver::getCallModRef(CallInst *CI) {
    if (!CI) return ModRefInfo::ModRef;

    if (auto *BIF = CI->getBuiltinCallee()) {
        FuncAttr Attr = classifyBuiltin(BIF);
        if (Attr == FuncAttr::Untracked)    return ModRefInfo::ModRef;
        if (Attr == FuncAttr::NoModRef)     return ModRefInfo::NoModRef;
        if (Attr == FuncAttr::MemReadOnly)  return ModRefInfo::Ref;
        if (Attr == FuncAttr::MemWriteOnly) return ModRefInfo::Mod;
        return ModRefInfo::ModRef;
    }

    if (auto *CalleeF = CI->getDirectCallee()) {
        const SideEffectInfo &SEI = FAM.getResult<SideEffectAnalysis>(*CalleeF);
        for (const auto &KV : SEI.ptrMap) {
            if (KV.second.untracked) return ModRefInfo::ModRef;
        }
        return getFunctionModRefInfo(SEI);
    }

    return ModRefInfo::ModRef;
}

std::vector<Value *> GVNPRESolver::collectCallReadPtrs(CallInst *CI) {
    std::vector<Value *> ReadPtrs;
    if (!CI) return ReadPtrs;

    auto pushUnique = [&](Value *V) {
        if (!V) return;
        for (Value *X : ReadPtrs) {
            if (X == V) return;
        }
        ReadPtrs.push_back(V);
    };

    std::vector<Value *> Args = CI->getArgs();

    if (auto *BIF = CI->getBuiltinCallee()) {
        if (getIntrinsicID(BIF) == IntrinsicID::Memcpy) {
            if (Args.size() >= 2 && Args[1] && Args[1]->isPointerValue()) {
                pushUnique(Args[1]);
            }
            return ReadPtrs;
        }

        FuncAttr Attr = classifyBuiltin(BIF);
        bool DoRef = (Attr == FuncAttr::MemReadOnly || Attr == FuncAttr::MemReadWrite);
        if (!DoRef) return ReadPtrs;

        for (Value *Arg : Args) {
            if (!Arg || !Arg->isPointerValue()) continue;
            pushUnique(Arg);
        }
        return ReadPtrs;
    }

    if (auto *CalleeFunc = CI->getDirectCallee()) {
        const SideEffectInfo &SEI = FAM.getResult<SideEffectAnalysis>(*CalleeFunc);

        for (Value *RV : SEI.RefSet) {
            auto *Var = dynamic_cast<Variable *>(RV);
            if (!Var) continue;

            if (Var->isGlobal()) {
                pushUnique(RV);
                continue;
            }

            if (!Var->isParam()) continue;
            int Index = CalleeFunc->getParamIndex(RV);
            if (Index >= 0 && static_cast<size_t>(Index) < Args.size() &&
                Args[static_cast<size_t>(Index)] &&
                Args[static_cast<size_t>(Index)]->isPointerValue()) {
                pushUnique(Args[static_cast<size_t>(Index)]);
            }
        }
    }

    return ReadPtrs;
}

bool GVNPRESolver::identicalCall(CallInst *A, CallInst *B) {
    if (!A || !B) return false;
    if (A->getNumOperands() != B->getNumOperands()) return false;
    for (size_t i = 0; i < A->getNumOperands(); ++i) {
        if (!A->getOperand(i) || !B->getOperand(i)) return false;
        if (A->getOperand(i) != B->getOperand(i)) return false;
    }
    return true;
}

void GVNPRESolver::markDeadBranchBlocks(BasicBlock *BrBB, BasicBlock *DeadSucc) {
    if (!BrBB || !DeadSucc || DeadSucc == BrBB) return;

    std::unordered_set<BasicBlock *> Candidate;
    std::queue<BasicBlock *> Q;
    Q.push(DeadSucc);
    Candidate.insert(DeadSucc);

    while (!Q.empty()) {
        BasicBlock *Cur = Q.front();
        Q.pop();
        for (BasicBlock *Succ : Cur->getSuccessors()) {
            if (!Succ || Candidate.count(Succ)) continue;
            Candidate.insert(Succ);
            Q.push(Succ);
        }
    }

    std::vector<BasicBlock *> ToErase;
    bool Changed = true;
    while (Changed) {
        Changed = false;
        ToErase.clear();
        for (BasicBlock *BB : Candidate) {
            bool HasExternalPred = false;
            for (BasicBlock *Pred : BB->getPredecessors()) {
                if (!Pred) continue;
                if (Pred == BrBB) continue;
                if (Candidate.count(Pred) == 0) {
                    HasExternalPred = true;
                    break;
                }
            }
            if (HasExternalPred) {
                ToErase.push_back(BB);
            }
        }
        if (!ToErase.empty()) {
            Changed = true;
            for (BasicBlock *BB : ToErase) Candidate.erase(BB);
        }
    }

    for (BasicBlock *BB : Candidate) {
        DeadBlocks.insert(BB);
    }
}

void GVNPRESolver::processBranchForDeadBlocks(Instruction *I) {
    auto *Br = dynamic_cast<BrInst *>(I);
    if (!Br || !Br->isConditional()) return;
    Value *Cond = Br->getCondition();
    bool CondValue = false;
    if (!Cond || !Cond->tryGetBoolConst(CondValue)) return;
    BasicBlock *TrueBB = Br->getTrueSuccessor();
    BasicBlock *FalseBB = Br->getFalseSuccessor();
    BasicBlock *DeadSucc = CondValue ? FalseBB : TrueBB;
    markDeadBranchBlocks(Br->getParent(), DeadSucc);
}

bool GVNPRESolver::processLoad(Instruction *I) {
    auto *LIst = dynamic_cast<LoadInst *>(I);
    if (!LIst) return false;
    Value *Ptr = LIst->getPointerOperand();
    if (!Ptr) return false;

    BasicBlock *BB = I->getParent();
    if (!BB) return false;

    BasicBlock *ScanBB = BB;
    Instruction *StartI = I->getPrev();

    int ScanCount = 0;
    while (ScanBB) {
        Instruction *Cur = (ScanBB == BB) ? StartI : ScanBB->back();

        for (; Cur != nullptr; Cur = Cur->getPrev()) {
            if (++ScanCount > kBackwardScanLimit) return false;

            if (auto *SI = dynamic_cast<StoreInst *>(Cur)) {
                Value *SPtr = SI->getPointerOperand();
                if (!SPtr) continue;
                AliasInfo AI = AA.alias(Ptr, SPtr, LI);
                if (AI == AliasInfo::MustAlias) {
                    Value *Stored = SI->getValueOperand();
                    if (!Stored) return false;
                    I->replaceAllUsesWith(Stored);
                    forgetInstruction(I);
                    delete I;
                    return true;
                }
                if (AI == AliasInfo::MayAlias) return false;
                continue;
            }

            if (auto *LI2 = dynamic_cast<LoadInst *>(Cur)) {
                Value *PrevPtr = LI2->getPointerOperand();
                if (!PrevPtr) continue;
                AliasInfo AI = AA.alias(Ptr, PrevPtr, LI);
                if (AI == AliasInfo::MustAlias) {
                    I->replaceAllUsesWith(LI2);
                    forgetInstruction(I);
                    delete I;
                    return true;
                }
                continue;
            }

            if (dynamic_cast<CallInst *>(Cur)) {
                ModRefInfo MRI = getInstModRefInfo(Ptr, Cur, AA, LI, FAM);
                if (isModSet(MRI)) return false;
            }
        }

        auto Preds = ScanBB->getPredecessors();
        if (Preds.size() != 1 || isDeadBlock(Preds[0])) break;
        ScanBB = Preds[0];
    }
    return false;
}

bool GVNPRESolver::processCall(Instruction *I) {
    auto *CI = dynamic_cast<CallInst *>(I);
    if (!CI) return false;

    if (getCallModRef(CI) != ModRefInfo::Ref) return false;

    BasicBlock *BB = I->getParent();
    if (!BB) return false;

    auto ReadPtrs = collectCallReadPtrs(CI);

    BasicBlock *ScanBB = BB;
    Instruction *StartI = I->getPrev();

    int ScanCount = 0;
    while (ScanBB) {
        Instruction *Cur = (ScanBB == BB) ? StartI : ScanBB->back();

        for (; Cur != nullptr; Cur = Cur->getPrev()) {
            if (++ScanCount > kBackwardScanLimit) return false;

            if (auto *SI = dynamic_cast<StoreInst *>(Cur)) {
                Value *SPtr = SI->getPointerOperand();
                if (!SPtr) continue;
                for (Value *RP : ReadPtrs) {
                    if (!RP || !RP->isPointerValue())
                        continue;
                    if (AA.alias(RP, SPtr, LI) != AliasInfo::NoAlias) return false;
                }
                continue;
            }

            if (auto *PrevCall = dynamic_cast<CallInst *>(Cur)) {
                if (identicalCall(CI, PrevCall)) {
                    I->replaceAllUsesWith(PrevCall);
                    forgetInstruction(I);
                    delete I;
                    return true;
                }
                for (Value *RP : ReadPtrs) {
                    if (!RP || !RP->isPointerValue())
                        continue;
                    ModRefInfo MRI = getInstModRefInfo(RP, PrevCall, AA, LI, FAM);
                    if (isModSet(MRI)) return false;
                }
            }
        }

        auto Preds = ScanBB->getPredecessors();
        if (Preds.size() != 1 || isDeadBlock(Preds[0])) break;
        ScanBB = Preds[0];
    }
    return false;
}

bool GVNPRESolver::runGVN() {
    bool Changed = false;

    for (BasicBlock *BB : RPO) {
        if (isDeadBlock(BB)) continue;

        std::vector<Instruction *> Insts;
        for (Instruction *I : *BB) Insts.push_back(I);

        for (Instruction *I : Insts) {
            if (!I || I->getParent() != BB) continue;
            if (isDeadBlock(BB)) break;

            if (I->instType == Instruction::PHI) {
                continue;
            }

            processBranchForDeadBlocks(I);

            if (I->instType == Instruction::LOAD) {
                if (processLoad(I)) {
                    Changed = true;
                    continue;
                }
            }

            if (I->instType == Instruction::CALL) {
                if (processCall(I)) {
                    Changed = true;
                    continue;
                }
            }

            if (!hasResultValue(I)) {
                continue;
            }

            bool Fresh = false;
            int Num = getOrAssignValueNum(I, Fresh);
            if (Num <= 0) continue;

            if (I->instType == Instruction::ALLOCA || I->instType == Instruction::RET ||
                I->instType == Instruction::BR     || I->instType == Instruction::PHI) {
                addRecord(Num, I, BB);
                continue;
            }

            if (Fresh) {
                addRecord(Num, I, BB);
                continue;
            }

            Instruction *Leader = findLeader(Num, BB, I);
            if (!Leader || Leader == I) {
                addRecord(Num, I, BB);
                continue;
            }

            I->replaceAllUsesWith(Leader);
            forgetInstruction(I);
            delete I;
            Changed = true;
        }
    }

    return Changed;
}

bool GVNPRESolver::isBackedge(BasicBlock *Pred, BasicBlock *BB, const DominatorTree &DT) {
    if (!Pred || !BB) return false;
    return DT.dominates(BB, Pred);
}

bool GVNPRESolver::shouldSkipPREInst(Instruction *I) {
    if (!I || !hasResultValue(I)) return true;

    if (I->instType == Instruction::ALLOCA || I->instType == Instruction::RET ||
        I->instType == Instruction::BR     || I->instType == Instruction::PHI ||
        I->instType == Instruction::LOAD   || I->instType == Instruction::STORE) {
        return true;
    }

    if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
        if (BinaryInst::isCompareOp(BI->getOp())) return true;
    }

    if (I->instType == Instruction::SDIV || I->instType == Instruction::UDIV ||
        I->instType == Instruction::FDIV || I->instType == Instruction::SREM ||
        I->instType == Instruction::UREM || I->instType == Instruction::FREM) {
        return true;
    }

    // PRE may insert an expression on a path where it did not previously
    // execute.  Even a memory-pure call may trap or fail to return, so calls
    // are only eliminated by the dominating-call GVN path and are never
    // speculated by PRE.
    if (dynamic_cast<CallInst *>(I)) return true;

    return false;
}

// Improved phiTranslate:
// 1. Check PHI nodes in FromBB for direct mapping
// 2. If Num corresponds to a compound expression, recursively translate operands
//    and check if the resulting ExprKey exists in ExpressionTable
int GVNPRESolver::phiTranslate(int Num, BasicBlock *FromBB, BasicBlock *ToPred) {
    PhiTranslateKey Key{Num, FromBB, ToPred};
    auto It = PhiTranslateTable.find(Key);
    if (It != PhiTranslateTable.end()) {
        return It->second;
    }

    int Result = Num;

    // Step 1: Look for a PHI in FromBB whose value-number matches Num,
    // and return the value-number of the incoming value from ToPred.
    if (FromBB) {
        for (Instruction *DefI : *FromBB) {
            if (!DefI) continue;
            auto *Phi = dynamic_cast<PhiInst *>(DefI);
            if (!Phi) break; // PHIs always come first
            auto VIt = ValueNum.find(DefI);
            if (VIt == ValueNum.end() || VIt->second != Num) continue;

            for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
                if (Phi->getIncomingBlock(i) == ToPred) {
                    bool Fresh = false;
                    Result = getOrAssignValueNum(Phi->getIncomingValue(i), Fresh);
                    PhiTranslateTable[Key] = Result;
                    return Result;
                }
            }
        }
    }

    // Step 2: Translate compound expression by recursively translating operands.
    // If all operands translate, look up the resulting ExprKey.
    auto EI = NumToExpr.find(Num);
    if (EI != NumToExpr.end()) {
        ExprKey NewK = EI->second;
        bool AnyChanged = false;

        for (int &ArgNum : NewK.Args) {
            int Translated = phiTranslate(ArgNum, FromBB, ToPred);
            if (Translated != ArgNum) AnyChanged = true;
            ArgNum = Translated;
        }

        // Re-canonicalize commutative ops after translation
        unsigned BaseOp = NewK.Opcode >> 8;
        if (BaseOp == Instruction::ADD  || BaseOp == Instruction::FADD ||
            BaseOp == Instruction::MUL  || BaseOp == Instruction::FMUL ||
            BaseOp == Instruction::AND  || BaseOp == Instruction::OR   ||
            BaseOp == Instruction::XOR) {
            std::sort(NewK.Args.begin(), NewK.Args.end());
        }

        if (AnyChanged) {
            auto Found = ExpressionTable.find(NewK);
            if (Found != ExpressionTable.end()) {
                Result = Found->second;
            }
            // If not found, Result stays as original Num — expression not available
        }
    }

    PhiTranslateTable[Key] = Result;
    return Result;
}

// Resolve an operand value for hoisting to PredBB.
// OrigV: original operand of instruction in SrcBB.
// If OrigV is defined in SrcBB itself, find its translated leader in PredBB.
// If OrigV is defined outside SrcBB, verify it dominates PredBB.
Value *GVNPRESolver::resolveAvailOperand(Value *OrigV, BasicBlock *SrcBB, BasicBlock *PredBB) {
    if (!OrigV) return nullptr;

    auto *OpI = dynamic_cast<Instruction *>(OrigV);
    if (!OpI) {
        // Constants, globals, params – always available
        return OrigV;
    }

    BasicBlock *DefBB = OpI->getParent();
    if (!DefBB) return nullptr;

    if (DefBB == SrcBB) {
        // Operand is defined in the same block: need its translated version in PredBB
        bool OpFresh = false;
        int OpNum    = getOrAssignValueNum(OrigV, OpFresh);
        int TransNum = phiTranslate(OpNum, SrcBB, PredBB);

        // First try fast AVAIL_OUT lookup
        if (Instruction *Avail = findAvailLeader(TransNum, PredBB)) {
            return Avail;
        }
        // Fallback: full LeaderTable scan
        if (Instruction *Leader = findLeader(TransNum, PredBB, nullptr)) {
            return Leader;
        }
        return nullptr; // Not available — hoisting blocked
    }

    // Operand comes from outside SrcBB: must dominate PredBB
    if (!DT.dominates(DefBB, PredBB)) return nullptr;
    return OrigV;
}

void GVNPRESolver::seedDeadBlocksToLeaderTable() {
    for (BasicBlock *BB : DeadBlocks) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (!I || !hasResultValue(I)) continue;
            bool Fresh = false;
            int Num = getOrAssignValueNum(I, Fresh);
            addRecord(Num, I, BB);
        }
    }
}

bool GVNPRESolver::runPRE() {
    bool Changed = false;

    seedDeadBlocksToLeaderTable();

    if (!F.front()) return false;

    // DFS pre-order from entry (mirrors domtree top-down order for PRE)
    std::unordered_set<BasicBlock *> Visited;
    std::vector<BasicBlock *> DFSOrder;
    {
        std::vector<BasicBlock *> Stack;
        Stack.push_back(F.front());
        while (!Stack.empty()) {
            BasicBlock *Cur = Stack.back();
            Stack.pop_back();
            if (!Cur || !Visited.insert(Cur).second) continue;
            DFSOrder.push_back(Cur);
            auto Succs = Cur->getSuccessors();
            for (auto It = Succs.rbegin(); It != Succs.rend(); ++It)
                if (*It) Stack.push_back(*It);
        }
    }

    for (BasicBlock *BB : DFSOrder) {
        if (!BB || BB == F.front() || isDeadBlock(BB)) continue;

        std::vector<Instruction *> Insts;
        for (Instruction *I : *BB) Insts.push_back(I);

        for (Instruction *I : Insts) {
            if (!I || I->getParent() != BB) continue;
            if (shouldSkipPREInst(I)) continue;

            bool Fresh = false;
            int Num = getOrAssignValueNum(I, Fresh);
            if (Num <= 0) continue;

            std::vector<BasicBlock *> Preds = BB->getPredecessors();
            if (Preds.empty()) continue;

            int NumWithout = 0;
            int NumWith    = 0;
            BasicBlock *PREPred = nullptr;
            bool Abort   = false;

            // PredMap: pred -> available leader value (for preds where it IS available)
            std::unordered_map<BasicBlock *, Value *> PredMap;
            std::vector<BasicBlock *> ConsideredPreds;

            for (BasicBlock *Pred : Preds) {
                if (!Pred || isDeadBlock(Pred)) {
                    Abort = true;
                    break;
                }
                if (isBackedge(Pred, BB, DT)) {
                    Abort = true;
                    break;
                }

                ConsideredPreds.push_back(Pred);

                int TransNum = phiTranslate(Num, BB, Pred);

                // Fast AVAIL_OUT lookup first, then fallback to full scan
                Instruction *Leader = findAvailLeader(TransNum, Pred);
                if (!Leader) {
                    Leader = findLeader(TransNum, Pred, nullptr);
                }

                if (!Leader) {
                    ++NumWithout;
                    PREPred = Pred;
                    continue;
                }
                if (Leader == I) {
                    // This is a back-edge scenario or same instruction: abort
                    NumWithout = 2;
                    Abort      = true;
                    break;
                }

                ++NumWith;
                PredMap[Pred] = Leader;
            }

            if (Abort) continue;

            // PRE condition: exactly one pred missing, at least one has it
            if (!(NumWithout == 1 && NumWith > 0 && PREPred)) {
                continue;
            }

            // Clone the instruction and fix operands for PREPred
            Instruction *Cloned = I->clone({});
            if (!Cloned) continue;

            bool OperandOK = true;
            if (I->getNumOperands() != Cloned->getNumOperands()) {
                delete Cloned;
                continue;
            }

            for (size_t Idx = 0;
                 OperandOK && Idx < I->getNumOperands(); ++Idx) {
                Value *OrigV = I->getOperand(Idx);
                if (!OrigV) {
                    OperandOK = false;
                    break;
                }

                // Use improved resolveAvailOperand to find the right value in PREPred
                Value *Resolved = resolveAvailOperand(OrigV, BB, PREPred);
                if (!Resolved) {
                    OperandOK = false;
                    break;
                }

                if (Cloned->getOperand(Idx) != Resolved)
                    Cloned->replaceOperand(Idx, Resolved);
            }

            if (!OperandOK) {
                delete Cloned;
                continue;
            }

            // Insert cloned instruction into PREPred before its terminator
            PREPred->insertBeforeTerminator(Cloned);
            ValueNum[Cloned] = Num;
            NumToValue[Num]  = Cloned;
            addRecord(Num, Cloned, PREPred);

            // Update AVAIL_OUT[PREPred] so future PRE rounds see this new def
            AvailOut[PREPred][Num] = {Cloned, PREPred};

            // Create PHI at BB to merge
            auto *NewPhi = new PhiInst(I->getIRType());
            BB->insertAfterPhiNodes(NewPhi);

            for (BasicBlock *Pred : ConsideredPreds) {
                auto It = PredMap.find(Pred);
                if (It != PredMap.end()) {
                    NewPhi->addIncoming(It->second, Pred);
                } else if (Pred == PREPred) {
                    NewPhi->addIncoming(Cloned, Pred);
                }
            }

            // Give PHI the same num; update leader tables and AVAIL_OUT[BB]
            ValueNum[NewPhi] = Num;
            NumToValue[Num]  = NewPhi;
            addRecord(Num, NewPhi, BB);
            AvailOut[BB][Num] = {NewPhi, BB};

            // Propagate updated availability to domtree children of BB
            // (simple: invalidate their AVAIL_OUT entry so next lookup falls back)
            for (BasicBlock *Cand : RPO) {
                if (!Cand || Cand == BB || isDeadBlock(Cand)) continue;
                if (DT.dominates(BB, Cand)) {
                    auto AOIt = AvailOut.find(Cand);
                    if (AOIt != AvailOut.end()) {
                        auto EntIt = AOIt->second.find(Num);
                        if (EntIt == AOIt->second.end()) {
                            AOIt->second[Num] = {NewPhi, BB};
                        }
                    }
                }
            }

            // Invalidate phiTranslate cache entries involving BB (they may be stale)
            // Selective: remove keys where FromBB == BB
            for (auto It = PhiTranslateTable.begin(); It != PhiTranslateTable.end(); ) {
                if (It->first.From == BB)
                    It = PhiTranslateTable.erase(It);
                else
                    ++It;
            }

            I->replaceAllUsesWith(NewPhi);
            forgetInstruction(I);
            delete I;
            Changed = true;
        }
    }

    return Changed;
}

void GVNPRESolver::domTreeDFS(BasicBlock *BB,
                              std::function<void(BasicBlock *)> Visit) {
    if (!BB) return;
    Visit(BB);
    for (BasicBlock *Child : DT.getChildren(BB)) {
        if (Child) domTreeDFS(Child, Visit);
    }
}

PreservationStatus GVNPREPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);

    GVNPRESolver Solver(F, FAM, DT, LI, AA, EnablePRE);
    bool Changed = Solver.run();

    if (!Changed) {
        return PreservationStatus::all();
    }

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}

namespace {

struct LocalExprKey {
    unsigned Opcode = 0;
    unsigned SubOpcode = 0;
    const IRType *ResultType = nullptr;
    std::vector<Value *> Operands;

    bool operator==(const LocalExprKey &Other) const {
        return Opcode == Other.Opcode && SubOpcode == Other.SubOpcode &&
               ResultType == Other.ResultType && Operands == Other.Operands;
    }
};

struct LocalExprKeyHash {
    std::size_t operator()(const LocalExprKey &K) const {
        std::size_t H = static_cast<std::size_t>(K.Opcode);
        auto mix = [&](std::size_t V) {
            H ^= V + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
        };
        mix(static_cast<std::size_t>(K.SubOpcode));
        mix(reinterpret_cast<std::uintptr_t>(K.ResultType));
        for (Value *V : K.Operands)
            mix(reinterpret_cast<std::uintptr_t>(V));
        return H;
    }
};

bool buildLocalExprKey(Instruction *I, LocalExprKey &K) {
    if (!I || !I->getIRType() || I->getIRType()->getEnum() == IR_VOID)
        return false;

    K = LocalExprKey{};
    K.Opcode = static_cast<unsigned>(I->instType);
    K.ResultType = I->getIRType().get();

    if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
        Value *LHS = nullptr;
        Value *RHS = nullptr;
        if (!BinaryInst::getOperands(BI, LHS, RHS) || !LHS || !RHS ||
            Value::isUndef(LHS) || Value::isUndef(RHS)) {
            return false;
        }
        K.SubOpcode = static_cast<unsigned>(BI->getOp());
        K.Operands = {LHS, RHS};
        if (BinaryInst::isCommutativeOp(BI->getOp()) &&
            std::less<Value *>{}(K.Operands[1], K.Operands[0])) {
            std::swap(K.Operands[0], K.Operands[1]);
        }
        return true;
    }

    switch (I->instType) {
    case Instruction::GEP:
    case Instruction::SELECT:
    case Instruction::TRUNC:
    case Instruction::ZEXT:
    case Instruction::SEXT:
    case Instruction::FPTOSI:
    case Instruction::SITOFP:
    case Instruction::BITCAST:
        break;
    default:
        return false;
    }

    K.Operands.reserve(I->getNumOperands());
    for (size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (!Operand || Value::isUndef(Operand)) return false;
        K.Operands.push_back(Operand);
    }
    return !K.Operands.empty();
}

} // namespace

PreservationStatus LocalGVNPass::run(Function &F,
                                     FunctionAnalysisPassManager &) {
    bool Changed = false;

    for (BasicBlock *BB : F) {
        if (!BB) continue;
        std::unordered_map<LocalExprKey, Instruction *, LocalExprKeyHash> Leaders;

        std::vector<Instruction *> Insts;
        for (Instruction *I : *BB) Insts.push_back(I);

        for (Instruction *I : Insts) {
            if (!I || I->getParent() != BB) continue;
            LocalExprKey Key;
            if (!buildLocalExprKey(I, Key)) continue;

            auto It = Leaders.find(Key);
            if (It == Leaders.end()) {
                Leaders.emplace(std::move(Key), I);
                continue;
            }

            Instruction *Leader = It->second;
            if (!Leader || Leader->getParent() != BB || Leader == I) {
                It->second = I;
                continue;
            }

            I->replaceAllUsesWith(Leader);
            delete I;
            Changed = true;
        }
    }

    if (!Changed) return PreservationStatus::all();
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
