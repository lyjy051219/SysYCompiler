#include "ir/opt/GVNPRE.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <tuple>
#include <utility>
//构造并设置是否启用部分冗余消除
GVNPREPass::GVNPREPass(bool EnablePRE) : EnablePRE(EnablePRE) {}
//判断两个全局表达式键是否完全相同
bool ExprKey::operator==(const ExprKey &RHS) const {
    return Opcode == RHS.Opcode && ResultType == RHS.ResultType &&
           Args == RHS.Args;
}
//计算给定全局表达式键的哈希值
std::size_t ExprKeyHash::operator()(const ExprKey &K) const {
    std::size_t H = static_cast<std::size_t>(K.Opcode);
    const auto TypeHash = reinterpret_cast<std::uintptr_t>(K.ResultType);
    H ^= TypeHash + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
    for (int V : K.Args) {
        auto X = static_cast<std::size_t>(static_cast<std::uint32_t>(V));
        H ^= X + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
    }
    return H;
}
//判断两个PHI翻译查询键是否完全相同
bool PhiTranslateKey::operator==(const PhiTranslateKey &RHS) const {
    return Num == RHS.Num && From == RHS.From && ToPred == RHS.ToPred;
}
//计算给定PHI翻译查询键的哈希值
std::size_t PhiTranslateKeyHash::operator()(const PhiTranslateKey &K) const {
    auto A = static_cast<std::size_t>(K.Num);
    auto B = reinterpret_cast<std::uintptr_t>(K.From);
    auto C = reinterpret_cast<std::uintptr_t>(K.ToPred);
    std::size_t H = A;
    H ^= B + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
    H ^= C + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
    return H;
}
//判断两个规范化值对是否完全相同
bool PhiCongruenceMatcher::ValuePair::operator==(
    const ValuePair &Other) const {
    return A == Other.A && B == Other.B;
}
//计算给定规范化值对的哈希值
std::size_t PhiCongruenceMatcher::ValuePairHash::operator()(
    const ValuePair &P) const {
    std::size_t H = reinterpret_cast<std::uintptr_t>(P.A);
    const auto R = reinterpret_cast<std::uintptr_t>(P.B);
    H ^= R + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
    return H;
}
//判断两个IR值是否在支持的纯表达式图中同余
bool PhiCongruenceMatcher::equivalent(Value *A, Value *B) {
    return equivalentImpl(A, B, 0);
}
//按照稳定指针顺序规范化两个待比较值
PhiCongruenceMatcher::ValuePair PhiCongruenceMatcher::canonicalPair(
    Value *A, Value *B) {
    if (std::less<Value *>{}(B, A)) std::swap(A, B);
    return {A, B};
}
//判断指令是否属于同余匹配支持的纯表达式集合
bool PhiCongruenceMatcher::isSupportedPureInstruction(Instruction *I) {
    if (!I) return false;
    if (dynamic_cast<BinaryInst *>(I) || dynamic_cast<PhiInst *>(I))
        return true;
    switch (I->instType) {
    case Instruction::GEP:
    case Instruction::SELECT:
    case Instruction::TRUNC:
    case Instruction::ZEXT:
    case Instruction::SEXT:
    case Instruction::FPTOSI:
    case Instruction::SITOFP:
    case Instruction::BITCAST:
        return true;
    default:
        return false;
    }
}
//递归比较两个PHI的入边及对应操作数
bool PhiCongruenceMatcher::equivalentPhis(
    PhiInst *A, PhiInst *B, unsigned Depth) {
    //2.1 同块 PHI 按前驱而非 incoming 顺序配对，每个前驱必须恰出现一次；
    //对应值再递归比较，避免因边枚举顺序不同漏掉等价关系。
    if (!A || !B || A->getParent() != B->getParent() ||
        A->getNumIncomingValues() != B->getNumIncomingValues()) {
        return false;
    }
    std::unordered_set<BasicBlock *> SeenA;
    std::unordered_set<BasicBlock *> SeenB;
    bool Equal = forEachPhiIncoming(
        A, [&](unsigned, Value *Incoming, BasicBlock *Pred) {
        if (!Pred || !SeenA.insert(Pred).second) return false;
        Value *OtherIncoming = nullptr;
        unsigned Matches = 0;
        forEachPhiIncoming(
            B, [&](unsigned, Value *Candidate, BasicBlock *CandidatePred) {
                if (CandidatePred != Pred) return;
                OtherIncoming = Candidate;
                ++Matches;
            });
        if (Matches != 1 || !SeenB.insert(Pred).second ||
            !equivalentImpl(Incoming, OtherIncoming, Depth + 1)) {
            return false;
        }
        return true;
    });
    return Equal && SeenA.size() == SeenB.size();
}
//递归比较两个IR值并维护三态缓存和深度预算
bool PhiCongruenceMatcher::equivalentImpl(
    Value *A, Value *B, unsigned Depth) {
    //2.2 仅递归比较无 undef 的纯表达式。值对用三态 memo 处理循环 PHI：正在访问
    //的同一对暂按共递归相等，最终由外层操作码、类型和全部操作数共同定论。
    if (A == B) return A && !Value::isUndef(A);
    if (!A || !B || Value::isUndef(A) || Value::isUndef(B) ||
        Depth > 128 || A->getIRType().get() != B->getIRType().get()) {
        return false;
    }
    auto *IA = dynamic_cast<Instruction *>(A);
    auto *IB = dynamic_cast<Instruction *>(B);
    if (!IA || !IB || IA->instType != IB->instType ||
        !isSupportedPureInstruction(IA) ||
        !isSupportedPureInstruction(IB)) {
        return false;
    }
    ValuePair Pair = canonicalPair(A, B);
    auto It = States.find(Pair);
    if (It != States.end()) {
        if (It->second == State::Visiting || It->second == State::Equal)
            return true;
        return false;
    }
    States.emplace(Pair, State::Visiting);
    bool Equal = false;
    if (auto *PA = dynamic_cast<PhiInst *>(IA)) {
        Equal = equivalentPhis(PA, dynamic_cast<PhiInst *>(IB), Depth);
    } else if (auto *BA = dynamic_cast<BinaryInst *>(IA)) {
        auto *BB = dynamic_cast<BinaryInst *>(IB);
        Value *AL = nullptr;
        Value *AR = nullptr;
        Value *BL = nullptr;
        Value *BR = nullptr;
        Equal = BB && BA->getOp() == BB->getOp() &&
                BinaryInst::getOperands(BA, AL, AR) &&
                BinaryInst::getOperands(BB, BL, BR) &&
                equivalentImpl(AL, BL, Depth + 1) &&
                equivalentImpl(AR, BR, Depth + 1);
    } else if (IA->getNumOperands() == IB->getNumOperands()) {
        Equal = forEachOperand(
            IA, [&](size_t Index, Use *, Value *Operand) {
                return equivalentImpl(
                    Operand, IB->getOperand(Index), Depth + 1);
            });
    }
    States[Pair] = Equal ? State::Equal : State::Different;
    return Equal;
}
//构造绑定函数及全部依赖分析的GVN/PRE求解器
GVNPRESolver::GVNPRESolver(Function &F,
                           FunctionAnalysisPassManager &FAM,
                           DominatorTree &DT,
                           LoopInfo &LI,
                           BasicAAResult &AA,
                           MemorySSAResult &MSSA,
                           bool EnablePRE)
    : F(F), FAM(FAM), DT(DT), LI(LI), AA(AA), MSSA(MSSA),
      EnablePRE(EnablePRE) {}
