// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/InvariantPrivateStoreHoist.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/BlockFrequencyInfo.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/TargetCostAnalysis.hpp"
#include "ir/opt/DCE.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>
 
//只依据已注册内建memset的语义、volatile标记和对象类型判断完整清零
bool InvariantPrivateStoreHoistPass::isWholeZeroMemset(
    CallInst *CI, AllocaInst *AI) {
    if (!CI || !AI || CI->getArgCount() < 4 ||
        !CI->getBuiltinCallee() ||
        !BuildInFunction::isMemsetName(CI->getCalleeName()))
        return false;
    int32_t FillValue = 0;
    int32_t IsVolatile = 1;
    int64_t ByteCount = 0;
    if (!CI->getArg(1) ||
        !CI->getArg(1)->tryGetIntConst(FillValue) || FillValue != 0 ||
        !CI->getArg(2) ||
        !CI->getArg(2)->tryGetInt64Const(ByteCount) || ByteCount < 0 ||
        !CI->getArg(3) ||
        !CI->getArg(3)->tryGetIntConst(IsVolatile) || IsVolatile != 0)
        return false;
    auto ObjectType = AllocaInst::getAllocatedType(AI);
    if (!ObjectType ||
        static_cast<uint64_t>(ByteCount) !=
            static_cast<uint64_t>(ObjectType->getSize()))
        return false;
    Value *Pointer = CI->getArg(0);
    std::unordered_set<Value *> Seen;
    while (Pointer && Seen.insert(Pointer).second) {
        if (auto *Cast = dynamic_cast<BitCastInst *>(Pointer)) {
            Pointer = Cast->getUnaryOperand();
            continue;
        }
        auto *GEP = dynamic_cast<GetElementPtrInst *>(Pointer);
        if (!GEP) break;
        for (Value *Index : GEP->getIndices()) {
            int ConstantIndex = 0;
            if (!Value::tryGetIntIndex(Index, ConstantIndex) ||
                ConstantIndex != 0)
                return false;
        }
        Pointer = GEP->getBasePointer();
    }
    return Pointer == AI;
}

//不移动任何可能陷阱的表达式；候选值必须已在原预头中求值
bool InvariantPrivateStoreHoistPass::isInvariantAvailableAtPreHeader(
    Value *V, Loop *L, BasicBlock *PreHeader,
    const DominatorTree &DT) {
    if (!V || !L || !PreHeader || Value::isUndef(V) ||
        valueMayDependOnUndef(V))
        return false;
    if (V->isConst() || V->isParam() || V->isGlobal()) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    Instruction *InsertPoint = PreHeader->getBranchTerminator();
    return I && I->getParent() && !L->contains(I->getParent()) &&
           InsertPoint && DT.dominatesValue(I, InsertPoint);
}

//循环内定义只有在支配原use、无副作用且全部操作数同样稳定时才可重复省略
bool InvariantPrivateStoreHoistPass::isStablePrefixValue(
    Value *V, Instruction *UsePoint, Loop *L, LoopInfo &LI,
    const DominatorTree &DT, std::unordered_set<Value *> &Active,
    std::unordered_set<Value *> &Proven, int &Budget) {
    if (!V || !UsePoint || !L || Value::isUndef(V) ||
        valueMayDependOnUndef(V, 512) || ++Budget > 512)
        return false;
    if (V->isConst() || V->isParam() || V->isGlobal()) return true;
    if (Proven.count(V)) return true;
    if (!Active.insert(V).second) return false;
    auto Finish = [&](bool Result) {
        Active.erase(V);
        if (Result) Proven.insert(V);
        return Result;
    };
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || !DT.dominatesValue(I, UsePoint))
        return Finish(false);
    if (!L->contains(I->getParent())) return Finish(true);
    if (LI.getLoopFor(I->getParent()) != L ||
        dynamic_cast<PhiInst *>(I) || dynamic_cast<LoadInst *>(I) ||
        dynamic_cast<StoreInst *>(I) || dynamic_cast<CallInst *>(I) ||
        dynamic_cast<BrInst *>(I) || dynamic_cast<RetInst *>(I) ||
        dynamic_cast<AllocaInst *>(I) ||
        dceInstructionMustBePreserved(I) ||
        instructionMayFaultOrProducePoison(I))
        return Finish(false);
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (dynamic_cast<BasicBlock *>(Operand)) return Finish(false);
        if (!isStablePrefixValue(
                Operand, I, L, LI, DT, Active, Proven, Budget))
            return Finish(false);
    }
    return Finish(true);
}

