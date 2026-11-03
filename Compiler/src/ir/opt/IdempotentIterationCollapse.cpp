// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/IdempotentIterationCollapse.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_set>
 
//把分段 typed GEP 链归一化为按字节步长降序排列的坐标带。
bool IdempotentIterationCollapsePass::normalizeTypedAddress(
    const LoopMemoryAccess &Access, BasicAAResult &AA,
    IdempotentTypedAddress &Address) {
    Address = IdempotentTypedAddress{};
    if (!Access.Inst || !Access.Pointer || !Access.Location.Size ||
        *Access.Location.Size == 0 || !Access.AddressExact ||
        !Access.Root || Access.Pointer != Access.Location.Pointer) {
        return false;
    }
    std::map<std::int64_t, IdempotentAddressBand,
             std::greater<std::int64_t>> Bands;
    Value *Current = Access.Pointer;
    std::unordered_set<Value *> Seen;
    std::size_t Depth = 0;
    while (Current) {
        if (++Depth > MaxPointerDepth || !Seen.insert(Current).second)
            return false;
        auto *GEP = dynamic_cast<GetElementPtrInst *>(Current);
        if (!GEP) break;
        Value *Base = nullptr;
        std::vector<Value *> Indices;
        std::vector<std::int64_t> Strides;
        std::int64_t FinalStride = 0;
        if (!GEP->getBaseAndIndices(Base, Indices) || !Base ||
            !GetElementPtrInst::getIndexStrides(
                Base, Indices, Strides, FinalStride) ||
            Indices.size() != Strides.size()) {
            return false;
        }
        (void)FinalStride;
        for (std::size_t Index = 0; Index < Indices.size(); ++Index) {
            if (!Indices[Index] || Strides[Index] <= 0 ||
                valueMayDependOnUndef(Indices[Index], 256)) {
                return false;
            }
            IdempotentAddressBand &Band = Bands[Strides[Index]];
            Band.ByteStride = Strides[Index];
            std::int64_t Constant = 0;
            if (Indices[Index]->tryGetInt64Const(Constant)) {
                const __int128 Sum =
                    static_cast<__int128>(Band.ConstantIndex) + Constant;
                if (Constant < 0 ||
                    Sum > std::numeric_limits<std::int64_t>::max()) {
                    return false;
                }
                Band.ConstantIndex = static_cast<std::int64_t>(Sum);
            } else {
                if (Band.DynamicIndex) return false;
                Band.DynamicIndex = Indices[Index];
            }
        }
        Current = Base;
    }
    if (!Current || dynamic_cast<BitCastInst *>(Current) ||
        AA.getUniqueUnderlyingObject(Access.Pointer) != Current ||
        Current != Access.Root) {
        return false;
    }
    Address.Inst = Access.Inst;
    Address.Pointer = Access.Pointer;
    Address.Root = Current;
    Address.AccessBytes = *Access.Location.Size;
    for (const auto &Entry : Bands) {
        const IdempotentAddressBand &Band = Entry.second;
        if (!Band.DynamicIndex && Band.ConstantIndex == 0) continue;
        Address.Bands.push_back(Band);
    }
    std::int64_t SmallestDynamicStride = 0;
    for (const IdempotentAddressBand &Band : Address.Bands) {
        if (!Band.DynamicIndex) continue;
        SmallestDynamicStride = Band.ByteStride;
    }
    return SmallestDynamicStride > 0 &&
        static_cast<std::uint64_t>(SmallestDynamicStride) ==
            Address.AccessBytes;
}

//判断两个目标地址是否具有同根、同宽度和同矩形布局。
bool IdempotentIterationCollapsePass::hasSameRectangleLayout(
    const IdempotentTypedAddress &First,
    const IdempotentTypedAddress &Second) {
    if (!First.Root || First.Root != Second.Root ||
        First.AccessBytes == 0 ||
        First.AccessBytes != Second.AccessBytes ||
        First.Bands.size() != Second.Bands.size()) {
        return false;
    }
    for (std::size_t Index = 0; Index < First.Bands.size(); ++Index) {
        const IdempotentAddressBand &A = First.Bands[Index];
        const IdempotentAddressBand &B = Second.Bands[Index];
        if (A.ByteStride != B.ByteStride ||
            A.ConstantIndex != B.ConstantIndex ||
            static_cast<bool>(A.DynamicIndex) !=
                static_cast<bool>(B.DynamicIndex)) {
            return false;
        }
    }
    return true;
}

