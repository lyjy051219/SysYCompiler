#include "ir/opt/LoadElimination.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "lib/IRUtils.hpp"
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
//构造携带确定可用值的查询结果
LoadElimQueryResult LoadElimQueryResult::known(Value *V) {
    return {KNOWN, V, {}};
}
//构造已被相关内存写破坏的查询结果
LoadElimQueryResult LoadElimQueryResult::clobbered() {
    return {CLOBBERED, nullptr, {}};
}
//构造因预算或未知语义而无法证明的查询结果
LoadElimQueryResult LoadElimQueryResult::unknown() {
    return {UNKNOWN, nullptr, {}};
}
//构造需要继续查询基本块前驱的查询结果
LoadElimQueryResult LoadElimQueryResult::needPredecessors() {
    return {NEED_PREDECESSORS, nullptr, {}};
}
//构造需要按入边值在汇合块合成PHI的查询结果
LoadElimQueryResult LoadElimQueryResult::phiNeeded(
    std::vector<std::pair<BasicBlock *, Value *>> M) {
    return {PHI_NEEDED, nullptr, std::move(M)};
}
//判断候选值与目标load是否具有相同的结果类型形状
bool LoadEliminationPass::isTypeCompatible(
    Value *source, LoadInst *targetLoad) {
    if (!source || !targetLoad) return false;
    if (!source->getIRType() || !targetLoad->getIRType()) return false;
    return source->getIRType()->hasSameShape(*targetLoad->getIRType());
}
//通过统一ModRef查询判断指令是否可能修改目标内存位置
bool LoadEliminationPass::mayModifyPointer(
    const MemoryLocation &Location, Instruction *I, BasicAAResult &AA,
    const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    //1.1 顺序屏障不一定读写 SysY 对象，但删除它两侧的load会改变
    //计时区间内的工作量；因此在值可用性证明中与内存写统一视为截断点。
    if (!Location.Pointer || !I) return true;
    if (auto *CI = dynamic_cast<CallInst *>(I)) {
        if (getCallEffectSummary(CI, FAM).HasOrderingBarrier)
            return true;
    }
    ModRefInfo MR = getInstModRefInfo(Location, I, AA, LI, FAM);
    return isModSet(MR);
}
//证明候选load到目标load的每条CFG路径都没有相关写入
bool LoadEliminationPass::isUnmodifiedOnAllPaths(
    LoadInst *Candidate, LoadInst *Target, Value *P,
    DominatorTree &DT, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    //2.1候选 load 必须支配目标。随后从目标反向遍历全部前驱路径，直到每条路径
    //都命中候选；期间一旦出现可能修改该地址的指令，证明即失败。
    if (!Candidate || !Target || !P) return false;
    BasicBlock *CandidateBB = Candidate->getParent();
    BasicBlock *TargetBB = Target->getParent();
    if (!CandidateBB || !TargetBB || CandidateBB == TargetBB) return false;
    if (!DT.dominates(CandidateBB, TargetBB)) return false;
    std::vector<PathScanState> worklist{{TargetBB, true}};
    std::unordered_set<BasicBlock *> visited;
    int scannedBlocks = 0;
    int scannedInstructions = 0;
    while (!worklist.empty()) {
        PathScanState State = worklist.back();
        worklist.pop_back();
        BasicBlock *BB = State.BB;
        if (!BB) return false;
        //目标块首次只扫描 load 之前的前缀；经回边再次到达时必须扫描整块，
        //才不会漏掉上一轮迭代中位于目标 load 之后的写入。
        if (!State.TargetPrefixOnly && !visited.insert(BB).second) continue;
        if (++scannedBlocks > MaxPathScanBlocks) return false;
        Instruction *I = State.TargetPrefixOnly ? Target->getPrev() : BB->back();
        bool reachedCandidate = false;
        for (; I; I = I->getPrev()) {
            if (++scannedInstructions > MaxPathScanInstructions) return false;
            if (BB == CandidateBB && I == Candidate) {
                reachedCandidate = true;
                break;
            }
            if (mayModifyPointer(MemoryLocation::get(Target), I, AA, LI,
                                 FAM))
                return false;
        }
        if (BB == CandidateBB) {
            if (!reachedCandidate) return false;
            continue;
        }
        auto preds = BB->getPredecessors();
        if (preds.empty()) return false;
        for (BasicBlock *Pred : preds) {
            if (!Pred) return false;
            worklist.push_back({Pred, false});
        }
    }
    return true;
}
//沿支配链寻找经别名与全路径无写证明的循环外可用load
LoadInst *LoadEliminationPass::findAvailableDominatingLoad(
    LoadInst *Target, BasicBlock *BB, DominatorTree &DT,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    //2.2该兜底只服务循环：地址必须在目标循环外定义，再沿支配链寻找循环外、
    //MustAlias 且类型一致的 load，并交给全路径扫描验证跨迭代没有写入。
    Value *P = Target ? Target->getPointerOperand() : nullptr;
    if (!Target || !BB || !P) return nullptr;
    Loop *TargetLoop = LI.getLoopFor(BB);
    if (!TargetLoop) return nullptr;
    if (auto *PtrI = dynamic_cast<Instruction *>(P)) {
        BasicBlock *PtrBB = PtrI->getParent();
        if (!PtrBB || TargetLoop->contains(PtrBB)) return nullptr;
    }
    BasicBlock *Ancestor = DT.getIDom(BB);
    int depth = 0;
    for (; Ancestor && depth < MaxDominatorSearchDepth;
         Ancestor = DT.getIDom(Ancestor), ++depth) {
        if (TargetLoop->contains(Ancestor)) continue;
        if (!DT.dominates(Ancestor, TargetLoop->getHeader())) continue;
        for (Instruction *I = Ancestor->back(); I; I = I->getPrev()) {
            auto *Candidate = dynamic_cast<LoadInst *>(I);
            if (!Candidate || !isTypeCompatible(Candidate, Target)) continue;
            Value *CandidateP = Candidate->getPointerOperand();
            if (!CandidateP ||
                AA.aliasWithIndexRange(P, CandidateP, LI) !=
                    AliasInfo::MustAlias) {
                continue;
            }
            if (isUnmodifiedOnAllPaths(Candidate, Target, P, DT, AA, LI,
                                       FAM)) {
                return Candidate;
            }
        }
    }
    return nullptr;
}
//从指定位置向块首查询最近的MustAlias值或相关clobber
LoadElimQueryResult LoadEliminationPass::scanBlockBackward(
    BasicBlock *BB, Value *P, Instruction *stopBefore, LoadInst *targetLoad,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    //3.1从指定位置向块首逆向寻找最近内存事件：MustAlias store/load 给出已知值，
    //MayAlias 写入或有写效应调用返回 Clobbered，纯读事件可以继续穿越。
    Instruction *cur = nullptr;
    if (stopBefore) {
        cur = stopBefore->getPrev();
    } else {
        cur = BB->back();
    }
    while (cur) {
        if (auto *SI = dynamic_cast<StoreInst *>(cur)) {
            Value *StorePtr = SI->getPointerOperand();
            Value *StoredVal = SI->getValueOperand();
            if (StorePtr && StoredVal) {
                AliasInfo ai = AA.aliasWithIndexRange(P, StorePtr, LI);
                if (ai == AliasInfo::MustAlias && isTypeCompatible(StoredVal, targetLoad)) {
                    return LoadElimQueryResult::known(StoredVal);
                }
                if (ai != AliasInfo::NoAlias) {
                    return LoadElimQueryResult::clobbered();
                }
            }
        }
        else if (auto *LI2 = dynamic_cast<LoadInst *>(cur)) {
            if (LI2 != targetLoad) {
                Value *LoadPtr = LI2->getPointerOperand();
                if (AA.aliasWithIndexRange(P, LoadPtr, LI) == AliasInfo::MustAlias
                    && isTypeCompatible(LI2, targetLoad)) {
                    return LoadElimQueryResult::known(LI2);
                }
            }
        }
        else if (dynamic_cast<CallInst *>(cur)) {
            if (mayModifyPointer(
                    MemoryLocation::get(targetLoad), cur, AA, LI, FAM)) {
                return LoadElimQueryResult::clobbered();
            }
        }
        else {
            ModRefInfo MR = getInstModRefInfo(
                MemoryLocation::get(targetLoad), cur, AA, LI, FAM);
            if (isModSet(MR)) {
                return LoadElimQueryResult::clobbered();
            }
        }
        cur = cur->getPrev();
    }
    return LoadElimQueryResult::needPredecessors();
}
//递归汇总基本块全部前驱上的可用值并识别PHI需求
LoadElimQueryResult LoadEliminationPass::queryAvailableValue(
    Value *P, BasicBlock *BB, LoadInst *targetLoad, int depth,
    std::unordered_set<BasicBlock*> &onStack,
    std::unordered_map<BasicBlock*, LoadElimQueryResult> &memo,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    //3.2递归求解块入口的可用值。深度预算、递归栈环或无法解释的前驱都保守失败；
    //memo 让钻石 CFG 的公共祖先只求解一次。
    if (depth > MaxQueryDepth) {
        return LoadElimQueryResult::unknown();
    }
    ScopedActiveMark ActiveMark(onStack, BB);
    if (!ActiveMark.inserted()) return LoadElimQueryResult::unknown();
    auto memoIt = memo.find(BB);
    if (memoIt != memo.end()) {
        return memoIt->second;
    }
    LoadElimQueryResult result = scanBlockBackward(BB, P, nullptr, targetLoad, AA, LI, FAM);
    if (result.kind == LoadElimQueryResult::KNOWN || result.kind == LoadElimQueryResult::CLOBBERED) {
        memo[BB] = result;
        return result;
    }
    //3.3汇总全部前驱：值相同则直接可用，值不同则返回需在当前汇合点建 PHI；
    //任一路径被 clobber 或还需要嵌套 PHI 时，本层不继续猜测。
    auto preds = BB->getPredecessors();
    if (preds.empty()) {
        memo[BB] = LoadElimQueryResult::clobbered();
        return memo[BB];
    }
    std::vector<std::pair<BasicBlock*, Value*>> predValues;
    bool allSame = true;
    Value *firstVal = nullptr;
    for (BasicBlock *pred : preds) {
        LoadElimQueryResult predResult = queryAvailableValue(
            P, pred, targetLoad, depth + 1, onStack, memo, AA, LI, FAM);
        if (predResult.kind == LoadElimQueryResult::CLOBBERED ||
            predResult.kind == LoadElimQueryResult::UNKNOWN) {
            memo[BB] = LoadElimQueryResult::clobbered();
            return memo[BB];
        }
        if (predResult.kind == LoadElimQueryResult::KNOWN) {
            predValues.push_back({pred, predResult.value});
            if (firstVal == nullptr) {
                firstVal = predResult.value;
            } else if (predResult.value != firstVal) {
                allSame = false;
            }
        } else if (predResult.kind == LoadElimQueryResult::PHI_NEEDED) {
            memo[BB] = LoadElimQueryResult::clobbered();
            return memo[BB];
        }
    }
    if ((int)predValues.size() != (int)preds.size()) {
        memo[BB] = LoadElimQueryResult::clobbered();
        return memo[BB];
    }
    LoadElimQueryResult finalResult;
    if (allSame) {
        finalResult = LoadElimQueryResult::known(firstVal);
    } else {
        finalResult = LoadElimQueryResult::phiNeeded(std::move(predValues));
    }
    memo[BB] = finalResult;
    return finalResult;
}
//依次尝试块内转发、支配load复用和前驱值合并来处理单条load
bool LoadEliminationPass::processLoad(
    LoadInst *loadInst, BasicBlock *BB,
    DominatorTree &DT, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM, int &DominatingQueryBudget) {
    //4.单条 load 先查询块内最近定义，这是成本最低且无需修改 SSA 的路径。
    Value *P = loadInst ? loadInst->getPointerOperand() : nullptr;
    if (!P) return false;
    LoadElimQueryResult result = scanBlockBackward(BB, P, loadInst, loadInst, AA, LI, FAM);
    if (result.kind == LoadElimQueryResult::KNOWN) {
        loadInst->replaceAllUsesWith(result.value);
        delete loadInst;
        return true;
    }
    if (result.kind != LoadElimQueryResult::NEED_PREDECESSORS) {
        return false;
    }
    //4.1块内无定义时，优先尝试有总预算限制的跨回边支配 load 复用。
    if (DominatingQueryBudget > 0) {
        --DominatingQueryBudget;
        if (LoadInst *available = findAvailableDominatingLoad(
                loadInst, BB, DT, AA, LI, FAM)) {
            loadInst->replaceAllUsesWith(available);
            delete loadInst;
            return true;
        }
    }
    //4.2常规前驱求解要求每条入边均得到已知值；全相同直接替换，否则在块首
    //合成 PHI，再用该 PHI 替换 load。
    auto preds = BB->getPredecessors();
    if (preds.empty()) return false;
    std::unordered_set<BasicBlock*> onStack;
    std::unordered_map<BasicBlock*, LoadElimQueryResult> memo;
    std::vector<std::pair<BasicBlock*, Value*>> predValues;
    bool allSame = true;
    Value *firstVal = nullptr;
    for (BasicBlock *pred : preds) {
        LoadElimQueryResult predResult = queryAvailableValue(
            P, pred, loadInst, 1, onStack, memo, AA, LI, FAM);
        if (predResult.kind != LoadElimQueryResult::KNOWN) {
            return false;
        }
        predValues.push_back({pred, predResult.value});
        if (firstVal == nullptr) {
            firstVal = predResult.value;
        } else if (predResult.value != firstVal) {
            allSame = false;
        }
    }
    if ((int)predValues.size() != (int)preds.size()) {
        return false;
    }
    if (allSame) {
        loadInst->replaceAllUsesWith(firstVal);
        delete loadInst;
        return true;
    }
    PhiInst *phi = new PhiInst(loadInst->getIRType());
    for (auto &[pred, val] : predValues) {
        phi->addIncoming(val, pred);
    }
    BB->pushFront(phi);
    loadInst->replaceAllUsesWith(phi);
    delete loadInst;
    return true;
}
//按支配树前序处理MemorySSA快速层未消除的剩余load
bool LoadEliminationPass::processFunction(
    Function &F, DominatorTree &DT,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    //5.按支配树前序处理基本块，使支配侧先完成替换；每块先快照 load，避免删除
    //当前指令破坏遍历，并共享函数级跨回边查询预算。
    bool changed = false;
    int DominatingQueryBudget = MaxDominatingQueriesPerFunction;
    std::vector<DominatorNode *> Roots;
    if (DT.Root) Roots.push_back(DT.Root);
    auto DomOrder =
        buildPreOrderFromRootsCustomSucc<DominatorNode>(
            Roots, [&DT](DominatorNode *Node) {
                if (!Node || !Node->Block)
                    return std::vector<DominatorNode *>{};
                return DT.getChildNodes(Node);
            });
    for (DominatorNode *node : DomOrder) {
        if (!node) continue;
        BasicBlock *BB = node->Block;
        if (!BB) continue;
        auto loads = collectInstructions<LoadInst>(*BB);
        for (LoadInst *LI2 : loads) {
            if (LI2->getParent() != BB) continue;
            changed |= processLoad(LI2, BB, DT, AA, LI, FAM,
                                   DominatingQueryBudget);
        }
    }
    return changed;
}
//证明规范循环至少执行一次，避免把首轮load无条件放到零次循环的预头。
bool LoadEliminationPass::isKnownNonZeroTrip(
    const LoopTripCountInfo &Trip,
    const InductionDescriptor &Induction,
    RangeInfo &RI, DominatorTree &DT) {
    //6.1 常量次数直接判定；规范尾部测试循环从预头进入Header后必执行首轮。
    if (!Trip.Valid || !Trip.PreHeader || !Trip.Header || !Trip.Latch ||
        !Trip.ExitCmp || !Induction.Valid || !Induction.Phi ||
        !Induction.StartValue) {
        return false;
    }
    if (Trip.ConstantTripCount)
        return *Trip.ConstantTripCount > 0;
    if (Trip.ExitingBlock == Trip.Latch &&
        Trip.ComparePhase == LoopExitComparePhase::BackedgeUpdate) {
        return true;
    }
    //6.2 头部测试的运行时次数必须由预头路径范围证明首次继续条件恒真；仅知道
    //循环有限并不足以安全投机一次解引用。
    if (Trip.ExitingBlock != Trip.Header ||
        Trip.ComparePhase != LoopExitComparePhase::HeaderPhi) {
        return false;
    }
    BinaryInst::Operation Predicate = Trip.ContinueOnTrue
        ? Trip.ExitCmp->getOp()
        : BinaryInst::negatePredicate(Trip.ExitCmp->getOp());
    Value *Left = Trip.ExitCmp->getOperand(0);
    Value *Right = Trip.ExitCmp->getOperand(1);
    if (Right == Induction.Phi) {
        std::swap(Left, Right);
        Predicate = BinaryInst::reversePredicate(Predicate);
    }
    if (Left != Induction.Phi || !Right) return false;
    IntRange Start = RI.query(Induction.StartValue, Trip.PreHeader, DT);
    IntRange Bound = RI.query(Right, Trip.PreHeader, DT);
    if (Start.isBottom() || Bound.isBottom()) return false;
    switch (Predicate) {
    case BinaryInst::L:
        return Start.max < Bound.min;
    case BinaryInst::LE:
        return Start.max <= Bound.min;
    case BinaryInst::G:
        return Start.min > Bound.max;
    case BinaryInst::GE:
        return Start.min >= Bound.max;
    case BinaryInst::E:
        return Start.min == Start.max && Bound.min == Bound.max &&
               Start.min == Bound.min;
    case BinaryInst::NE:
        return Start.max < Bound.min || Bound.max < Start.min;
    default:
        return false;
    }
}
//验证循环中不存在能够覆盖候选地址的其他写入或不可跨越调用。
bool LoadEliminationPass::hasNoInterferingWrite(
    const LoopCarriedForwardingCandidate &Candidate,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    if (!Candidate.TargetLoop || !Candidate.Load || !Candidate.Store)
        return false;
    MemoryLocation LoadLocation = MemoryLocation::get(Candidate.Load);
    MemoryLocation StoreLocation = MemoryLocation::get(Candidate.Store);
    for (BasicBlock *BB :
         Candidate.TargetLoop->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I || I == Candidate.Store) continue;
            //6.3 调用即使不写程序内存也可能具有I/O、计时或故障顺序，当前变换
            //不把首轮解引用跨越任何调用。
            if (dynamic_cast<CallInst *>(I)) return false;
            if (mayModifyPointer(LoadLocation, I, AA, LI, FAM) ||
                mayModifyPointer(StoreLocation, I, AA, LI, FAM)) {
                return false;
            }
        }
    }
    return true;
}
//递归把首轮地址表达式物化到预头，循环Header PHI替换成对应入口值。
Value *LoadEliminationPass::materializeFirstIterationValue(
    Value *Original, const LoopCarriedForwardingCandidate &Candidate,
    DominatorTree &DT,
    std::unordered_map<Value *, Value *> &ValueMap,
    std::unordered_set<Value *> &Active,
    std::vector<Instruction *> &Created, unsigned Depth) {
    if (!Original || Value::isUndef(Original) ||
        !Candidate.TargetLoop || !Candidate.PreHeader || Depth > 32) {
        return nullptr;
    }
    auto Known = ValueMap.find(Original);
    if (Known != ValueMap.end()) return Known->second;
    if (Original->isConst() || Original->isParam() ||
        Original->isGlobal()) {
        ValueMap.emplace(Original, Original);
        return Original;
    }
    auto *OriginalInst = dynamic_cast<Instruction *>(Original);
    Instruction *PreHeaderTerminator = Candidate.PreHeader->back();
    if (!OriginalInst || !OriginalInst->getParent() ||
        !PreHeaderTerminator) {
        return nullptr;
    }
    if (!Candidate.TargetLoop->contains(OriginalInst->getParent())) {
        if (!DT.dominatesValue(OriginalInst, PreHeaderTerminator))
            return nullptr;
        ValueMap.emplace(Original, Original);
        return Original;
    }
    //6.4 Header PHI在首轮的值严格取唯一预头incoming；其他循环PHI不猜测。
    if (auto *Phi = dynamic_cast<PhiInst *>(OriginalInst)) {
        if (Phi->getParent() != Candidate.TargetLoop->getHeader())
            return nullptr;
        Value *Incoming = nullptr;
        unsigned IncomingCount = 0;
        forEachPhiIncoming(
            Phi, [&](unsigned, Value *ValueIncoming, BasicBlock *Pred) {
                if (Pred == Candidate.PreHeader) {
                    Incoming = ValueIncoming;
                    ++IncomingCount;
                }
                return true;
            });
        if (IncomingCount != 1 || !Incoming) return nullptr;
        Value *Mapped = materializeFirstIterationValue(
            Incoming, Candidate, DT, ValueMap, Active, Created,
            Depth + 1);
        if (Mapped) ValueMap.emplace(Original, Mapped);
        return Mapped;
    }
    bool Supported = dynamic_cast<GetElementPtrInst *>(OriginalInst) ||
                     dynamic_cast<BitCastInst *>(OriginalInst) ||
                     dynamic_cast<ZextInst *>(OriginalInst) ||
                     dynamic_cast<SextInst *>(OriginalInst);
    if (auto *Binary = dynamic_cast<BinaryInst *>(OriginalInst)) {
        Supported = Binary->getOp() == BinaryInst::ADD ||
                    Binary->getOp() == BinaryInst::SUB ||
                    Binary->getOp() == BinaryInst::MUL;
    }
    if (!Supported ||
        Created.size() >= MaxInitialAddressInstructions) {
        return nullptr;
    }
    ScopedActiveMark ActiveMark(Active, Original);
    if (!ActiveMark.inserted()) return nullptr;
    for (const auto &OperandUse : OriginalInst->getUserList()) {
        Value *Operand = OperandUse ? OperandUse->getValue() : nullptr;
        Value *Mapped = materializeFirstIterationValue(
            Operand, Candidate, DT, ValueMap, Active, Created,
            Depth + 1);
        if (!Mapped) return nullptr;
        ValueMap[Operand] = Mapped;
    }
    if (auto *Binary = dynamic_cast<BinaryInst *>(OriginalInst)) {
        Value *Left = nullptr;
        Value *Right = nullptr;
        if (!Binary->getOperands(Left, Right)) return nullptr;
        auto MappedLeft = ValueMap.find(Left);
        auto MappedRight = ValueMap.find(Right);
        if (MappedLeft == ValueMap.end() ||
            MappedRight == ValueMap.end()) {
            return nullptr;
        }
        Value *Folded = BinaryInst::foldConstants(
            Binary->getOp(), MappedLeft->second,
            MappedRight->second, true);
        if (Folded) {
            ValueMap.emplace(Original, Folded);
            return Folded;
        }
    }
    //6.5 只有删除目标load后会随之变死的单用户地址链才允许复制；因此运行时
    //仅一轮时，预头地址计算替代原地址计算而不会额外增加动态算术。
    if (OriginalInst->getUses().size() != 1 ||
        Created.size() >= MaxInitialAddressInstructions) {
        return nullptr;
    }
    Instruction *Cloned = OriginalInst->clone(ValueMap);
    if (!Cloned ||
        !Candidate.PreHeader->insertBeforeTerminator(Cloned)) {
        delete Cloned;
        return nullptr;
    }
    Created.push_back(Cloned);
    ValueMap.emplace(Original, Cloned);
    return Cloned;
}
//为单个规范叶子循环收集一个精确距离一的跨迭代转发候选。
bool LoadEliminationPass::collectLoopCarriedForwardingCandidate(
    Loop *L, LoopAccessInfo &LA, SCEVResult &SE, RangeInfo &RI,
    DominatorTree &DT, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    LoopCarriedForwardingCandidate &Candidate) {
    Candidate = LoopCarriedForwardingCandidate{};
    if (!L || !L->isInnermost() ||
        L->getBackedgeSrcs().size() != 1) {
        return false;
    }
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch || Latch != L->getBackedgeSrcs().front() ||
        !PreHeader->hasTerminator() || !Latch->hasTerminator()) {
        return false;
    }
    auto Trip = SE.analyzeLoopTripCount(L);
    auto Induction = SE.getCanonicalInduction(L);
    std::vector<BasicBlock *> ExitBlocks = L->getExitBlocks();
    if (!Trip || !Induction || !Trip->Valid || !Induction->Valid ||
        !Trip->ExitingBlock || !Trip->ExitBlock ||
        ExitBlocks.size() != 1 || ExitBlocks.front() != Trip->ExitBlock ||
        !DT.dominates(Trip->ExitingBlock, Latch) ||
        !SE.isKnownFiniteLoop(L, RI, DT) ||
        !isKnownNonZeroTrip(*Trip, *Induction, RI, DT) ||
        (Trip->ConstantTripCount && *Trip->ConstantTripCount < 2)) {
        return false;
    }
    const LoopAccessSummary &Summary = LA.getSummary(L);
    if (Summary.HasUnknownMemory) return false;
    std::vector<LoadInst *> Loads;
    std::vector<StoreInst *> Stores;
    for (const LoopMemoryAccess *Access : Summary.Accesses) {
        if (!Access || !Access->Inst) continue;
        if (auto *Load = dynamic_cast<LoadInst *>(Access->Inst))
            Loads.push_back(Load);
        if (auto *Store = dynamic_cast<StoreInst *>(Access->Inst))
            Stores.push_back(Store);
    }
    std::size_t ExaminedPairs = 0;
    for (LoadInst *Load : Loads) {
        if (!Load || !Load->getParent() || !Load->hasUses()) continue;
        StoreInst *UniqueStore = nullptr;
        for (StoreInst *Store : Stores) {
            if (++ExaminedPairs > MaxLoopCarriedPairs) return false;
            if (!Store || !Store->getParent() ||
                !Store->fullyCoversItsPointer() ||
                !isTypeCompatible(Store->getValueOperand(), Load)) {
                continue;
            }
            auto Distance = LA.getConstantDependenceDistance(
                Store, Load, L, L);
            auto StoreStride = LA.getByteStride(Store, L, L);
            auto LoadStride = LA.getByteStride(Load, L, L);
            if (!Distance || *Distance != 1 || !StoreStride ||
                !LoadStride || *StoreStride == 0 ||
                *StoreStride != *LoadStride) {
                continue;
            }
            if (UniqueStore && UniqueStore != Store) {
                UniqueStore = nullptr;
                break;
            }
            UniqueStore = Store;
        }
        if (!UniqueStore) continue;
        Value *StoredValue = UniqueStore->getValueOperand();
        Instruction *LatchTerminator = Latch->back();
        if (!StoredValue || !LatchTerminator ||
            !DT.dominates(Load->getParent(), Latch) ||
            !DT.dominates(UniqueStore->getParent(), Latch) ||
            !DT.dominatesValue(StoredValue, LatchTerminator)) {
            continue;
        }
        LoopCarriedForwardingCandidate Proposed;
        Proposed.TargetLoop = L;
        Proposed.PreHeader = PreHeader;
        Proposed.Latch = Latch;
        Proposed.Load = Load;
        Proposed.Store = UniqueStore;
        if (!hasNoInterferingWrite(Proposed, AA, LI, FAM))
            continue;
        Candidate = Proposed;
        return true;
    }
    return false;
}
//插入首轮load和循环携带PHI，用上一轮store值替换当前轮相邻地址load。
bool LoadEliminationPass::transformLoopCarriedForwarding(
    const LoopCarriedForwardingCandidate &Candidate,
    DominatorTree &DT) {
    if (!Candidate.TargetLoop || !Candidate.PreHeader ||
        !Candidate.Latch || !Candidate.Load || !Candidate.Store ||
        !Candidate.Load->getParent() || !Candidate.Store->getParent()) {
        return false;
    }
    std::unordered_map<Value *, Value *> ValueMap;
    std::unordered_set<Value *> Active;
    std::vector<Instruction *> Created;
    Value *InitialPointer = materializeFirstIterationValue(
        Candidate.Load->getPointerOperand(), Candidate, DT,
        ValueMap, Active, Created, 0);
    auto Rollback = [&]() {
        for (auto It = Created.rbegin(); It != Created.rend(); ++It)
            if (*It && (*It)->getParent()) delete *It;
    };
    if (!InitialPointer) {
        Rollback();
        return false;
    }
    auto *InitialLoad = new LoadInst(InitialPointer);
    if (!isTypeCompatible(InitialLoad, Candidate.Load) ||
        !Candidate.PreHeader->insertBeforeTerminator(InitialLoad)) {
        delete InitialLoad;
        Rollback();
        return false;
    }
    //6.6 PHI的预头incoming是原首轮load，回边incoming取store当前写入值；先替换
    //load用户再读取store值，可正确处理store值本身就是该load的退化复制。
    auto *Carried = new PhiInst(Candidate.Load->getIRType());
    Carried->addIncoming(InitialLoad, Candidate.PreHeader);
    Candidate.TargetLoop->getHeader()->pushFront(Carried);
    Candidate.Load->replaceAllUsesWith(Carried);
    Value *BackedgeValue = Candidate.Store->getValueOperand();
    if (!BackedgeValue ||
        !isTypeCompatible(BackedgeValue, Candidate.Load)) {
        //预检已保证此分支不可达；若IR接口返回异常，保守保留原load并撤销新增值。
        Carried->replaceAllUsesWith(Candidate.Load);
        delete Carried;
        delete InitialLoad;
        Rollback();
        return false;
    }
    Carried->addIncoming(BackedgeValue, Candidate.Latch);
    delete Candidate.Load;
    return true;
}
//按循环后序尝试一次距离一转发，成功后停止消费已失效的访问分析。
bool LoadEliminationPass::processOneLoopCarriedForwarding(
    const LoopInfo &LI, LoopAccessInfo &LA, SCEVResult &SE,
    RangeInfo &RI, DominatorTree &DT, BasicAAResult &AA,
    FunctionAnalysisPassManager &FAM) {
    for (Loop *L : LI.getLoopsInPostOrder()) {
        LoopCarriedForwardingCandidate Candidate;
        if (!collectLoopCarriedForwardingCandidate(
                L, LA, SE, RI, DT, AA, LI, FAM, Candidate)) {
            continue;
        }
        if (transformLoopCarriedForwarding(Candidate, DT))
            return true;
    }
    return false;
}
//运行MemorySSA快速层与CFG后备层并精确声明分析失效
PreservationStatus LoadEliminationPass::run(Function &F,
                                            FunctionAnalysisPassManager &FAM) {
    //1.获取别名、循环、支配和 MemorySSA，分别服务内存冲突、回边与定义链证明。
    BasicAAResult *AA = &FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    //2.在常规load消除前尝试一次严格的循环距离一转发。成功后地址、SCEV和内存
    //分析都已过期，立即返回；流水线后续LoadElimination实例会继续常规清理。
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &LA = FAM.getResult<LoopAccessAnalysis>(F);
    if (processOneLoopCarriedForwarding(
            LI, LA, SE, RI, DT, *AA, FAM)) {
        PreservationStatus PA;
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.discard<BasicAliasAnalysis>();
        PA.discard<MemorySSAAnalysis>();
        PA.discard<SideEffectAnalysis>();
        PA.discard<SCEVAnalysis>();
        PA.discard<RangeAnalysis>();
        PA.discard<LoopDependenceAnalysis>();
        PA.discard<LoopAccessAnalysis>();
        return PA;
    }
    //3.快速层：在查询预算内让MemorySSA汇总每条路径的最近clobber。若这些定义
    //全是MustAlias完整覆盖Store且写入同一个支配值，则直接计划转发；否则保留
    //完整clobber签名来筛选支配load，再通过CFG区间证明确认可复用。
    //MemorySSA只表示程序内存版本，后续接受候选时还要单独检查顺序屏障。
    auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
    constexpr std::size_t MaxMSSAQueries = 512;
    constexpr std::size_t MaxMSSAWalk = 256;
    std::vector<std::pair<LoadInst *, Value *>> MSSAReplacements;
    std::vector<MSSALoadRecord> MSSALoadRecords;
    std::size_t QueryCount = 0;
    for (LoadInst *Load : collectInstructions<LoadInst>(F)) {
        if (QueryCount++ >= MaxMSSAQueries) break;
        BasicBlock *BB = Load->getParent();
        if (!BB) continue;
        MemoryClobberResult ClobberResult = MSSA.getClobberingAccesses(
            Load, MemoryLocation::get(Load), *AA, LI, FAM,
            MaxMSSAWalk);
        if (!ClobberResult.Complete ||
            ClobberResult.Clobbers.empty())
            continue;
        MSSALoadRecords.push_back(
            {Load, BB, std::move(ClobberResult.Clobbers)});
    }
    for (const MSSALoadRecord &Record : MSSALoadRecords) {
        LoadInst *Load = Record.Load;
        BasicBlock *BB = Record.Block;
        if (!Load || !BB || Load->getParent() != BB) continue;
        MemoryLocation LoadLocation = MemoryLocation::get(Load);
        //2.1MemoryPhi后的不同路径可以由不同Store定义；只要所有首个写均完整
        //覆盖同一位置、写入同一个SSA值，并且该值支配load，就无需让任一Store
        //自身支配汇合块。
        Value *CommonStoredValue = nullptr;
        bool AllClobbersAreSameValueStores = true;
        for (MemoryAccess *Clobber : Record.Clobbers) {
            auto *Store = Clobber && Clobber->isDef()
                              ? dynamic_cast<StoreInst *>(
                                    Clobber->getInstruction())
                              : nullptr;
            Value *Stored = Store ? Store->getValueOperand() : nullptr;
            if (!Store || !Store->getParent() ||
                !Store->fullyCoversItsPointer() ||
                !isTypeCompatible(Stored, Load) ||
                AA->aliasWithIndexRange(
                    LoadLocation, MemoryLocation::get(Store), LI) !=
                    AliasInfo::MustAlias ||
                hasOrderingBarrierOnPath(F, Store, Load, FAM)) {
                AllClobbersAreSameValueStores = false;
                break;
            }
            if (!CommonStoredValue) {
                CommonStoredValue = Stored;
            } else if (CommonStoredValue != Stored) {
                AllClobbersAreSameValueStores = false;
                break;
            }
        }
        if (AllClobbersAreSameValueStores && CommonStoredValue &&
            DT.dominatesValue(CommonStoredValue, Load)) {
            MSSAReplacements.push_back({Load, CommonStoredValue});
            continue;
        }
        //2.2相同clobber集合只说明两个load反向可达同一批静态MemoryDef；
        //循环中同一Store节点可能分别代表上一轮与当前轮写入，因此它只是候选筛选，
        //还必须证明从支配load到当前load的每条CFG路径都没有相关写入。
        //同块要求严格指令顺序并扫描中间指令，跨块复用已有全路径证明。
        std::size_t Compared = 0;
        for (auto It = MSSALoadRecords.rbegin();
             It != MSSALoadRecords.rend(); ++It) {
            LoadInst *Candidate = It->Load;
            BasicBlock *CandidateBB = It->Block;
            if (!Candidate || Candidate == Load || !CandidateBB ||
                Candidate->getParent() != CandidateBB ||
                It->Clobbers != Record.Clobbers ||
                !isTypeCompatible(Candidate, Load) ||
                !DT.dominates(CandidateBB, BB) ||
                (CandidateBB == BB &&
                 !Instruction::comesBefore(Candidate, Load)))
                continue;
            if (Compared++ >= MaxMSSALoadComparisons) break;
            if (AA->aliasWithIndexRange(
                    LoadLocation, MemoryLocation::get(Candidate), LI) !=
                AliasInfo::MustAlias)
                continue;
            bool Unmodified = true;
            if (CandidateBB == BB) {
                Instruction *Cursor = Candidate->getNext();
                for (; Cursor && Cursor != Load; Cursor = Cursor->getNext()) {
                    if (mayModifyPointer(
                            LoadLocation, Cursor, *AA, LI, FAM)) {
                        Unmodified = false;
                        break;
                    }
                }
                if (Cursor != Load) Unmodified = false;
            } else {
                Unmodified = isUnmodifiedOnAllPaths(
                    Candidate, Load, LoadLocation.Pointer,
                    DT, *AA, LI, FAM);
            }
            if (!Unmodified) continue;
            MSSAReplacements.push_back({Load, Candidate});
            break;
        }
    }
    //2.3先解析计划之间的传递依赖并检测环，再批量替换use、批量删除load，
    //避免先删一个 load 后让另一条计划持有悬空替换值。
    std::unordered_map<LoadInst *, Value *> Planned;
    for (const auto &Replacement : MSSAReplacements)
        Planned.emplace(Replacement.first, Replacement.second);
    std::function<Value *(Value *, std::unordered_set<LoadInst *> &)>
        ResolveReplacement = [&](Value *V,
                                 std::unordered_set<LoadInst *> &Active)
            -> Value * {
        auto *AsLoad = dynamic_cast<LoadInst *>(V);
        auto It = AsLoad ? Planned.find(AsLoad) : Planned.end();
        if (It == Planned.end()) return V;
        ScopedActiveMark ActiveMark(Active, AsLoad);
        if (!ActiveMark.inserted()) return nullptr;
        return ResolveReplacement(It->second, Active);
    };
    std::vector<std::pair<LoadInst *, Value *>> ResolvedReplacements;
    ResolvedReplacements.reserve(MSSAReplacements.size());
    for (const auto &Replacement : MSSAReplacements) {
        LoadInst *Load = Replacement.first;
        if (!Load || !Load->getParent()) continue;
        std::unordered_set<LoadInst *> Active;
        Value *Resolved = ResolveReplacement(Replacement.second, Active);
        if (!Resolved || Resolved == Load ||
            !isTypeCompatible(Resolved, Load))
            continue;
        ResolvedReplacements.push_back({Load, Resolved});
    }
    bool changed = false;
    for (const auto &Replacement : ResolvedReplacements)
        Replacement.first->replaceAllUsesWith(Replacement.second);
    for (const auto &Replacement : ResolvedReplacements) {
        LoadInst *Load = Replacement.first;
        if (!Load || !Load->getParent()) continue;
        delete Load;
        changed = true;
    }
    //2.4快速层删除load后，旧AA缓存和MemorySSA节点仍引用修改前的Value/use图；
    //在进入后备扫描前精确失效并重建AA，结构分析因CFG未变可以继续保留。
    if (changed) {
        PreservationStatus RebuildPA;
        RebuildPA.keep<DominantAnalysis>();
        RebuildPA.keep<PostDominantAnalysis>();
        RebuildPA.keep<LoopAnalysis>();
        RebuildPA.discard<BasicAliasAnalysis>();
        RebuildPA.discard<MemorySSAAnalysis>();
        RebuildPA.discard<SideEffectAnalysis>();
        FAM.invalidate(F, std::move(RebuildPA));
        AA = &FAM.getResult<BasicAliasAnalysis>(F);
    }
    //3—5. 对快速层未覆盖的 load 运行块内、前驱数据流与跨回边支配证明。
    changed |= processFunction(F, DT, *AA, LI, FAM);
    if (!changed) {
        return PreservationStatus::all();
    }
    //6.CFG 未变，结构分析可保留；load 消失改变别名缓存、MemorySSA 和副作用摘要。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<MemorySSAAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