//唯一前驱链保证路径谓词是必要且充分的，而不是根据概率猜测执行路径
bool InvariantPrivateStoreHoistPass::collectPredicatePath(
    BasicBlock *Block, Loop *L, BasicBlock *PreHeader,
    const DominatorTree &DT,
    std::vector<InvariantPrivateStorePredicateEdge> &Path) {
    Path.clear();
    if (!Block || !L || !L->getHeader() || !L->contains(Block))
        return false;
    std::vector<InvariantPrivateStorePredicateEdge> ReversePath;
    std::unordered_set<BasicBlock *> Seen;
    BasicBlock *Current = Block;
    while (Current != L->getHeader()) {
        if (!Current || !Seen.insert(Current).second ||
            ReversePath.size() >= 128)
            return false;
        std::vector<BasicBlock *> InternalPredecessors;
        for (BasicBlock *Pred : Current->getPredecessors())
            if (Pred && L->contains(Pred))
                InternalPredecessors.push_back(Pred);
        if (InternalPredecessors.size() != 1) return false;
        BasicBlock *Pred = InternalPredecessors.front();
        auto *Branch = Pred->getBranchTerminator();
        if (!Branch || Pred->countEdgesToSucc(Current) != 1)
            return false;
        if (Branch->isConditional()) {
            Value *Condition = Branch->getCondition();
            if (!Condition || !Condition->getIRType() ||
                !Condition->getIRType()->isBool() ||
                !isInvariantAvailableAtPreHeader(
                    Condition, L, PreHeader, DT))
                return false;
            bool TakeTrue = Branch->getTrueSuccessor() == Current;
            bool TakeFalse = Branch->getFalseSuccessor() == Current;
            if (TakeTrue == TakeFalse) return false;
            ReversePath.push_back({Condition, TakeTrue});
        } else if (Branch->getSuccessorCount() != 1 ||
                   Branch->getSuccessor(0) != Current) {
            return false;
        }
        Current = Pred;
    }
    Path.assign(ReversePath.rbegin(), ReversePath.rend());
    return true;
}

//指针闭包不允许PHI/select合流、传参、返回或存储指针本身
bool InvariantPrivateStoreHoistPass::collectPrivateAggregateUses(
    Value *Pointer, AllocaInst *Root, Loop *L,
    BasicAAResult &AA, std::unordered_set<Value *> &Seen,
    std::vector<LoadInst *> &Loads,
    std::vector<StoreInst *> &Stores,
    CallInst *&ZeroInitializer, int &Budget) {
    if (!Pointer || !Root || !L || ++Budget > 512) return false;
    if (!Seen.insert(Pointer).second) return true;
    for (Use *U : Pointer->getUses()) {
        if (!U || U->getValue() != Pointer) continue;
        auto *I = dynamic_cast<Instruction *>(U->getUser());
        if (!I) return false;
        int OperandIndex = I->getUserIndex(U);
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I)) {
            if (OperandIndex != 0 ||
                AA.getUniqueUnderlyingObject(GEP) != Root ||
                !collectPrivateAggregateUses(
                    GEP, Root, L, AA, Seen, Loads, Stores,
                    ZeroInitializer, Budget))
                return false;
            continue;
        }
        if (auto *Cast = dynamic_cast<BitCastInst *>(I)) {
            if (OperandIndex != 0 ||
                AA.getUniqueUnderlyingObject(Cast) != Root ||
                !collectPrivateAggregateUses(
                    Cast, Root, L, AA, Seen, Loads, Stores,
                    ZeroInitializer, Budget))
                return false;
            continue;
        }
        if (auto *Load = dynamic_cast<LoadInst *>(I)) {
            if (OperandIndex != 0 || !L->contains(Load->getParent()) ||
                AA.getUniqueUnderlyingObject(
                    Load->getPointerOperand()) != Root)
                return false;
            Loads.push_back(Load);
            continue;
        }
        if (auto *Store = dynamic_cast<StoreInst *>(I)) {
            if (OperandIndex != 1 || !L->contains(Store->getParent()) ||
                !Store->fullyCoversItsPointer() ||
                AA.getUniqueUnderlyingObject(
                    Store->getPointerOperand()) != Root)
                return false;
            Stores.push_back(Store);
            continue;
        }
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            if (CI->getArgIndex(U) != 0 ||
                !isWholeZeroMemset(CI, Root) ||
                (ZeroInitializer && ZeroInitializer != CI))
                return false;
            ZeroInitializer = CI;
            continue;
        }
        return false;
    }
    return true;
}