//证明动态坐标的起点在所有可达路径上非负。
bool IdempotentIterationCollapsePass::isKnownNonNegativeStart(
    Value *Start, Instruction *Context, Loop *Parent,
    SCEVResult &SE, RangeInfo &RI,
    const DominatorTree &DT, const LoopInfo &LI,
    std::size_t Depth) {
    if (!Start || !Context || !Parent || Depth > 8 ||
        valueMayDependOnUndef(Start, 256)) {
        return false;
    }
    std::int64_t Constant = 0;
    if (Start->tryGetInt64Const(Constant)) return Constant >= 0;
    IntRange Range = SE.evaluateValueRangeAt(Start, Context, RI, DT);
    if (!Range.isBottom() && Range.min >= 0) return true;
    auto *Definition = dynamic_cast<Instruction *>(Start);
    if (!Definition || !Definition->getParent()) return false;
    for (Loop *Axis = LI.getLoopFor(Definition->getParent());
         Axis && Axis != Parent; Axis = Axis->getParentLoop()) {
        auto Induction = SE.getCanonicalInduction(Axis);
        if (!Induction || !Induction->Valid || !Induction->Phi ||
            !Induction->UpdateValue || !Induction->ConstantStep ||
            *Induction->ConstantStep != 1 ||
            (Start != Induction->Phi && Start != Induction->UpdateValue) ||
            !SE.isKnownNoWrapInductionValue(Axis, Induction->Phi) ||
            !SE.isKnownNoWrapInductionValue(
                Axis, Induction->UpdateValue)) {
            continue;
        }
        return isKnownNonNegativeStart(
            Induction->StartValue, Context, Parent,
            SE, RI, DT, LI, Depth + 1);
    }
    return false;
}

//证明受回边比较控制的 PHI 首次进入循环前也满足严格上界。
bool IdempotentIterationCollapsePass::hasMatchingEntryGuard(
    const IdempotentAxisMatch &Match,
    const DominatorTree &DT) {
    if (!Match.Axis || !Match.Start || !Match.Bound || !Match.Phi)
        return false;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!Match.Axis->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch) {
        return false;
    }
    BasicBlock *Path = PreHeader;
    for (unsigned Depth = 0; Depth < 4; ++Depth) {
        std::vector<BasicBlock *> Preds = Path->getPredecessors();
        if (Preds.size() != 1 || !Preds.front() ||
            Match.Axis->contains(Preds.front())) {
            return false;
        }
        BasicBlock *Pred = Preds.front();
        BrInst *Branch = Pred->getBranchTerminator();
        if (!Branch) return false;
        if (!Branch->isConditional()) {
            if (Branch->getSuccessorCount() != 1 ||
                Branch->getSuccessor(0) != Path) {
                return false;
            }
            Path = Pred;
            continue;
        }
        const bool PathOnTrue = Branch->getTrueSuccessor() == Path;
        const bool PathOnFalse = Branch->getFalseSuccessor() == Path;
        if (PathOnTrue == PathOnFalse) return false;
        auto *Compare = dynamic_cast<BinaryInst *>(Branch->getCondition());
        if (!Compare || !BinaryInst::isCompareOp(Compare->getOp()) ||
            valueMayDependOnUndef(Compare, 256)) {
            return false;
        }
        BinaryInst::Operation Predicate = PathOnTrue
            ? Compare->getOp()
            : BinaryInst::negatePredicate(Compare->getOp());
        Value *Left = Compare->getOperand(0);
        Value *Right = Compare->getOperand(1);
        auto SameValue = [](Value *First, Value *Second) {
            return First == Second ||
                Value::areEqualConstants(First, Second);
        };
        if (SameValue(Right, Match.Start)) {
            std::swap(Left, Right);
            Predicate = BinaryInst::reversePredicate(Predicate);
        }
        return SameValue(Left, Match.Start) &&
            SameValue(Right, Match.Bound) &&
            Predicate == BinaryInst::L && DT.dominates(Pred, PreHeader);
    }
    return false;
}