//运行有界GVN固定点及可选PRE固定点并返回IR是否变化
bool GVNPRESolver::run() {
    //1. 先应用函数/基本块规模预算；预算内先让 GVN 收敛，再按配置运行 PRE 收敛。
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
//判断函数规模是否超过GVN/PRE编译时间预算
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
//有界迭代GVN直到不动点或达到轮数预算
bool GVNPRESolver::runGVNFixedPoint() {
    //2. 每轮重建值号与可达性，直到没有冗余被删除或达到轮数上限；删除会暴露
    //新支配 leader，因此不能只扫描一次。
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
//有界迭代PRE直到不动点或达到轮数预算
bool GVNPRESolver::runPREFixedPoint() {
    //3. PRE 每轮先重建值号表和支配可用集，再补一条缺失路径；新表达式/PHI 会
    //改变后续可用性，因此同样有界迭代。
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
//清空一轮值编号和内存可用性求解状态
void GVNPRESolver::clearIterationState() {
    //1.1 清空所有派生表与查询预算，确保新一轮不使用被上一轮删除指令的记录。
    NextValueNum = 1;
    ValueNum.clear();
    ExpressionTable.clear();
    LeaderTable.clear();
    NumToExpr.clear();
    NumToValue.clear();
    DeadBlocks.clear();
    PhiTranslateTable.clear();
    AvailOut.clear();
    AvailableMemoryLoads.clear();
    AvailableMemoryCalls.clear();
    MemorySSAQueries = 0;
}
//判断指令是否产生可以参与值编号的SSA结果
bool GVNPRESolver::hasResultValue(Instruction *I) {
    return I && I->getIRType() && !I->getIRType()->isVoid();
}
//重建可达基本块集合及其反向后序
void GVNPRESolver::rebuildRPOAndReachability() {
    //1.2 从入口重建 RPO，并以同一集合区分真实不可达块。
    ReachableBlocks.clear();
    RPO.clear();
    BasicBlock *Entry = F.front();
    if (!Entry) return;
    std::vector<BasicBlock *> Roots{Entry};
    RPO = buildRPOFromRoots<BasicBlock>(Roots);
    ReachableBlocks.insert(RPO.begin(), RPO.end());
}
//在不改写IR的情况下重建PRE所需值号表
void GVNPRESolver::buildTablesForPRE() {
    //3.1 按 RPO 为 PRE 预编号全部可达结果值，同时根据常量分支标记本轮死区域。
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
//沿支配树前序计算每个基本块出口的可用值集合
void GVNPRESolver::buildAvailOut() {
    //3.2 按支配树前序复制立即支配者的出口可用表，再加入本块首次出现的每个值号；
    //因而 AvailOut 中的 leader 必然支配对应块出口。
    BasicBlock *Entry = F.front();
    if (!Entry) return;
    std::vector<BasicBlock *> Roots;
    if (!isDeadBlock(Entry)) Roots.push_back(Entry);
    auto DomOrder =
        buildPreOrderFromRootsCustomSucc<BasicBlock>(
            Roots, [this](BasicBlock *BB) {
                std::vector<BasicBlock *> Children;
                if (!BB || isDeadBlock(BB)) return Children;
                for (BasicBlock *Child : DT.getChildren(BB)) {
                    if (Child && !isDeadBlock(Child))
                        Children.push_back(Child);
                }
                std::reverse(Children.begin(), Children.end());
                return Children;
            });
    for (BasicBlock *BB : DomOrder) {
        auto &Avail = AvailOut[BB];
        BasicBlock *IDom = DT.getIDom(BB);
        if (IDom && !isDeadBlock(IDom)) {
            auto It = AvailOut.find(IDom);
            if (It != AvailOut.end()) {
                Avail = It->second;
            }
        }
        for (Instruction *I : *BB) {
            if (!I || !hasResultValue(I)) continue;
            auto VIt = ValueNum.find(I);
            if (VIt == ValueNum.end()) continue;
            int Num = VIt->second;
            if (Num <= 0) continue;
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
//判断基本块是否不可达或已被常量分支证明为死块
bool GVNPRESolver::isDeadBlock(BasicBlock *BB) const {
    return !BB || DeadBlocks.count(BB) || ReachableBlocks.count(BB) == 0;
}
//为给定值号登记一条候选代表定义
void GVNPRESolver::addRecord(int Num, Instruction *I, BasicBlock *BB) {
    if (!I || !BB || Num <= 0) return;
    auto &Vec = LeaderTable[Num];
    for (const auto &R : Vec) {
        if (R.Inst == I) return;
    }
    Vec.push_back({Num, I, BB});
    NumToValue[Num] = I;
}
//从值号表和代表表中遗忘即将删除的指令
void GVNPRESolver::forgetInstruction(Instruction *I) {
    //2.3 删除前同步驱逐 ValueNum、LeaderTable 与代表值，防止后续命中悬空 leader。
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
//判断同一基本块中的指令A是否位于指令B之前
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
//查找在使用位置合法可用的值号代表
Instruction *GVNPRESolver::findLeader(int Num, BasicBlock *UseBB, Instruction *UseI) const {
    //2.4 leader 必须支配 use；同块还必须位于 use 之前，且两者之间不能跨计时屏障。
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
        if (UseI && F.hasTimingBarrierOnPath(R.Inst, UseI)) continue;
        Best = R.Inst;
        if (R.Inst->isConst()) return Best;
    }
    return Best;
}
//查找在指定前驱块出口可用的值号代表
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
//为给定IR值分配新的唯一值号
int GVNPRESolver::allocateFreshNum(Value *V) {
    const int N = NextValueNum++;
    ValueNum[V] = N;
    NumToValue.emplace(N, V);
    return N;
}
//返回值的现有值号或为其分配新值号
int GVNPRESolver::getOrAssignValueNum(Value *V, bool &IsFresh) {
    //2.5 内存、PHI、控制和有副作用调用各获独立值号；其余纯表达式由规范键查表，
    //已有键共享值号，新键同时登记反向表达式供 PHI translate 使用。
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
//为支持的纯指令构造规范化全局表达式键
bool GVNPRESolver::buildExpressionKey(Instruction *I, ExprKey &K) {
    //2.6 表达式键包含精确结果类型、opcode/谓词和操作数值号；交换律运算排序，
    //比较交换两侧时同步反转谓词，调用仅接纳可 CSE 的 NoModRef 情形。
    if (!I || !I->getIRType()) return false;
    auto appendOperands = [&](Instruction *Inst, std::vector<int> &Args) {
        forEachOperand(
            Inst, [&](size_t, Use *, Value *Operand) {
            bool Fresh = false;
            Args.push_back(getOrAssignValueNum(Operand, Fresh));
        });
    };
    K.Args.clear();
    K.Opcode = (static_cast<unsigned>(I->instType) << 8);
    K.ResultType = I->getIRType().get();
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
        if (!CI || getCallModRef(CI) != ModRefInfo::NoModRef ||
            !isCallCSEEligible(CI)) {
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
//查询调用的程序内存ModRef行为
ModRefInfo GVNPRESolver::getCallModRef(CallInst *CI) {
    //2.7 统一复用 SideEffect 的程序内存摘要；timer 的可观察性和 CSE 禁止
    //由 isCallCSEEligible 单独维护，不再污染 ModRef。
    return getCallProgramModRefInfo(CI, FAM);
}
//判断调用是否满足GVN公共子表达式消除条件
bool GVNPRESolver::isCallCSEEligible(CallInst *CI) {
    //2.8 调用 CSE 仅支持已定义、非 builtin、非 void/指针返回且摘要至多 Ref 的函数，
    //并排除传递调用 builtin 的不稳定摘要。
    if (!CI || CI->mayReachTimingCall() || CI->getBuiltinCallee() ||
        !CI->getIRType() ||
        CI->getIRType()->isVoid() || CI->getIRType()->isPointer())
        return false;
    Function *Callee = CI->getDirectCallee();
    if (!Callee || CI->getArgCount() != Callee->getParamCount())
        return false;
    ModRefInfo MRI = getCallModRef(CI);
    if (!isNoModRef(MRI) && MRI != ModRefInfo::Ref)
        return false;
    const SideEffectInfo &Effects =
        FAM.getResult<SideEffectAnalysis>(*Callee);
    return !Effects.hasBuiltin;
}
//收集只读调用实际读取的指针实参
std::vector<Value *> GVNPRESolver::collectCallReadPtrs(CallInst *CI) {
    //2.9 将只读调用的 RefSet 映射为实际全局和指针实参，供两次调用间写入屏障检查。
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
        if (getIntrinsicID(BIF) == IntrinsicID::Memcpy ||
            getIntrinsicID(BIF) == IntrinsicID::Memmove) {
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
            if (!RV) continue;
            if (RV->isGlobal()) {
                pushUnique(RV);
                continue;
            }
            if (!RV->isParam()) continue;
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
//判断两个调用的被调目标、类型和实参是否完全相同
bool GVNPRESolver::identicalCall(CallInst *A, CallInst *B) {
    if (!A || !B) return false;
    if (A->getNumOperands() != B->getNumOperands()) return false;
    return forEachOperand(
        A, [&](size_t Index, Use *, Value *Operand) {
            Value *OtherOperand = B->getOperand(Index);
            return Operand && OtherOperand && Operand == OtherOperand;
        });
}
//判断两个调用读取位置及其内存版本签名是否相同
bool GVNPRESolver::MemoryReadClobber::operator==(
    const MemoryReadClobber &Other) const {
    return Location.Pointer == Other.Location.Pointer &&
           Location.Size == Other.Location.Size &&
           Clobbers == Other.Clobbers;
}
//构建只读调用所有读取位置的MemorySSA签名
bool GVNPRESolver::buildReadOnlyCallSignature(
    CallInst *CI, std::vector<MemoryReadClobber> &Signature) {
    Signature.clear();
    if (!CI || !MemorySSAValid) return false;
    //只接纳目标、读写角色和顺序语义全部明确的纯只读调用；跨对象访问即使角色
    //是Ref也缺少可靠区间，不能构造逐位置内存版本签名。
    CallEffectSummary Effects = getCallEffectSummary(CI, FAM);
    if (Effects.HasUnknownMemory || Effects.HasObservableEffect ||
        Effects.HasOrderingBarrier || Effects.MayCrossObjectBounds ||
        Effects.ProgramMemory.empty()) {
        return false;
    }
    for (const CallMemoryEffect &Effect : Effects.ProgramMemory) {
        if (Effect.ModRef != ModRefInfo::Ref ||
            !Effect.Location.Pointer ||
            MemorySSAQueries >= kMaxMemorySSAQueries) {
            Signature.clear();
            return false;
        }
        ++MemorySSAQueries;
        MemoryClobberResult Clobber = MSSA.getClobberingAccesses(
            CI, Effect.Location, AA, LI, FAM, 256);
        if (!Clobber.Complete || Clobber.Clobbers.empty()) {
            Signature.clear();
            return false;
        }
        Signature.push_back(
            {Effect.Location, std::move(Clobber.Clobbers)});
    }
    //摘要集合本身不承诺遍历顺序；规范化后相同调用才能逐项比较签名。
    std::sort(
        Signature.begin(), Signature.end(),
        [](const MemoryReadClobber &LHS,
           const MemoryReadClobber &RHS) {
            if (LHS.Location.Pointer != RHS.Location.Pointer) {
                return std::less<Value *>{}(
                    LHS.Location.Pointer, RHS.Location.Pointer);
            }
            if (LHS.Location.Size != RHS.Location.Size)
                return LHS.Location.Size < RHS.Location.Size;
            if (LHS.Clobbers.size() != RHS.Clobbers.size())
                return LHS.Clobbers.size() < RHS.Clobbers.size();
            for (std::size_t Index = 0;
                 Index < LHS.Clobbers.size(); ++Index) {
                unsigned LHSID = LHS.Clobbers[Index]->getID();
                unsigned RHSID = RHS.Clobbers[Index]->getID();
                if (LHSID != RHSID) return LHSID < RHSID;
            }
            return false;
        });
    return true;
}
//从常量分支的死后继开始标记只由死边可达的基本块
void GVNPRESolver::markDeadBranchBlocks(BasicBlock *BrBB, BasicBlock *DeadSucc) {
    //2.10 从常量分支未取后继收集候选区域，再迭代剔除仍有区域外前驱的块；剩余块
    //才真正只由死边可达，可从本轮值编号与 PRE 路径中排除。
    if (!BrBB || !DeadSucc || DeadSucc == BrBB) return;
    std::vector<BasicBlock *> Roots{DeadSucc};
    std::unordered_set<BasicBlock *> Candidate =
        collectReachableFromRoots<
            BasicBlock, WorkListOrder::FIFO>(Roots);
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
//识别常量条件分支并更新不可达基本块集合
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
//尝试使用等价内存版本的支配store或load替换当前load
bool GVNPRESolver::processLoad(Instruction *I) {
    //2.11 load消除先查询有预算的MemorySSA完整clobber集合：若每条路径均由
    //MustAlias完整Store写入同一个支配值，直接转发；否则保留集合用于load CSE。
    auto *LIst = dynamic_cast<LoadInst *>(I);
    if (!LIst) return false;
    Value *Ptr = LIst->getPointerOperand();
    if (!Ptr) return false;
    BasicBlock *BB = I->getParent();
    if (!BB) return false;
    auto TypeCompatible = [&](Value *V) {
        return V && V->getIRType() && LIst->getIRType() &&
               V->getIRType()->hasSameShape(*LIst->getIRType());
    };
    MemoryLocation Location = MemoryLocation::get(LIst);
    std::vector<MemoryAccess *> Clobbers;
    if (MemorySSAValid && MemorySSAQueries < kMaxMemorySSAQueries) {
        ++MemorySSAQueries;
        MemoryClobberResult Result = MSSA.getClobberingAccesses(
            LIst, Location, AA, LI, FAM, 256);
        if (Result.Complete)
            Clobbers = std::move(Result.Clobbers);
    }
    Value *CommonStoredValue = nullptr;
    bool AllSameValueStores = !Clobbers.empty();
    for (MemoryAccess *Clobber : Clobbers) {
        auto *Store = Clobber && Clobber->isDef()
                          ? dynamic_cast<StoreInst *>(
                                Clobber->getInstruction())
                          : nullptr;
        Value *Stored = Store ? Store->getValueOperand() : nullptr;
        MemoryLocation StoreLocation = MemoryLocation::get(Store);
        if (!Store || !Stored || Stored->isUndef() ||
            !Store->fullyCoversItsPointer() ||
            !StoreLocation.Size || !Location.Size ||
            *StoreLocation.Size < *Location.Size ||
            !TypeCompatible(Stored) ||
            AA.aliasWithIndexRange(Location, StoreLocation, LI) !=
                AliasInfo::MustAlias) {
            AllSameValueStores = false;
            break;
        }
        if (!CommonStoredValue)
            CommonStoredValue = Stored;
        else if (CommonStoredValue != Stored) {
            AllSameValueStores = false;
            break;
        }
    }
    if (AllSameValueStores && CommonStoredValue &&
        DT.dominatesValue(CommonStoredValue, LIst)) {
        I->replaceAllUsesWith(CommonStoredValue);
        forgetInstruction(I);
        delete I;
        return true;
    }
    //2.12 相同完整clobber集合只用于筛选支配的MustAlias load；循环回边会让
    //不同迭代到达同一批静态MemoryDef，因此还要从当前状态反向证明每条路径
    //都能无写到达候选load消费的内存边界。
    if (!Clobbers.empty()) {
        std::size_t Compared = 0;
        for (auto It = AvailableMemoryLoads.rbegin();
             It != AvailableMemoryLoads.rend() &&
             Compared < kMaxMemoryLoadComparisons; ++It, ++Compared) {
            if (It->Clobbers != Clobbers || !It->Load || !It->Block ||
                !It->Load->getParent() ||
                !TypeCompatible(It->Load) ||
                !DT.dominates(It->Block, BB) ||
                (It->Block == BB &&
                 !Instruction::comesBefore(It->Load, LIst)))
                continue;
            if (AA.aliasWithIndexRange(Location, It->Location, LI) !=
                AliasInfo::MustAlias)
                continue;
            MemoryAccess *LoadAccess = MSSA.getMemoryAccess(LIst);
            if (!LoadAccess ||
                MSSA.hasClobberBetween(
                    It->Load, LoadAccess, Location, AA, LI, FAM, 256))
                continue;
            I->replaceAllUsesWith(It->Load);
            forgetInstruction(I);
            delete I;
            return true;
        }
    }
    auto RememberAndFail = [&]() {
        if (!Clobbers.empty() &&
            AvailableMemoryLoads.size() < kMaxMemoryLoadRecords) {
            AvailableMemoryLoads.push_back(
                {Clobbers, Location, LIst, BB});
        }
        return false;
    };
    //2.13 最后沿唯一前驱链做有界逆向扫描，最近 MustAlias store/load 可转发；
    //MayAlias 写或调用 Mod 终止，timer 的调用节点按程序内存摘要透明越过。
    BasicBlock *ScanBB = BB;
    Instruction *StartI = I->getPrev();
    int ScanCount = 0;
    while (ScanBB) {
        Instruction *Cur = (ScanBB == BB) ? StartI : ScanBB->back();
        for (; Cur != nullptr; Cur = Cur->getPrev()) {
            if (++ScanCount > kBackwardScanLimit) return RememberAndFail();
            if (auto *SI = dynamic_cast<StoreInst *>(Cur)) {
                Value *SPtr = SI->getPointerOperand();
                if (!SPtr) continue;
                AliasInfo AI = AA.aliasWithIndexRange(
                    Location, MemoryLocation::get(SI), LI);
                if (AI == AliasInfo::MustAlias) {
                    Value *Stored = SI->getValueOperand();
                    if (!TypeCompatible(Stored)) return RememberAndFail();
                    I->replaceAllUsesWith(Stored);
                    forgetInstruction(I);
                    delete I;
                    return true;
                }
                if (AI == AliasInfo::MayAlias) return RememberAndFail();
                continue;
            }
            if (auto *LI2 = dynamic_cast<LoadInst *>(Cur)) {
                Value *PrevPtr = LI2->getPointerOperand();
                if (!PrevPtr) continue;
                AliasInfo AI = AA.aliasWithIndexRange(
                    Location, MemoryLocation::get(LI2), LI);
                if (AI == AliasInfo::MustAlias && TypeCompatible(LI2)) {
                    I->replaceAllUsesWith(LI2);
                    forgetInstruction(I);
                    delete I;
                    return true;
                }
                continue;
            }
            if (dynamic_cast<CallInst *>(Cur)) {
                ModRefInfo MRI = getInstModRefInfo(
                    Location, Cur, AA, LI, FAM);
                if (isModSet(MRI)) return RememberAndFail();
            }
        }
        auto Preds = ScanBB->getPredecessors();
        if (Preds.size() != 1 || isDeadBlock(Preds[0])) break;
        ScanBB = Preds[0];
    }
    return RememberAndFail();
}
//尝试使用等价实参和内存签名的支配调用替换当前调用
bool GVNPRESolver::processCall(Instruction *I) {
    //2.14 对Ref-only调用先用逐读取位置的MemorySSA clobber签名筛选支配调用，
    //再逐位置证明两次调用之间所有路径均无写；查询不可用时沿唯一前驱链执行
    //原有保守扫描。静态签名相同本身不能区分循环中的不同动态迭代。
    auto *CI = dynamic_cast<CallInst *>(I);
    if (!CI) return false;
    if (getCallModRef(CI) != ModRefInfo::Ref ||
        !isCallCSEEligible(CI))
        return false;
    BasicBlock *BB = I->getParent();
    if (!BB) return false;
    std::vector<MemoryReadClobber> Signature;
    if (buildReadOnlyCallSignature(CI, Signature)) {
        std::size_t Compared = 0;
        for (auto It = AvailableMemoryCalls.rbegin();
             It != AvailableMemoryCalls.rend() &&
             Compared < kMaxMemoryCallComparisons; ++It, ++Compared) {
            CallInst *Previous = It->Call;
            if (!Previous || !It->Block ||
                !Previous->isInBlock(It->Block) ||
                It->Signature != Signature ||
                !identicalCall(CI, Previous) ||
                !DT.dominates(It->Block, BB) ||
                (It->Block == BB &&
                 !Instruction::comesBefore(Previous, CI)) ||
                F.hasTimingBarrierOnPath(Previous, CI)) {
                continue;
            }
            MemoryAccess *CallAccess = MSSA.getMemoryAccess(CI);
            bool Unmodified = CallAccess != nullptr;
            for (const MemoryReadClobber &Read : Signature) {
                if (!Unmodified ||
                    MemorySSAQueries >= kMaxMemorySSAQueries) {
                    Unmodified = false;
                    break;
                }
                ++MemorySSAQueries;
                if (MSSA.hasClobberBetween(
                        Previous, CallAccess, Read.Location,
                        AA, LI, FAM, 256)) {
                    Unmodified = false;
                    break;
                }
            }
            if (!Unmodified) continue;
            I->replaceAllUsesWith(Previous);
            forgetInstruction(I);
            //删除MemoryUse不会改变定义链，但旧图仍持有该Use的指令指针；后续
            //不再进行MemorySSA查询，避免未来正向walker观察悬空节点。
            MemorySSAValid = false;
            AvailableMemoryLoads.clear();
            AvailableMemoryCalls.clear();
            delete I;
            return true;
        }
        if (AvailableMemoryCalls.size() < kMaxMemoryCallRecords)
            AvailableMemoryCalls.push_back(
                {CI, BB, std::move(Signature)});
    }
    auto ReadPtrs = collectCallReadPtrs(CI);
    //Ref摘要若不能完整映射到调用点的实际读取位置，唯一前驱扫描也无法判断
    //Store是否干扰该调用，必须保守放弃而不能把所有Store视作透明。
    if (ReadPtrs.empty()) return false;
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
                    if (!AA.haveKnownDistinctUnderlyingObjects(RP, SPtr))
                        return false;
                }
                continue;
            }
            if (auto *PrevCall = dynamic_cast<CallInst *>(Cur)) {
                if (PrevCall->mayReachTimingCall()) return false;
                if (identicalCall(CI, PrevCall)) {
                    I->replaceAllUsesWith(PrevCall);
                    forgetInstruction(I);
                    MemorySSAValid = false;
                    AvailableMemoryLoads.clear();
                    AvailableMemoryCalls.clear();
                    delete I;
                    return true;
                }
                for (Value *RP : ReadPtrs) {
                    if (!RP || !RP->isPointerValue())
                        continue;
                    ModRefInfo MRI = getInstModRefInfo(RP, PrevCall, AA, LI, FAM);
                    if (isModSet(MRI) ||
                        (isNoModRef(MRI) &&
                         !PrevCall->getBuiltinCallee() &&
                         AA.callMayAliasPointer(PrevCall, RP, LI)))
                        return false;
                }
            }
        }
        auto Preds = ScanBB->getPredecessors();
        if (Preds.size() != 1 || isDeadBlock(Preds[0])) break;
        ScanBB = Preds[0];
    }
    return false;
}
//执行一轮全局值编号、内存转发和冗余替换
bool GVNPRESolver::runGVN() {
    //2. 先消除递归等价 PHI，再按 RPO 扫描可达指令；timer 不清空程序内存
    //load 缓存，常量分支即时标记死块，load/call 使用专门的内存感知路径。
    bool Changed = eliminateCongruentPhis();
    for (BasicBlock *BB : RPO) {
        if (isDeadBlock(BB)) continue;
        auto Insts = collectInstructions(*BB);
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
            //2.15 普通结果获取值号：新表达式登记 leader，已有表达式寻找合法支配
            //leader 替换；删除前同步清理所有编号记录。
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
            if (dynamic_cast<CallInst *>(I)) {
                MemorySSAValid = false;
                AvailableMemoryLoads.clear();
                AvailableMemoryCalls.clear();
            }
            delete I;
            Changed = true;
        }
    }
    return Changed;
}
//合并支配等价的循环PHI并维护SSA使用关系
bool GVNPRESolver::eliminateCongruentPhis() {
    //2.16 在每个可达块中成对比较同型 PHI；不因循环含 timer 整体放弃，
    //而是对候选操作数图做有界跨屏障检查后再合并。
    bool Changed = false;
    for (BasicBlock *BB : RPO) {
        if (!BB || isDeadBlock(BB)) continue;
        auto Phis = BB->collectPhiNodes();
        std::unordered_set<PhiInst *> Removed;
        for (size_t I = 0; I < Phis.size(); ++I) {
            PhiInst *Leader = Phis[I];
            if (!Leader || Removed.count(Leader)) continue;
            for (size_t J = I + 1; J < Phis.size(); ++J) {
                PhiInst *Candidate = Phis[J];
                if (!Candidate || Removed.count(Candidate) ||
                    Leader->getIRType().get() !=
                        Candidate->getIRType().get() ||
                    Leader->getNumIncomingValues() !=
                        Candidate->getNumIncomingValues()) {
                    continue;
                }
                PhiCongruenceMatcher Matcher;
                if (!Matcher.equivalent(Leader, Candidate)) continue;
                auto CrossesTiming = [&](Value *Root) {
                    std::vector<Value *> WorkList{Root};
                    std::unordered_set<Value *> Seen;
                    std::size_t Budget = 0;
                    while (!WorkList.empty()) {
                        Value *Current = WorkList.back();
                        WorkList.pop_back();
                        if (!Current || !Seen.insert(Current).second) continue;
                        if (++Budget > 512) return true;
                        auto *Def = dynamic_cast<Instruction *>(Current);
                        if (!Def || !Def->getParent()) continue;
                        if (Def != Leader && Def != Candidate &&
                            F.hasTimingBarrierOnPath(Def, Candidate)) {
                            return true;
                        }
                        forEachOperand(
                            Def, [&](size_t, Use *, Value *Operand) {
                                if (Operand) WorkList.push_back(Operand);
                            });
                    }
                    return false;
                };
                if (CrossesTiming(Leader) || CrossesTiming(Candidate))
                    continue;
                Candidate->replaceAllUsesWith(Leader);
                Removed.insert(Candidate);
                Changed = true;
            }
        }
        for (PhiInst *Phi : Removed) {
            if (Phi) delete Phi;
        }
    }
    return Changed;
}
//判断给定CFG边是否为支配树意义上的循环回边
bool GVNPRESolver::isBackedge(BasicBlock *Pred, BasicBlock *BB, const DominatorTree &DT) {
    if (!Pred || !BB) return false;
    return DT.dominates(BB, Pred);
}
//判断指令是否不适合作为PRE候选表达式
bool GVNPRESolver::shouldSkipPREInst(Instruction *I) {
    //3.3 PRE 只补纯、可安全推测且有结果的标量表达式；控制、内存、比较、调用和
    //可能陷阱/poison 的指令全部排除。
    if (!I || !hasResultValue(I)) return true;
    if (I->instType == Instruction::ALLOCA || I->instType == Instruction::RET ||
        I->instType == Instruction::BR     || I->instType == Instruction::PHI ||
        I->instType == Instruction::LOAD   || I->instType == Instruction::STORE) {
        return true;
    }
    if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
        if (BinaryInst::isCompareOp(BI->getOp())) return true;
    }
    if (instructionMayFaultOrProducePoison(I)) {
        return true;
    }
    if (dynamic_cast<CallInst *>(I)) return true;
    return false;
}
//把表达式值号从汇合块翻译到指定前驱并缓存结果
int GVNPRESolver::phiTranslate(int Num, BasicBlock *FromBB, BasicBlock *ToPred) {
    //3.4 把汇合块中的值号翻译到指定前驱：若值号来自 PHI，取该边 incoming；
    //若来自表达式，递归翻译各参数并重新查规范表达式键，结果按边缓存。
    PhiTranslateKey Key{Num, FromBB, ToPred};
    auto It = PhiTranslateTable.find(Key);
    if (It != PhiTranslateTable.end()) {
        return It->second;
    }
    int Result = Num;
    if (FromBB) {
        for (Instruction *DefI : *FromBB) {
            if (!DefI) continue;
            auto *Phi = dynamic_cast<PhiInst *>(DefI);
            if (!Phi) break;
            auto VIt = ValueNum.find(DefI);
            if (VIt == ValueNum.end() || VIt->second != Num) continue;
            bool FoundIncoming = false;
            forEachPhiIncoming(
                Phi, [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                    if (Pred != ToPred) return true;
                    bool Fresh = false;
                    Result = getOrAssignValueNum(Incoming, Fresh);
                    FoundIncoming = true;
                    return false;
                });
            if (FoundIncoming) {
                PhiTranslateTable[Key] = Result;
                return Result;
            }
        }
    }
    auto EI = NumToExpr.find(Num);
    if (EI != NumToExpr.end()) {
        ExprKey NewK = EI->second;
        bool AnyChanged = false;
        for (int &ArgNum : NewK.Args) {
            int Translated = phiTranslate(ArgNum, FromBB, ToPred);
            if (Translated != ArgNum) AnyChanged = true;
            ArgNum = Translated;
        }
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
        }
    }
    PhiTranslateTable[Key] = Result;
    return Result;
}
//把原始操作数解析为指定前驱出口可用的SSA值
Value *GVNPRESolver::resolveAvailOperand(Value *OrigV, BasicBlock *SrcBB, BasicBlock *PredBB) {
    //3.5 克隆到缺失前驱前，块内操作数需经 PHI translate 找到该前驱可用 leader；
    //块外定义则必须支配前驱，否则无法构造合法 SSA 操作数。
    if (!OrigV) return nullptr;
    auto *OpI = dynamic_cast<Instruction *>(OrigV);
    if (!OpI) {
        return OrigV;
    }
    BasicBlock *DefBB = OpI->getParent();
    if (!DefBB) return nullptr;
    if (DefBB == SrcBB) {
        bool OpFresh = false;
        int OpNum    = getOrAssignValueNum(OrigV, OpFresh);
        int TransNum = phiTranslate(OpNum, SrcBB, PredBB);
        if (Instruction *Avail = findAvailLeader(TransNum, PredBB)) {
            return Avail;
        }
        if (Instruction *Leader = findLeader(TransNum, PredBB, nullptr)) {
            return Leader;
        }
        return nullptr;
    }
    if (!DT.dominates(DefBB, PredBB)) return nullptr;
    return OrigV;
}
//把死块中的定义播种到代表表以维持死代码内部编号稳定
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
//执行一轮PRE插入、合流PHI构造和冗余表达式替换
bool GVNPRESolver::runPRE() {
    //3. 先把死块结果加入 leader 表以稳定值号，再按入口 DFS 扫描非入口汇合块。
    bool Changed = false;
    seedDeadBlocksToLeaderTable();
    if (!F.front()) return false;
    std::vector<BasicBlock *> Roots{F.front()};
    std::vector<BasicBlock *> DFSOrder =
        buildPreOrderFromRoots<BasicBlock>(Roots);
    for (BasicBlock *BB : DFSOrder) {
        if (!BB || BB == F.front() || isDeadBlock(BB)) continue;
        auto Insts = collectInstructions(*BB);
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
            std::unordered_map<BasicBlock *, Value *> PredMap;
            std::vector<BasicBlock *> ConsideredPreds;
            //3.6 对每条非死、非回边前驱翻译当前表达式值号；PRE 只接受恰好一条
            //缺失、至少一条已有的部分冗余形态，避免代码膨胀和循环回边插入。
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
                Instruction *Leader = findAvailLeader(TransNum, Pred);
                if (!Leader) {
                    Leader = findLeader(TransNum, Pred, nullptr);
                }
                if (!Leader) {
                    ++NumWithout;
                    PREPred = Pred;
                    continue;
                }
                if (F.hasTimingBarrierOnPath(Leader, I)) {
                    Abort = true;
                    break;
                }
                if (Leader == I) {
                    NumWithout = 2;
                    Abort      = true;
                    break;
                }
                ++NumWith;
                PredMap[Pred] = Leader;
            }
            if (Abort) continue;
            if (!(NumWithout == 1 && NumWith > 0 && PREPred)) {
                continue;
            }
            Instruction *PredTerminator = PREPred->getTerminator();
            if (!PredTerminator ||
                F.hasTimingBarrierOnPath(
                    PREPred, PredTerminator->getPrev(), I)) {
                continue;
            }
            //3.7 在缺失前驱终结前克隆表达式，并逐操作数解析该边可用值；任一操作数
            //无合法 leader 时丢弃离线克隆。
            Instruction *Cloned = I->clone({});
            if (!Cloned) continue;
            if (I->getNumOperands() != Cloned->getNumOperands()) {
                delete Cloned;
                continue;
            }
            bool OperandOK = forEachOperand(
                I, [&](size_t Idx, Use *, Value *OrigV) {
                if (!OrigV) return false;
                Value *Resolved = resolveAvailOperand(OrigV, BB, PREPred);
                if (!Resolved) return false;
                if (Cloned->getOperand(Idx) != Resolved)
                    Cloned->replaceOperand(Idx, Resolved);
                return true;
            });
            if (!OperandOK) {
                delete Cloned;
                continue;
            }
            PREPred->insertBeforeTerminator(Cloned);
            ValueNum[Cloned] = Num;
            NumToValue[Num]  = Cloned;
            addRecord(Num, Cloned, PREPred);
            AvailOut[PREPred][Num] = {Cloned, PREPred};
            //3.8 在汇合块 PHI 区后建立新 PHI，合并已有 leader 与新克隆；同步更新
            //值号、AvailOut 及其支配子树，并清除从本块出发的旧翻译缓存。
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
            ValueNum[NewPhi] = Num;
            NumToValue[Num]  = NewPhi;
            addRecord(Num, NewPhi, BB);
            AvailOut[BB][Num] = {NewPhi, BB};
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
//判断两个局部表达式键是否完全相同
bool LocalGVNPass::LocalExprKey::operator==(
    const LocalExprKey &Other) const {
    return Opcode == Other.Opcode && SubOpcode == Other.SubOpcode &&
           ResultType == Other.ResultType && Operands == Other.Operands;
}
//计算给定局部表达式键的哈希值
std::size_t LocalGVNPass::LocalExprKeyHash::operator()(
    const LocalExprKey &K) const {
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
//为支持的纯指令构造单块局部表达式键
bool LocalGVNPass::buildLocalExprKey(Instruction *I, LocalExprKey &K) {
    //1.1 本地键直接使用操作数指针，只覆盖无 undef 的纯二元/GEP/select/cast；
    //交换律二元式规范操作数顺序，不涉及跨块值号或内存表达式。
    if (!I || !I->getIRType() || I->getIRType()->isVoid())
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
    bool OperandsOK = forEachOperand(
        I, [&](size_t, Use *, Value *Operand) {
            if (!Operand || Value::isUndef(Operand)) return false;
            K.Operands.push_back(Operand);
            return true;
        });
    return OperandsOK && !K.Operands.empty();
}
//对给定函数运行GVN及可选PRE并返回分析保留状态
PreservationStatus GVNPREPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    //1. 获取支配、循环、AA 与 MemorySSA，构造求解器执行 GVN 及可选 PRE 固定点。
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
    GVNPRESolver Solver(F, FAM, DT, LI, AA, MSSA, EnablePRE);
    bool Changed = Solver.run();
    if (!Changed) {
        return PreservationStatus::all();
    }
    //4. CFG 不变，结构分析可保留；表达式/内存访问与调用集合变化使其余分析失效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<MemorySSAAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
//对给定函数逐块运行局部值编号并返回分析保留状态
PreservationStatus LocalGVNPass::run(Function &F,
                                     FunctionAnalysisPassManager &) {
    //1. 每个基本块独立维护表达式 leader 表，按程序顺序将重复纯表达式替换为首个；
    //计时调用清空表，禁止跨越计时边界合并地址或计算。
    bool Changed = false;
    for (BasicBlock *BB : F) {
        if (!BB) continue;
        std::unordered_map<LocalExprKey, Instruction *, LocalExprKeyHash> Leaders;
        auto Insts = collectInstructions(*BB);
        for (Instruction *I : Insts) {
            if (!I || I->getParent() != BB) continue;
            if (auto *CI = dynamic_cast<CallInst *>(I);
                CI && CI->mayReachTimingCall()) {
                Leaders.clear();
                continue;
            }
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
    //2. 仅删普通指令，保留 CFG 分析并失效 AA/副作用摘要。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