//只处理能用完整证明链覆盖的私有聚合，任一根访问未建模即整体拒绝
bool InvariantPrivateStoreHoistPass::buildPlan(
    Loop *L, LoopInfo &LI, SCEVResult &SE, RangeInfo &RI,
    BasicAAResult &AA, DominatorTree &DT,
    const PostDominatorTree &PDT, const BlockFrequencyInfo &BFI,
    const TargetCostInfo &TargetCost,
    InvariantPrivateStoreHoistPlan &Plan) {
    Plan = {};
    if (!L || !L->getHeader() || L->getParentLoop()) return false;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch || !PreHeader->getBranchTerminator())
        return false;
    auto Trip = SE.analyzeLoopTripCount(L);
    //首版只接受从预头进入后必定至少执行一次的有限顶层循环，避免在
    //零次路径消费原本未被分支选择的值，也保证清零基线不会跨父迭代复用。
    const bool KnownPositiveTrip = Trip && Trip->Valid &&
        (!Trip->MayBeZeroTrip ||
         (Trip->ConstantTripCount && *Trip->ConstantTripCount > 0) ||
         (Trip->ExitingBlock == Latch &&
          Trip->ComparePhase == LoopExitComparePhase::BackedgeUpdate));
    if (!KnownPositiveTrip || !SE.isKnownFiniteLoop(L, RI, DT))
        return false;
    if (Trip->ConstantTripCount && *Trip->ConstantTripCount <= 1)
        return false;

    std::map<AllocaInst *, std::vector<StoreInst *>> StoreGroups;
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB || LI.getLoopFor(BB) != L) continue;
        for (Instruction *I : *BB) {
            auto *Store = dynamic_cast<StoreInst *>(I);
            if (!Store) continue;
            auto *Root = dynamic_cast<AllocaInst *>(
                AA.getUniqueUnderlyingObject(Store->getPointerOperand()));
            if (!Root || !AllocaInst::isArrayAllocation(Root)) continue;
            StoreGroups[Root].push_back(Store);
        }
    }
    for (auto &Group : StoreGroups) {
        AllocaInst *Root = Group.first;
        auto &DirectStores = Group.second;
        if (!Root || DirectStores.empty() || DirectStores.size() > 128 ||
            !Root->getParent() || !Root->getParent()->getParent() ||
            Root->getParent()->getParent()->front() != Root->getParent())
            continue;
        std::unordered_set<Value *> Seen;
        std::vector<LoadInst *> Loads;
        std::vector<StoreInst *> AllStores;
        CallInst *Initializer = nullptr;
        int Budget = 0;
        if (!collectPrivateAggregateUses(
                Root, Root, L, AA, Seen, Loads, AllStores,
                Initializer, Budget) ||
            !Initializer || Loads.empty() ||
            AllStores.size() != DirectStores.size() ||
            L->contains(Initializer->getParent()) ||
            LI.getLoopFor(Initializer->getParent()) != nullptr ||
            !DT.dominatesValue(
                Initializer, PreHeader->getBranchTerminator()))
            continue;

        std::set<std::pair<int64_t, int64_t>> WrittenIntervals;
        std::vector<InvariantPrivateStoreCandidate> Candidates;
        std::unordered_set<StoreInst *> CandidateStoreSet;
        bool Valid = true;
        for (StoreInst *Store : AllStores) {
            std::unordered_set<Value *> ActiveValues;
            std::unordered_set<Value *> ProvenValues;
            int StableValueBudget = 0;
            if (!Store || LI.getLoopFor(Store->getParent()) != L ||
                !isInvariantAvailableAtPreHeader(
                    Store->getPointerOperand(), L, PreHeader, DT) ||
                !isStablePrefixValue(
                    Store->getValueOperand(), Store, L, LI, DT,
                    ActiveValues, ProvenValues, StableValueBudget) ||
                !AA.isLocationProvablyInBoundsAt(
                    MemoryLocation::get(Store),
                    PreHeader->getBranchTerminator(), SE, RI, DT)) {
                Valid = false;
                break;
            }
            auto StoredType = Store->getValueOperand()->getIRType();
            Value *Zero = Value::makeZeroForType(StoredType);
            if (!StoredType ||
                !(StoredType->isInt() || StoredType->isFloat() ||
                  StoredType->isBool()) ||
                !Zero || !IRType::hasSameShape(
                    Zero->getIRType(), StoredType)) {
                Valid = false;
                break;
            }
            auto Offset = AA.getConstantByteOffset(
                Store->getPointerOperand());
            std::size_t Size = Store->getValueOperand()->getIRType()
                ? Store->getValueOperand()->getIRType()->getSize() : 0;
            if (!Offset || *Offset < 0 || Size == 0 ||
                static_cast<uint64_t>(*Offset) >
                    static_cast<uint64_t>(
                        std::numeric_limits<int64_t>::max()) - Size) {
                Valid = false;
                break;
            }
            int64_t End = *Offset + static_cast<int64_t>(Size);
            for (const auto &Interval : WrittenIntervals)
                if (*Offset < Interval.second &&
                    Interval.first < End) {
                    Valid = false;
                    break;
                }
            if (!Valid) break;
            WrittenIntervals.emplace(*Offset, End);
            InvariantPrivateStoreCandidate Candidate;
            Candidate.Store = Store;
            if (!collectPredicatePath(
                    Store->getParent(), L, PreHeader, DT,
                    Candidate.Path)) {
                Valid = false;
                break;
            }
            Candidates.push_back(std::move(Candidate));
            CandidateStoreSet.insert(Store);
        }
        if (!Valid || Candidates.empty()) continue;

        //找到一个位于全部候选之后且位于全部读取之前的共同区域出口
        BasicBlock *RegionExit = nullptr;
        for (BasicBlock *BB : L->getBlocks()) {
            if (!BB || LI.getLoopFor(BB) != L) continue;
            bool AfterStores = PDT.postDominates(BB, L->getHeader());
            for (StoreInst *Store : AllStores)
                AfterStores &= PDT.postDominates(BB, Store->getParent());
            bool BeforeLoads = true;
            for (LoadInst *Load : Loads)
                BeforeLoads &= DT.dominates(BB, Load->getParent());
            if (AfterStores && BeforeLoads) {
                RegionExit = BB;
                break;
            }
        }
        if (!RegionExit || RegionExit == L->getHeader() ||
            RegionExit == Latch || LI.getLoopFor(RegionExit) != L ||
            !DT.dominates(RegionExit, Latch) ||
            !RegionExit->collectPhiNodes().empty() ||
            Latch->countEdgesToSucc(L->getHeader()) != 1 ||
            Latch->countEdgesToSucc(RegionExit) != 0)
            continue;
        std::vector<BasicBlock *> HeaderPredecessors =
            L->getHeader()->getPredecessors();
        if (HeaderPredecessors.size() != 2 ||
            std::find(HeaderPredecessors.begin(), HeaderPredecessors.end(),
                      PreHeader) == HeaderPredecessors.end() ||
            std::find(HeaderPredecessors.begin(), HeaderPredecessors.end(),
                      Latch) == HeaderPredecessors.end() ||
            PreHeader->countEdgesToSucc(L->getHeader()) != 1)
            continue;
        for (StoreInst *Store : AllStores)
            if (DT.dominates(RegionExit, Store->getParent()))
                Valid = false;
        if (!Valid) continue;

        //从Header沿全部可能路径遍历到共同汇合块。前缀必须无环、没有
        //旁路出口且不进入子循环，因此这里证明的是完整闭合区域而非热路径。
        std::unordered_set<BasicBlock *> PrefixBlocks;
        std::unordered_set<BasicBlock *> ActivePrefixBlocks;
        std::unordered_set<BasicBlock *> CompletePrefixBlocks;
        std::function<bool(BasicBlock *)> CollectPrefix =
            [&](BasicBlock *BB) -> bool {
            if (BB == RegionExit) return true;
            if (!BB || !L->contains(BB) || LI.getLoopFor(BB) != L)
                return false;
            if (CompletePrefixBlocks.count(BB)) return true;
            if (!ActivePrefixBlocks.insert(BB).second) return false;
            std::vector<BasicBlock *> Successors = BB->getSuccessors();
            if (Successors.empty()) return false;
            PrefixBlocks.insert(BB);
            for (BasicBlock *Successor : Successors)
                if (!CollectPrefix(Successor)) return false;
            ActivePrefixBlocks.erase(BB);
            CompletePrefixBlocks.insert(BB);
            return true;
        };
        if (!CollectPrefix(L->getHeader()) ||
            !PrefixBlocks.count(L->getHeader()))
            continue;
        for (StoreInst *Store : AllStores)
            if (!Store || !PrefixBlocks.count(Store->getParent()))
                Valid = false;
        if (!Valid) continue;

        //闭合前缀中除候选私有store外只允许可丢弃、不会故障或产生
        //poison的纯计算；每条条件边也必须由循环不变且已定义的i1控制。
        for (BasicBlock *BB : PrefixBlocks) {
            for (Instruction *I : *BB) {
                if (auto *Branch = dynamic_cast<BrInst *>(I)) {
                    if (Branch->isConditional() &&
                        (!Branch->getCondition() ||
                         !Branch->getCondition()->getIRType() ||
                         !Branch->getCondition()->getIRType()->isBool() ||
                         !isInvariantAvailableAtPreHeader(
                             Branch->getCondition(), L, PreHeader, DT))) {
                        Valid = false;
                        break;
                    }
                    continue;
                }
                if ((BB == L->getHeader() && dynamic_cast<PhiInst *>(I)) ||
                    CandidateStoreSet.count(
                        dynamic_cast<StoreInst *>(I)) != 0)
                    continue;
                if (dynamic_cast<PhiInst *>(I) ||
                    dynamic_cast<LoadInst *>(I) ||
                    dynamic_cast<StoreInst *>(I) ||
                    dynamic_cast<CallInst *>(I) ||
                    dynamic_cast<RetInst *>(I) ||
                    dynamic_cast<AllocaInst *>(I) ||
                    dceInstructionMustBePreserved(I) ||
                    instructionMayFaultOrProducePoison(I) ||
                    valueMayDependOnUndef(I)) {
                    Valid = false;
                    break;
                }
                //前缀内普通定义不能流入后缀；循环携带状态由Header PHI
                //单独建立桥接，其他值若跨界会在旁路轮次沿用首轮旧值。
                for (Use *U : I->getUses()) {
                    auto *UserI = U
                        ? dynamic_cast<Instruction *>(U->getUser()) : nullptr;
                    if (!UserI || !UserI->getParent()) {
                        Valid = false;
                        break;
                    }
                    BasicBlock *UseBlock = UserI->getParent();
                    if (auto *UserPhi = dynamic_cast<PhiInst *>(UserI)) {
                        auto OperandIndex = U->getOperandIndex();
                        if (!OperandIndex || (*OperandIndex & 1U) != 0) {
                            Valid = false;
                            break;
                        }
                        std::size_t IncomingIndex = *OperandIndex / 2;
                        if (IncomingIndex >=
                            UserPhi->getNumIncomingValues()) {
                            Valid = false;
                            break;
                        }
                        UseBlock = UserPhi->getIncomingBlock(
                            static_cast<unsigned>(IncomingIndex));
                    }
                    if (!UseBlock || !PrefixBlocks.count(UseBlock)) {
                        Valid = false;
                        break;
                    }
                }
                if (!Valid) break;
            }
            if (!Valid) break;
        }
        if (!Valid) continue;

        //收益判断只使用通用目标代价和静态频率：互斥路径按各自块频率
        //加权，且至少要求预计重复两轮、节省覆盖新增汇合分支的两倍。
        const int JoinCost = TargetCost.getBranchCost(
            false, TargetCostKind::Throughput);
        if (JoinCost <= 0 || JoinCost >= TargetCostInfo::InvalidCost)
            continue;
        int64_t StaticPrefixCost = 0;
        unsigned __int128 WeightedPrefixCost = 0;
        for (BasicBlock *BB : PrefixBlocks) {
            int BlockCost = 0;
            for (Instruction *I : *BB) {
                int Cost = TargetCost.getInstructionCost(
                    I, TargetCostKind::Throughput);
                if (Cost < 0 || Cost >= TargetCostInfo::InvalidCost ||
                    BlockCost > TargetCostInfo::InvalidCost - Cost) {
                    Valid = false;
                    break;
                }
                BlockCost += Cost;
            }
            if (!Valid) break;
            StaticPrefixCost = std::min<int64_t>(
                TargetCostInfo::InvalidCost,
                StaticPrefixCost + static_cast<int64_t>(BlockCost));
            WeightedPrefixCost +=
                static_cast<unsigned __int128>(BlockCost) *
                BFI.getBlockFrequency(BB);
        }
        //频率传播未收敛时只把统一的保守回退频率当作盈利提示，并把
        //静态收益余量提高到八倍；它绝不参与等价性或可达性证明。
        const int64_t StaticCostMargin = BFI.isReliable() ? 2 : 8;
        if (!Valid ||
            StaticPrefixCost <= StaticCostMargin * JoinCost)
            continue;
        const uint64_t HeaderFrequency =
            BFI.getBlockFrequency(L->getHeader());
        const uint64_t PreHeaderFrequency =
            BFI.getBlockFrequency(PreHeader);
        if (HeaderFrequency == 0 || PreHeaderFrequency == 0) continue;
        uint64_t EstimatedIterations = HeaderFrequency / PreHeaderFrequency;
        if (Trip->ConstantTripCount && *Trip->ConstantTripCount > 1)
            EstimatedIterations = static_cast<uint64_t>(
                *Trip->ConstantTripCount);
        if (EstimatedIterations < 2) continue;
        const unsigned __int128 PrefixCostPerIteration =
            WeightedPrefixCost / HeaderFrequency;
        const unsigned __int128 EstimatedSaving =
            static_cast<unsigned __int128>(EstimatedIterations - 1) *
            PrefixCostPerIteration;
        if (PrefixCostPerIteration == 0 ||
            EstimatedSaving <=
                static_cast<unsigned __int128>(2 * JoinCost))
            continue;

        std::vector<BasicBlock *> RegionPredecessors =
            RegionExit->getPredecessors();
        if (RegionPredecessors.empty()) continue;
        for (BasicBlock *Pred : RegionPredecessors) {
            if (!Pred || Pred == Latch || !PrefixBlocks.count(Pred) ||
                Pred->countEdgesToSucc(RegionExit) != 1) {
                Valid = false;
                break;
            }
        }
        if (!Valid) continue;

        std::vector<InvariantPrivateStoreHeaderState> HeaderStates;
        for (PhiInst *Phi : L->getHeader()->collectPhiNodes()) {
            if (!Phi || Phi->getNumIncomingValues() != 2 ||
                Phi->countIncomingFromPred(PreHeader) != 1 ||
                Phi->countIncomingFromPred(Latch) != 1) {
                Valid = false;
                break;
            }
            Value *InitialValue = Phi->getIncomingFromPred(PreHeader);
            Value *BackedgeValue = Phi->getIncomingFromPred(Latch);
            auto *BackedgeInstruction =
                dynamic_cast<Instruction *>(BackedgeValue);
            if (!InitialValue || !BackedgeValue ||
                InitialValue == Phi || BackedgeValue == Phi ||
                !Phi->getIRType() || !InitialValue->getIRType() ||
                !BackedgeValue->getIRType() ||
                !IRType::hasSameShape(
                    Phi->getIRType(), InitialValue->getIRType()) ||
                !IRType::hasSameShape(
                    Phi->getIRType(), BackedgeValue->getIRType()) ||
                !isInvariantAvailableAtPreHeader(
                    InitialValue, L, PreHeader, DT) ||
                valueMayDependOnUndef(InitialValue) ||
                valueMayDependOnUndef(BackedgeValue) ||
                dynamic_cast<PhiInst *>(BackedgeValue) ||
                (BackedgeInstruction &&
                 (!DT.dominatesValue(
                      BackedgeInstruction, Latch->getBranchTerminator()) ||
                  BackedgeInstruction->getParent() == L->getHeader() ||
                  PrefixBlocks.count(
                      BackedgeInstruction->getParent()) != 0))) {
                Valid = false;
                break;
            }
            InvariantPrivateStoreHeaderState State;
            State.HeaderPhi = Phi;
            State.InitialValue = InitialValue;
            State.BackedgeValue = BackedgeValue;
            for (Use *U : Phi->getUses()) {
                auto *UserI = U
                    ? dynamic_cast<Instruction *>(U->getUser()) : nullptr;
                if (!UserI || !UserI->getParent()) {
                    Valid = false;
                    break;
                }
                BasicBlock *UseBlock = UserI->getParent();
                if (auto *UserPhi = dynamic_cast<PhiInst *>(UserI)) {
                    auto OperandIndex = U->getOperandIndex();
                    if (!OperandIndex || (*OperandIndex & 1U) != 0) {
                        Valid = false;
                        break;
                    }
                    std::size_t IncomingIndex = *OperandIndex / 2;
                    if (IncomingIndex >=
                        UserPhi->getNumIncomingValues()) {
                        Valid = false;
                        break;
                    }
                    UseBlock = UserPhi->getIncomingBlock(
                        static_cast<unsigned>(IncomingIndex));
                }
                if (UseBlock == Latch && UserI == Phi) continue;
                if (UseBlock && DT.dominates(RegionExit, UseBlock)) {
                    State.SuffixUses.push_back(U);
                    continue;
                }
                if (!UseBlock || (!PrefixBlocks.count(UseBlock) &&
                                  UseBlock != L->getHeader())) {
                    Valid = false;
                    break;
                }
            }
            if (!Valid) break;
            HeaderStates.push_back(std::move(State));
        }
        if (!Valid || HeaderStates.empty()) continue;

        Plan.TargetLoop = L;
        Plan.PreHeader = PreHeader;
        Plan.Header = L->getHeader();
        Plan.Latch = Latch;
        Plan.RegionExit = RegionExit;
        Plan.Root = Root;
        Plan.ZeroInitializer = Initializer;
        Plan.Stores = std::move(Candidates);
        Plan.HeaderStates = std::move(HeaderStates);
        Plan.RegionPredecessors = std::move(RegionPredecessors);
        return true;
    }
    return false;
}