//识别严格小于运行时上界的正单位规范归纳轴。
bool IdempotentIterationCollapsePass::matchAxisPredicate(
    Loop *Axis, Value *Index, Instruction *Context,
    Loop *Parent, SCEVResult &SE, RangeInfo &RI,
    DominatorTree &DT, const LoopInfo &LI,
    IdempotentAxisMatch &Match) {
    Match = IdempotentAxisMatch{};
    if (!Axis || !Index || !Context || !Parent || Axis == Parent ||
        !Parent->contains(Axis->getHeader())) {
        return false;
    }
    auto Induction = SE.getCanonicalInduction(Axis);
    auto Trip = SE.analyzeLoopTripCount(Axis);
    if (!Induction || !Trip || !Induction->Valid || !Trip->Valid ||
        !Induction->Phi || !Induction->UpdateValue ||
        !Induction->ConstantStep || *Induction->ConstantStep != 1 ||
        !Trip->ExitCmp || !Trip->ExitingBlock ||
        Trip->ComparePhase == LoopExitComparePhase::Unknown ||
        (Index != Induction->Phi && Index != Induction->UpdateValue) ||
        !SE.isKnownFiniteLoop(Axis, RI, DT) ||
        !SE.isKnownNoWrapInductionValue(Axis, Induction->Phi) ||
        !SE.isKnownNoWrapInductionValue(
            Axis, Induction->UpdateValue) ||
        valueMayDependOnUndef(Trip->ExitCmp, 256)) {
        return false;
    }
    Value *Compared = Trip->ComparePhase ==
            LoopExitComparePhase::HeaderPhi
        ? static_cast<Value *>(Induction->Phi)
        : Induction->UpdateValue;
    Value *Left = Trip->ExitCmp->getOperand(0);
    Value *Right = Trip->ExitCmp->getOperand(1);
    BinaryInst::Operation Predicate = Trip->ContinueOnTrue
        ? Trip->ExitCmp->getOp()
        : BinaryInst::negatePredicate(Trip->ExitCmp->getOp());
    if (Right == Compared) {
        std::swap(Left, Right);
        Predicate = BinaryInst::reversePredicate(Predicate);
    }
    if (Left != Compared || Predicate != BinaryInst::L || !Right ||
        !IRType::hasIntegerBitWidth(Right->getIRType(), 32) ||
        !LoopInfo::isLoopInvariant(Right, Axis) ||
        valueMayDependOnUndef(Right, 256)) {
        return false;
    }
    BrInst *ExitBranch = Trip->ExitingBlock->getBranchTerminator();
    if (!ExitBranch || !ExitBranch->isConditional() ||
        ExitBranch->getCondition() != Trip->ExitCmp) {
        return false;
    }
    BasicBlock *Continue = Trip->ContinueOnTrue
        ? ExitBranch->getTrueSuccessor()
        : ExitBranch->getFalseSuccessor();
    if (!Continue || !Axis->contains(Continue) ||
        !isKnownNonNegativeStart(
            Induction->StartValue, Context, Parent,
            SE, RI, DT, LI)) {
        return false;
    }
    Match.Axis = Axis;
    Match.Phi = Induction->Phi;
    Match.Update = Induction->UpdateValue;
    Match.Index = Index;
    Match.Start = Induction->StartValue;
    Match.Bound = Right;
    Match.BoundExpr = SE.getSCEV(Right);
    Match.ContinueSuccessor = Continue;
    Match.UsesUpdate = Index == Induction->UpdateValue;
    if (!Match.BoundExpr) return false;
    //只有继续块由退出比较的继续边唯一进入时，块支配才等价于边控制；
    //否则尾测循环最后一次失败探针可能已经等于上界。
    auto IsControlledByContinueEdge = [&]() {
        std::vector<BasicBlock *> Preds = Continue->getPredecessors();
        return Continue != Axis->getHeader() && Preds.size() == 1 &&
            Preds.front() == Trip->ExitingBlock &&
            Trip->ExitingBlock->countEdgesToSucc(Continue) == 1 &&
            DT.dominates(Continue, Context->getParent());
    };
    if (Match.UsesUpdate) {
        return Trip->ComparePhase ==
                LoopExitComparePhase::BackedgeUpdate &&
            Compared == Index &&
            IsControlledByContinueEdge();
    }
    if (Trip->ComparePhase == LoopExitComparePhase::HeaderPhi)
        return Compared == Index &&
            IsControlledByContinueEdge();
    return hasMatchingEntryGuard(Match, DT);
}

//证明地址中的每个动态坐标都包含于共同的半开矩形边界。
bool IdempotentIterationCollapsePass::matchContainedAddress(
    const IdempotentTypedAddress &Address, Loop *Parent,
    Value *&SharedBound, const SCEVExpr *&SharedBoundExpr,
    SCEVResult &SE, RangeInfo &RI,
    DominatorTree &DT, const LoopInfo &LI) {
    if (!Address.Inst || !Parent || Address.Bands.empty()) return false;
    bool SawDynamic = false;
    for (const IdempotentAddressBand &Band : Address.Bands) {
        if (!Band.DynamicIndex) continue;
        if (Band.ConstantIndex != 0) return false;
        SawDynamic = true;
        auto *Definition =
            dynamic_cast<Instruction *>(Band.DynamicIndex);
        if (!Definition || !Definition->getParent()) return false;
        IdempotentAxisMatch Match;
        bool Matched = false;
        for (Loop *Axis = LI.getLoopFor(Definition->getParent());
             Axis && Axis != Parent; Axis = Axis->getParentLoop()) {
            if (matchAxisPredicate(
                    Axis, Band.DynamicIndex, Address.Inst,
                    Parent, SE, RI, DT, LI, Match)) {
                Matched = true;
                break;
            }
        }
        if (!Matched || !Match.Bound || !Match.BoundExpr) return false;
        if (SharedBoundExpr && SharedBoundExpr != Match.BoundExpr &&
            SharedBoundExpr->Key != Match.BoundExpr->Key) {
            return false;
        }
        if (!SharedBoundExpr) {
            SharedBound = Match.Bound;
            SharedBoundExpr = Match.BoundExpr;
        }
    }
    return SawDynamic;
}

//由对象大小和 typed stride 推导当前地址允许的最大共同边界。
bool IdempotentIterationCollapsePass::accumulateSafeUpperBound(
    const IdempotentTypedAddress &Address, BasicAAResult &AA,
    std::int64_t &SafeUpperBound) {
    if (!Address.Pointer || Address.AccessBytes == 0 ||
        AA.getKnownAlignment(Address.Pointer) < Address.AccessBytes) {
        return false;
    }
    auto ObjectSize = AA.getObjectSize(Address.Pointer);
    if (!ObjectSize || *ObjectSize < Address.AccessBytes) return false;
    const __int128 ObjectBytes = static_cast<__int128>(*ObjectSize);
    __int128 ConstantBytes = 0;
    __int128 DynamicStride = 0;
    for (const IdempotentAddressBand &Band : Address.Bands) {
        if (Band.ByteStride <= 0 || Band.ConstantIndex < 0) return false;
        const __int128 ConstantTerm =
            static_cast<__int128>(Band.ByteStride) *
            static_cast<__int128>(Band.ConstantIndex);
        if (ConstantTerm > ObjectBytes ||
            ConstantBytes > ObjectBytes - ConstantTerm) {
            return false;
        }
        ConstantBytes += ConstantTerm;
        if (Band.DynamicIndex) {
            if (Band.ConstantIndex != 0) return false;
            //超过对象大小后精确和值不再影响容量结果，饱和到对象大小加一。
            DynamicStride = std::min(
                ObjectBytes + 1,
                DynamicStride + static_cast<__int128>(Band.ByteStride));
        }
    }
    const __int128 Available = ObjectBytes -
        Address.AccessBytes - ConstantBytes;
    if (Available < 0 || DynamicStride <= 0) return false;
    __int128 Capacity = Available / DynamicStride + 1;
    Capacity = std::min(
        Capacity, static_cast<__int128>(IntRange::I32_MAX));
    if (Capacity <= 0) return false;
    SafeUpperBound = std::min(
        SafeUpperBound, static_cast<std::int64_t>(Capacity));
    return SafeUpperBound > 0;
}

//检查父归纳值只参与回边控制，不改变本轮计算或循环外标量状态。
bool IdempotentIterationCollapsePass::hasOnlyControlUses(
    const LoopTripCountInfo &Trip, PhiInst *Induction,
    Value *Update, BrInst *LatchBranch) {
    auto *UpdateInstruction = dynamic_cast<Instruction *>(Update);
    BrInst *ExitBranch = Trip.ExitingBlock
        ? Trip.ExitingBlock->getBranchTerminator() : nullptr;
    const bool LatchExiting = Trip.ExitingBlock == Trip.Latch &&
        ExitBranch == LatchBranch && LatchBranch &&
        LatchBranch->isConditional() &&
        LatchBranch->getCondition() == Trip.ExitCmp;
    const bool HeaderExiting = Trip.ExitingBlock == Trip.Header &&
        ExitBranch && ExitBranch->isConditional() &&
        ExitBranch->getCondition() == Trip.ExitCmp &&
        LatchBranch && !LatchBranch->isConditional() &&
        LatchBranch->getSuccessorCount() == 1 &&
        LatchBranch->getSuccessor(0) == Trip.Header;
    if (!Induction || !UpdateInstruction || !Trip.ExitCmp ||
        !ExitBranch || (!LatchExiting && !HeaderExiting)) {
        return false;
    }
    for (Use *UseNode : Induction->getUses()) {
        auto *User = UseNode
            ? dynamic_cast<Instruction *>(UseNode->getUser()) : nullptr;
        if (User != UpdateInstruction && User != Trip.ExitCmp) return false;
    }
    for (Use *UseNode : UpdateInstruction->getUses()) {
        auto *User = UseNode
            ? dynamic_cast<Instruction *>(UseNode->getUser()) : nullptr;
        if (User != Induction && User != Trip.ExitCmp) return false;
    }
    return Trip.ExitCmp->hasExactlyOneUse() &&
        Trip.ExitCmp->getUses().front()->getUser() == ExitBranch;
}