//首轮仍执行原条件CFG；只有原回边改走汇合块，因此不投机任何谓词或访存
bool InvariantPrivateStoreHoistPass::applyPlan(
    InvariantPrivateStoreHoistPlan &Plan) {
    if (!Plan.TargetLoop || !Plan.PreHeader || !Plan.Header ||
        !Plan.Latch || !Plan.RegionExit || !Plan.Root ||
        Plan.Stores.empty() || Plan.HeaderStates.empty() ||
        Plan.RegionPredecessors.empty() ||
        !Plan.Latch->getBranchTerminator() ||
        Plan.Latch->countEdgesToSucc(Plan.Header) != 1 ||
        !Plan.RegionExit->collectPhiNodes().empty())
        return false;
    for (BasicBlock *Pred : Plan.RegionPredecessors)
        if (!Pred || Pred->countEdgesToSucc(Plan.RegionExit) != 1)
            return false;
    for (const InvariantPrivateStoreHeaderState &State : Plan.HeaderStates) {
        if (!State.HeaderPhi || !State.InitialValue ||
            !State.BackedgeValue ||
            State.HeaderPhi->getParent() != Plan.Header ||
            State.HeaderPhi->countIncomingFromPred(Plan.PreHeader) != 1 ||
            State.HeaderPhi->countIncomingFromPred(Plan.Latch) != 1 ||
            State.HeaderPhi->getIncomingFromPred(Plan.PreHeader) !=
                State.InitialValue ||
            State.HeaderPhi->getIncomingFromPred(Plan.Latch) !=
                State.BackedgeValue)
            return false;
        for (Use *U : State.SuffixUses)
            if (!U || U->getValue() != State.HeaderPhi) return false;
    }

    Function *Owner = Plan.Header->getParent();
    if (!Owner || Plan.RegionExit->getParent() != Owner ||
        Plan.Latch->getParent() != Owner)
        return false;
    //先把前缀的多条汇合边压到单一代理块，避免为每个旧前驱复制一份
    //相同PHI incoming；代理只改变CFG表示，不增加任何动态计算。
    auto *FirstIterationJoin = new BasicBlock();
    FirstIterationJoin->setIdent(Owner->makeUniqueBlockIdent(
        Plan.RegionExit->getIdent() + ".first.iteration.join"));
    Owner->pushBlock(FirstIterationJoin);
    FirstIterationJoin->genUnCondInst(Plan.RegionExit);
    for (BasicBlock *Pred : Plan.RegionPredecessors) {
        bool Rewired = BasicBlock::redirectBranchTarget(
            Pred, Plan.RegionExit, FirstIterationJoin);
        assert(Rewired);
        (void)Rewired;
    }

    std::vector<PhiInst *> MergePhis;
    MergePhis.reserve(Plan.HeaderStates.size());
    for (const InvariantPrivateStoreHeaderState &State : Plan.HeaderStates) {
        auto *Merge = new PhiInst(State.HeaderPhi->getIRType());
        Merge->addIncoming(State.InitialValue, FirstIterationJoin);
        Merge->addIncoming(State.BackedgeValue, Plan.Latch);
        bool Inserted = BasicBlock::insertAfterPhiNodes(
            Plan.RegionExit, Merge);
        assert(Inserted);
        (void)Inserted;
        MergePhis.push_back(Merge);
    }

    //先把后缀use全部改接到新状态，再重定向真实CFG边并清除旧回边incoming。
    for (std::size_t Index = 0; Index < Plan.HeaderStates.size(); ++Index)
        for (Use *U : Plan.HeaderStates[Index].SuffixUses) {
            bool Replaced = U->replaceUseeWith(MergePhis[Index]);
            assert(Replaced);
            (void)Replaced;
        }
    bool RewiredLatch = BasicBlock::redirectBranchTarget(
        Plan.Latch, Plan.Header, Plan.RegionExit);
    assert(RewiredLatch);
    (void)RewiredLatch;
    for (const InvariantPrivateStoreHeaderState &State : Plan.HeaderStates) {
        bool Removed = PhiInst::removeIncomingByPred(
            State.HeaderPhi, Plan.Latch, 1);
        assert(Removed);
        (void)Removed;
    }
    return true;
}

//每次只提交一个聚合计划，使效分析后由外层固定点重建循环和CFG
PreservationStatus InvariantPrivateStoreHoistPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &BFI = FAM.getResult<BlockFrequencyAnalysis>(F);
    const auto &TargetCost = FAM.getResult<TargetCostAnalysis>(F);
    for (Loop *Top : LI.getTopLevelLoops()) {
        if (!Top) continue;
        std::vector<Loop *> Loops = Top->getLoopsInPostOrder();
        std::reverse(Loops.begin(), Loops.end());
        for (Loop *L : Loops) {
            InvariantPrivateStoreHoistPlan Plan;
            if (!buildPlan(
                    L, LI, SE, RI, AA, DT, PDT, BFI, TargetCost, Plan))
                continue;
            if (applyPlan(Plan)) return PreservationStatus::none();
        }
    }
    return PreservationStatus::all();
}