//为单个父循环建立完整覆盖、不动点和对象安全计划。
bool IdempotentIterationCollapsePass::buildPlan(
    Loop *Parent, LoopInfo &LI,
    SCEVResult &SE, RangeInfo &RI, BasicAAResult &AA,
    LoopDependenceInfo &LD, LoopAccessInfo &LA,
    DominatorTree &DT, IdempotentIterationPlan &Plan) {
    Plan = IdempotentIterationPlan{};
    if (!Parent || !Parent->getHeader() ||
        !LCSSAPass::isLCSSAForm(Parent)) {
        return false;
    }
    auto Exits = Parent->getExitBlocks();
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    std::vector<BasicBlock *> InsideHeaderPreds;
    std::vector<BasicBlock *> OutsideHeaderPreds;
    for (BasicBlock *Pred : Parent->getHeader()->getPredecessors()) {
        if (!Pred) return false;
        (Parent->contains(Pred)
             ? InsideHeaderPreds : OutsideHeaderPreds).push_back(Pred);
    }
    if (Exits.size() == 1 && InsideHeaderPreds.size() == 1 &&
        OutsideHeaderPreds.size() == 1) {
        Latch = InsideHeaderPreds.front();
        PreHeader = OutsideHeaderPreds.front();
    }
    bool EntryShapeValid = PreHeader && Latch;
    BrInst *EntryBranch = EntryShapeValid
        ? PreHeader->getBranchTerminator() : nullptr;
    if (!EntryBranch) {
        EntryShapeValid = false;
    } else if (EntryBranch->isConditional()) {
        BasicBlock *TrueSuccessor = EntryBranch->getTrueSuccessor();
        BasicBlock *FalseSuccessor = EntryBranch->getFalseSuccessor();
        EntryShapeValid =
            ((TrueSuccessor == Parent->getHeader() &&
              FalseSuccessor == Exits.front()) ||
             (FalseSuccessor == Parent->getHeader() &&
              TrueSuccessor == Exits.front())) &&
            EntryBranch->getCondition() &&
            !valueMayDependOnUndef(EntryBranch->getCondition(), 256);
    } else {
        EntryShapeValid = EntryBranch->getSuccessorCount() == 1 &&
            EntryBranch->getSuccessor(0) == Parent->getHeader();
    }
    if (EntryShapeValid) {
        for (BasicBlock *Pred : Exits.front()->getPredecessors()) {
            if (!Pred || (!Parent->contains(Pred) && Pred != PreHeader)) {
                EntryShapeValid = false;
                break;
            }
        }
    }
    if (!EntryShapeValid) return false;
    auto Trip = SE.analyzeLoopTripCount(Parent);
    auto Induction = SE.getCanonicalInduction(Parent);
    auto *LatchBranch = Latch->getBranchTerminator();
    std::size_t ExitingBlockCount = 0;
    BasicBlock *OnlyExitingBlock = nullptr;
    for (BasicBlock *Block : Parent->getBlocksIncludingSubLoops()) {
        if (!Block || !Parent->isExitingBlock(Block)) continue;
        ++ExitingBlockCount;
        OnlyExitingBlock = Block;
    }
    if (Exits.size() != 1 || !Trip || !Trip->Valid ||
        !Trip->ConstantTripCount || *Trip->ConstantTripCount < 2 ||
        !Trip->ProvenNoWrap || !Induction || !Induction->Valid ||
        !Induction->Phi || !Induction->UpdateValue ||
        !LatchBranch ||
        Trip->PreHeader != PreHeader || Trip->Latch != Latch ||
        Trip->ExitBlock != Exits.front() ||
        ExitingBlockCount != 1 ||
        OnlyExitingBlock != Trip->ExitingBlock ||
        !SE.isKnownFiniteLoop(Parent, RI, DT) ||
        !hasOnlyControlUses(
            *Trip, Induction->Phi,
            Induction->UpdateValue, LatchBranch) ||
        Parent->getHeader()->collectPhiNodes().size() != 1 ||
        !Exits.front()->collectPhiNodes().empty()) {
        return false;
    }
    //绝大多数循环不是重复常量父阶段；只有通过廉价结构门禁后才构建
    //完整内存依赖摘要，避免无关大函数承担分析成本。
    if (LD.hasUnknownMemory(Parent) ||
        LD.hasObservableEffect(Parent) ||
        LD.hasOrderingBarrier(Parent)) {
        return false;
    }

    std::size_t InstructionCount = 0;
    for (BasicBlock *Block : Parent->getBlocksIncludingSubLoops()) {
        if (!Block) return false;
        for (Instruction *I : *Block) {
            if (!I || ++InstructionCount > MaxInstructions ||
                dynamic_cast<CallInst *>(I) ||
                dynamic_cast<RetInst *>(I) ||
                dynamic_cast<AllocaInst *>(I) ||
                dceInstructionMustBePreserved(I)) {
                return false;
            }
            if (!dynamic_cast<LoadInst *>(I) &&
                !dynamic_cast<StoreInst *>(I) &&
                !dynamic_cast<BrInst *>(I) &&
                !dynamic_cast<PhiInst *>(I) &&
                !dynamic_cast<GetElementPtrInst *>(I) &&
                !dynamic_cast<BitCastInst *>(I) &&
                instructionMayFaultOrProducePoison(I)) {
                return false;
            }
            if (valueMayDependOnUndef(I, 4096)) return false;
            for (Instruction *User : I->getInstructionUsers()) {
                if (!User || !User->getParent() ||
                    !Parent->contains(User->getParent())) {
                    return false;
                }
            }
        }
    }

    const auto &Memory = LD.getMemoryAccesses(Parent);
    if (Memory.empty() || Memory.size() > MaxMemoryAccesses) return false;
    std::vector<IdempotentTypedAddress> Addresses;
    Addresses.reserve(Memory.size());
    std::unordered_set<Value *> WrittenRoots;
    for (const LoopMemoryAccess &Access : Memory) {
        IdempotentTypedAddress Address;
        if (!normalizeTypedAddress(Access, AA, Address)) return false;
        Addresses.push_back(std::move(Address));
        if (Access.Writes) WrittenRoots.insert(Access.Root);
    }
    if (WrittenRoots.size() != 1) return false;
    Value *TargetRoot = *WrittenRoots.begin();
    auto *TargetVariable = dynamic_cast<Variable *>(TargetRoot);
    if (!TargetVariable ||
        TargetVariable->vartag != Variable::GlobalVariable) {
        return false;
    }
    for (const LoopMemoryAccess &Access : Memory) {
        auto *RootVariable = dynamic_cast<Variable *>(Access.Root);
        if (!RootVariable ||
            (RootVariable->vartag != Variable::GlobalVariable &&
             RootVariable->vartag != Variable::GlobalConstant)) {
            return false;
        }
        if (Access.Root != TargetRoot &&
            !AA.haveKnownDistinctUnderlyingObjects(
                Access.Root, TargetRoot)) {
            return false;
        }
    }

    const LoopMemoryAccess *ResetAccess = nullptr;
    const LoopMemoryAccess *ResetSourceAccess = nullptr;
    Loop *ResetOuter = nullptr;
    Loop *ResetInner = nullptr;
    IdempotentTypedAddress ResetAddress;
    BasicBlock *ResetExit = nullptr;
    for (std::size_t CandidateIndex = 0;
         CandidateIndex < Memory.size() && !ResetAccess;
         ++CandidateIndex) {
        const LoopMemoryAccess &Access = Memory[CandidateIndex];
        auto *Store = dynamic_cast<StoreInst *>(Access.Inst);
        if (!Store || !Access.Writes || Access.Root != TargetRoot)
            continue;
        Loop *Top = LI.getLoopFor(Store->getParent());
        while (Top && Top->getParentLoop() != Parent)
            Top = Top->getParentLoop();
        if (!Top || Top->getSubLoops().size() != 1) continue;
        Loop *Inner = Top->getSubLoops().front();
        if (!Inner || !Inner->isInnermost() ||
            !Inner->contains(Store->getParent()) ||
            !LA.isRectangularNest(Top, Inner)) {
            continue;
        }
        unsigned RegionAccesses = 0;
        unsigned TargetAccesses = 0;
        const LoopMemoryAccess *SourceAccess = nullptr;
        bool RegionValid = true;
        for (const LoopMemoryAccess &RegionAccess : Memory) {
            if (!RegionAccess.Inst ||
                !Top->contains(RegionAccess.Inst->getParent())) {
                continue;
            }
            ++RegionAccesses;
            if (RegionAccess.Root == TargetRoot) {
                ++TargetAccesses;
                RegionValid = RegionValid &&
                    RegionAccess.Inst == Store &&
                    RegionAccess.Writes && !RegionAccess.Reads;
            } else if (RegionAccess.Reads && !RegionAccess.Writes) {
                if (SourceAccess) RegionValid = false;
                SourceAccess = &RegionAccess;
            } else {
                RegionValid = false;
            }
        }
        auto *SourceLoad = dynamic_cast<LoadInst *>(
            SourceAccess ? SourceAccess->Inst : nullptr);
        if (!RegionValid || RegionAccesses != 2 ||
            TargetAccesses != 1 || !SourceLoad ||
            Store->getValueOperand() != SourceLoad ||
            !Store->fullyCoversItsPointer()) {
            continue;
        }
        IdempotentTypedAddress &CandidateAddress =
            Addresses[CandidateIndex];
        std::vector<Value *> DynamicIndices;
        for (const IdempotentAddressBand &Band : CandidateAddress.Bands) {
            if (Band.DynamicIndex) {
                if (Band.ConstantIndex != 0) {
                    RegionValid = false;
                    break;
                }
                DynamicIndices.push_back(Band.DynamicIndex);
            }
        }
        auto OuterInduction = SE.getCanonicalInduction(Top);
        auto InnerInduction = SE.getCanonicalInduction(Inner);
        if (!RegionValid || DynamicIndices.size() != 2 ||
            !OuterInduction || !InnerInduction ||
            !OuterInduction->ConstantStart ||
            *OuterInduction->ConstantStart != 0 ||
            !InnerInduction->ConstantStart ||
            *InnerInduction->ConstantStart != 0 ||
            DynamicIndices[0] != OuterInduction->Phi ||
            DynamicIndices[1] != InnerInduction->Phi) {
            continue;
        }
        Value *ResetBound = nullptr;
        const SCEVExpr *ResetBoundExpr = nullptr;
        if (!matchContainedAddress(
                CandidateAddress, Parent, ResetBound, ResetBoundExpr,
                SE, RI, DT, LI)) {
            continue;
        }
        bool MustExecute = true;
        for (BasicBlock *Backedge : Inner->getBackedgeSrcs())
            MustExecute = MustExecute && Backedge &&
                DT.dominates(Store->getParent(), Backedge);
        for (BasicBlock *Backedge : Top->getBackedgeSrcs())
            MustExecute = MustExecute && Backedge &&
                DT.dominates(Inner->getHeader(), Backedge);
        auto RegionExits = Top->getExitBlocks();
        if (!MustExecute || RegionExits.size() != 1 ||
            !DT.dominates(Top->getHeader(), Latch) ||
            !DT.dominates(RegionExits.front(), Latch)) {
            continue;
        }
        bool OrdersAllTargetAccesses = true;
        for (const LoopMemoryAccess &TargetAccess : Memory) {
            if (TargetAccess.Root != TargetRoot ||
                Top->contains(TargetAccess.Inst->getParent())) {
                continue;
            }
            if (!DT.dominates(
                    RegionExits.front(), TargetAccess.Inst->getParent())) {
                OrdersAllTargetAccesses = false;
                break;
            }
        }
        if (!OrdersAllTargetAccesses) continue;
        ResetAccess = &Access;
        ResetSourceAccess = SourceAccess;
        ResetOuter = Top;
        ResetInner = Inner;
        ResetAddress = CandidateAddress;
        ResetExit = RegionExits.front();
    }
    if (!ResetAccess || !ResetSourceAccess || !ResetOuter ||
        !ResetInner || !ResetExit) {
        return false;
    }

    Value *SharedBound = nullptr;
    const SCEVExpr *SharedBoundExpr = nullptr;
    std::int64_t SafeUpperBound = IntRange::I32_MAX;
    for (std::size_t Index = 0; Index < Memory.size(); ++Index) {
        const LoopMemoryAccess &Access = Memory[Index];
        const IdempotentTypedAddress &Address = Addresses[Index];
        if (!matchContainedAddress(
                Address, Parent, SharedBound, SharedBoundExpr,
                SE, RI, DT, LI) ||
            !accumulateSafeUpperBound(
                Address, AA, SafeUpperBound)) {
            return false;
        }
        if (Access.Root == TargetRoot &&
            !hasSameRectangleLayout(ResetAddress, Address)) {
            return false;
        }
    }
    if (!SharedBound || !SharedBoundExpr || SafeUpperBound <= 0 ||
        !IRType::hasIntegerBitWidth(SharedBound->getIRType(), 32) ||
        valueMayDependOnUndef(SharedBound, 256)) {
        return false;
    }
    if (auto *BoundInstruction =
            dynamic_cast<Instruction *>(SharedBound)) {
        if (!BoundInstruction->getParent() ||
            !DT.dominatesValue(
                BoundInstruction, PreHeader->getTerminator())) {
            return false;
        }
    }

    Plan.Parent = Parent;
    Plan.PreHeader = PreHeader;
    Plan.Header = Parent->getHeader();
    Plan.Latch = Latch;
    Plan.Exit = Exits.front();
    Plan.LatchBranch = LatchBranch;
    Plan.OriginalCondition = LatchBranch->isConditional()
        ? LatchBranch->getCondition() : nullptr;
    Plan.ContinueOnTrue = Trip->ContinueOnTrue;
    Plan.SharedBound = SharedBound;
    Plan.SafeUpperBound = SafeUpperBound;
    return true;
}

//在预头生成对象安全门禁，并只在安全域截断父循环回边。
bool IdempotentIterationCollapsePass::applyPlan(
    IdempotentIterationPlan &Plan) {
    if (!Plan.Parent || !Plan.PreHeader || !Plan.Header ||
        !Plan.Latch || !Plan.Exit || !Plan.LatchBranch ||
        !Plan.SharedBound ||
        Plan.SafeUpperBound <= 0 ||
        Plan.PreHeader->getBranchTerminator() == nullptr ||
        Plan.Latch->getBranchTerminator() != Plan.LatchBranch ||
        (Plan.LatchBranch->isConditional() &&
         !Plan.OriginalCondition) ||
        (!Plan.LatchBranch->isConditional() &&
         (Plan.OriginalCondition ||
          Plan.LatchBranch->getSuccessorCount() != 1 ||
          Plan.LatchBranch->getSuccessor(0) != Plan.Header))) {
        return false;
    }
    Value *Zero = Value::makeIntegerLike(Plan.SharedBound, 0);
    Value *Capacity = Value::makeIntegerLike(
        Plan.SharedBound, Plan.SafeUpperBound);
    Value *FalseValue = ConstBool::newConstBool(false);
    if (!Zero || !Capacity || !FalseValue) return false;
    auto *NonNegative = new BinaryInst(
        Plan.SharedBound, BinaryInst::GE, Zero);
    auto *WithinObject = new BinaryInst(
        Plan.SharedBound, BinaryInst::LE, Capacity);
    auto *Safe = new BinaryInst(
        NonNegative, BinaryInst::AND, WithinObject);
    for (Instruction *I : {
            static_cast<Instruction *>(NonNegative),
            static_cast<Instruction *>(WithinObject),
            static_cast<Instruction *>(Safe)}) {
        if (!Plan.PreHeader->insertBeforeTerminator(I)) return false;
    }
    Value *OriginalContinue = Plan.OriginalCondition;
    if (OriginalContinue && !Plan.ContinueOnTrue) {
        auto *Negated = new BinaryInst(
            Plan.OriginalCondition, BinaryInst::E, FalseValue);
        if (!Plan.Latch->insertBeforeTerminator(Negated)) return false;
        OriginalContinue = Negated;
    }
    auto *Unsafe = new BinaryInst(Safe, BinaryInst::E, FalseValue);
    BinaryInst *Repeat = OriginalContinue
        ? new BinaryInst(OriginalContinue, BinaryInst::AND, Unsafe)
        : Unsafe;
    if (!Plan.Latch->insertBeforeTerminator(Unsafe) ||
        (Repeat != Unsafe &&
         !Plan.Latch->insertBeforeTerminator(Repeat))) {
        return false;
    }
    auto *NewBranch = new BrInst(Repeat, Plan.Header, Plan.Exit);
    return Plan.Latch->replaceTerminator(NewBranch);
}

//依次尝试父循环候选；一次改写后立即失效全部结构分析。
PreservationStatus IdempotentIterationCollapsePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);
    auto &LA = FAM.getResult<LoopAccessAnalysis>(F);
    (void)FAM.getResult<SideEffectAnalysis>(F);
    for (Loop *Parent : LI.getLoopsInPostOrder(true)) {
        IdempotentIterationPlan Plan;
        if (!buildPlan(
                Parent, LI, SE, RI, AA, LD, LA, DT, Plan)) {
            continue;
        }
        if (applyPlan(Plan)) return PreservationStatus::none();
    }
    return PreservationStatus::all();
}
