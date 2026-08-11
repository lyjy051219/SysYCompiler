#include "ir/opt/UnrollAndJam.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/TargetCostAnalysis.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "ir/opt/LoopUnroll.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <cassert>
#include <limits>
#include <numeric>
#include <utility>
//使用给定资源与收益参数构造展开并合并Pass。
UnrollAndJamPass::UnrollAndJamPass(UnrollAndJamConfig C)
    : Config(std::move(C)) {}
//从头测单位步长循环提取二维分块需要的常量边界和可改写位置。
bool UnrollAndJamPass::matchTilingAxis(
    Loop *Axis, const LoopTripCountInfo &Trip,
    const InductionDescriptor &Induction, PhiInst *&IV,
    BinaryInst *&Cmp, Value *&StartValue, Value *&BoundValue,
    unsigned &StartIncomingIndex, unsigned &BoundOperandIndex,
    std::int64_t &TripCount) {
    IV = nullptr;
    Cmp = nullptr;
    StartValue = nullptr;
    BoundValue = nullptr;
    StartIncomingIndex = 0;
    BoundOperandIndex = 0;
    TripCount = 0;
    //1.只接受头部执行“IV < 常量上界”的正单位步长循环；次数和更新均须证明无回绕。
    if (!Axis || !Axis->getHeader() || !Trip.Valid ||
        Trip.Header != Axis->getHeader() ||
        Trip.ExitingBlock != Axis->getHeader() ||
        Trip.ComparePhase != LoopExitComparePhase::HeaderPhi ||
        !Trip.ContinueOnTrue || !Trip.ProvenNoWrap ||
        !Trip.ConstantTripCount || *Trip.ConstantTripCount <= 0 ||
        !Induction.Valid || Induction.L != Axis || !Induction.Phi ||
        !Induction.StartValue || !Induction.UpdateValue ||
        !Induction.ConstantStart || !Induction.ConstantStep ||
        *Induction.ConstantStep != 1 ||
        !Induction.Phi->getIRType() ||
        !Induction.Phi->getIRType()->isInt() ||
        Induction.Phi->getIRType()->isBool() ||
        !Trip.ExitCmp || Trip.ExitCmp->getParent() != Axis->getHeader()) {
        return false;
    }
    Value *LHS = Trip.ExitCmp->getOperand(0);
    Value *RHS = Trip.ExitCmp->getOperand(1);
    BinaryInst::Operation Predicate = Trip.ExitCmp->getOp();
    if (LHS == Induction.Phi && RHS != Induction.Phi) {
        BoundValue = RHS;
        BoundOperandIndex = 1;
    } else if (RHS == Induction.Phi && LHS != Induction.Phi) {
        BoundValue = LHS;
        BoundOperandIndex = 0;
        Predicate = BinaryInst::reversePredicate(Predicate);
    } else {
        return false;
    }
    if (Predicate != BinaryInst::L || !BoundValue ||
        !BoundValue->getIRType() ||
        !BoundValue->getIRType()->hasSameShape(
            *Induction.Phi->getIRType())) {
        return false;
    }
    std::int64_t Bound = 0;
    if (!BoundValue->tryGetInt64Const(Bound)) return false;
    const __int128 Distance = static_cast<__int128>(Bound) -
                              *Induction.ConstantStart;
    if (Distance != *Trip.ConstantTripCount || Distance <= 0)
        return false;
    //2.PHI必须恰有一条来自规范预头的起始边和一条循环内回边。
    bool FoundStart = false;
    unsigned BackedgeCount = 0;
    for (unsigned Index = 0;
         Index < Induction.Phi->getNumIncomingValues(); ++Index) {
        BasicBlock *Pred = Induction.Phi->getIncomingBlock(Index);
        Value *Incoming = Induction.Phi->getIncomingValue(Index);
        if (!Pred || !Incoming) return false;
        if (!Axis->contains(Pred)) {
            if (FoundStart || Pred != Trip.PreHeader ||
                Incoming != Induction.StartValue) {
                return false;
            }
            FoundStart = true;
            StartIncomingIndex = Index;
        } else {
            if (Incoming != Induction.UpdateValue) return false;
            ++BackedgeCount;
        }
    }
    if (!FoundStart || BackedgeCount != 1) return false;
    IV = Induction.Phi;
    Cmp = Trip.ExitCmp;
    StartValue = Induction.StartValue;
    TripCount = *Trip.ConstantTripCount;
    return true;
}
//检查完美嵌套连接块或回边块只承担已识别控制。
bool UnrollAndJamPass::hasOnlyTilingControl(
    BasicBlock *BB,
    const std::unordered_set<Instruction *> &Allowed,
    BasicBlock *ExpectedSuccessor) {
    if (!BB || !ExpectedSuccessor) return false;
    BrInst *Branch = BB->getBranchTerminator();
    if (!BasicBlock::isUncondBr(Branch) ||
        Branch->getSuccessor(0) != ExpectedSuccessor) {
        return false;
    }
    for (Instruction *I : *BB) {
        if (I == Branch || Allowed.count(I)) continue;
        return false;
    }
    return true;
}
//构建严格整除、双轴独立且没有顺序屏障的二维分块候选。
bool UnrollAndJamPass::buildTilingShape(
    Loop *Outer, Loop *Inner, LoopAccessInfo &LA, SCEVResult &SE,
    DominatorTree &DT, PostDominatorTree &PDT,
    LoopTilingShape &Shape) {
    Shape = {};
    //1.只接受直接父子完美矩形；两个待重排轴均须不存在跨迭代写冲突。
    const bool Direct = Outer && Inner &&
        Inner->getParentLoop() == Outer &&
        Outer->getSubLoops().size() == 1 &&
        Outer->getSubLoops().front() == Inner &&
        Inner->getSubLoops().empty();
    const bool Rectangular =
        Direct && LA.isRectangularNest(Outer, Inner);
    //1.1 LoopAccess的通用成对判定保守失败时，只补充不同唯一对象之间的
    //不相交事实；同一对象的Unknown必须继续拒绝，不能用单个下标的非零系数
    //替代完整mixed-radix和坐标范围证明。
    auto IsUniqueObject = [](Value *Root) {
        if (auto *VariableRoot = dynamic_cast<Variable *>(Root)) {
            return VariableRoot->vartag == Variable::GlobalVariable ||
                   VariableRoot->vartag == Variable::GlobalConstant;
        }
        return dynamic_cast<AllocaInst *>(Root) != nullptr;
    };
    auto ProvesAxisIndependent = [&](Loop *Axis) {
        if (!Rectangular || !Axis) return false;
        if (LA.areIterationsIndependent(Outer, Axis)) return true;
        const LoopAccessSummary &Summary = LA.getSummary(Outer);
        if (Summary.HasUnknownMemory) return false;
        constexpr std::size_t MaxFallbackAccessPairs = 8192;
        std::size_t ExaminedPairs = 0;
        for (std::size_t FirstIndex = 0;
             FirstIndex < Summary.Accesses.size(); ++FirstIndex) {
            for (std::size_t SecondIndex = FirstIndex;
                 SecondIndex < Summary.Accesses.size(); ++SecondIndex) {
                const LoopMemoryAccess *First =
                    Summary.Accesses[FirstIndex];
                const LoopMemoryAccess *Second =
                    Summary.Accesses[SecondIndex];
                if (!First || !Second ||
                    (!First->Writes && !Second->Writes)) {
                    continue;
                }
                if (++ExaminedPairs > MaxFallbackAccessPairs)
                    return false;
                LoopAccessPairKind Kind = LA.classifyPair(
                    Outer, Axis, *First, Outer, Axis, *Second);
                if (Kind != LoopAccessPairKind::Unknown) continue;
                if (First->Root && Second->Root &&
                    First->Root != Second->Root &&
                    IsUniqueObject(First->Root) &&
                    IsUniqueObject(Second->Root)) {
                    continue;
                }
                return false;
            }
        }
        return true;
    };
    const bool OuterIndependent =
        Rectangular && ProvesAxisIndependent(Outer);
    const bool InnerIndependent =
        OuterIndependent && ProvesAxisIndependent(Inner);
    if (!InnerIndependent) {
        return false;
    }
    auto OuterTrip = SE.analyzeLoopTripCount(Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Inner);
    auto OuterInduction = SE.getCanonicalInduction(Outer);
    auto InnerInduction = SE.getCanonicalInduction(Inner);
    if (!OuterTrip || !InnerTrip || !OuterInduction ||
        !InnerInduction) {
        return false;
    }
    Shape.Outer = Outer;
    Shape.Inner = Inner;
    if (!Outer->getLoopSimplifyForm(
            Shape.OuterPreHeader, Shape.OuterLatch) ||
        !Inner->getLoopSimplifyForm(
            Shape.InnerPreHeader, Shape.InnerLatch) ||
        !Shape.OuterPreHeader || !Shape.OuterLatch ||
        !Shape.InnerPreHeader || !Shape.InnerLatch ||
        !matchTilingAxis(
            Outer, *OuterTrip, *OuterInduction, Shape.OuterIV,
            Shape.OuterCmp, Shape.OuterStartValue,
            Shape.OuterBoundValue, Shape.OuterStartIncomingIndex,
            Shape.OuterBoundOperandIndex, Shape.OuterTripCount) ||
        !matchTilingAxis(
            Inner, *InnerTrip, *InnerInduction, Shape.InnerIV,
            Shape.InnerCmp, Shape.InnerStartValue,
            Shape.InnerBoundValue, Shape.InnerStartIncomingIndex,
            Shape.InnerBoundOperandIndex, Shape.InnerTripCount)) {
        return false;
    }
    Shape.OuterHeader = Outer->getHeader();
    Shape.InnerHeader = Inner->getHeader();
    Shape.OuterExit = OuterTrip->ExitBlock;
    Shape.InnerExit = InnerTrip->ExitBlock;
    if (!Shape.OuterHeader || !Shape.InnerHeader || !Shape.OuterExit ||
        !Shape.InnerExit || Shape.InnerExit == Shape.OuterExit ||
        !Outer->contains(Shape.InnerExit) ||
        Inner->contains(Shape.InnerExit) ||
        !LCSSAPass::isLCSSAForm(Outer) ||
        !LCSSAPass::isLCSSAForm(Inner) || hasLoopLiveOut(Outer) ||
        !hasOnlyCanonicalHeaderPhi(Outer, Shape.OuterIV) ||
        !hasOnlyCanonicalHeaderPhi(Inner, Shape.InnerIV) ||
        !hasControlOnlyHeader(
            Outer, Shape.OuterIV, *OuterTrip, false) ||
        !hasControlOnlyHeader(
            Inner, Shape.InnerIV, *InnerTrip, false) ||
        !hasSingleExitEdge(Outer, *OuterTrip) ||
        !hasSingleExitEdge(Inner, *InnerTrip) ||
        !hasCompleteIterationPath(
            Outer, *OuterTrip, Shape.OuterLatch, PDT) ||
        !hasCompleteIterationPath(
            Inner, *InnerTrip, Shape.InnerLatch, PDT)) {
        return false;
    }
    //2.外轴直接块固定为头、内轴预头、内轴专用出口和回边，排除循环间前后缀语义。
    std::unordered_set<BasicBlock *> ExpectedOuterBlocks{
        Shape.OuterHeader, Shape.InnerPreHeader,
        Shape.InnerExit, Shape.OuterLatch};
    std::unordered_set<BasicBlock *> ActualOuterBlocks(
        Outer->getBlocks().begin(), Outer->getBlocks().end());
    const std::size_t ExpectedDirectBlockCount =
        Shape.InnerExit == Shape.OuterLatch ? 3U : 4U;
    if (ExpectedOuterBlocks.size() != ExpectedDirectBlockCount ||
        ActualOuterBlocks != ExpectedOuterBlocks ||
        Shape.OuterExit->collectPhiNodes().size() != 0) {
        return false;
    }
    Shape.OuterPreBranch =
        Shape.OuterPreHeader->getBranchTerminator();
    Shape.OuterHeaderBranch =
        Shape.OuterHeader->getBranchTerminator();
    Shape.InnerPreBranch =
        Shape.InnerPreHeader->getBranchTerminator();
    Shape.InnerHeaderBranch =
        Shape.InnerHeader->getBranchTerminator();
    Shape.InnerExitBranch =
        Shape.InnerExit->getBranchTerminator();
    if (!BasicBlock::isUncondBr(Shape.OuterPreBranch) ||
        Shape.OuterPreBranch->getSuccessor(0) != Shape.OuterHeader ||
        !BasicBlock::isCondBr(Shape.OuterHeaderBranch) ||
        Shape.OuterHeaderBranch->getCondition() != Shape.OuterCmp ||
        Shape.OuterHeaderBranch->getTrueSuccessor() !=
            Shape.InnerPreHeader ||
        Shape.OuterHeaderBranch->getFalseSuccessor() !=
            Shape.OuterExit ||
        !BasicBlock::isCondBr(Shape.InnerHeaderBranch) ||
        Shape.InnerHeaderBranch->getCondition() != Shape.InnerCmp ||
        Shape.InnerHeaderBranch->getFalseSuccessor() !=
            Shape.InnerExit ||
        !Inner->contains(
            Shape.InnerHeaderBranch->getTrueSuccessor()) ||
        !hasOnlyTilingControl(
            Shape.InnerPreHeader, {}, Shape.InnerHeader) ||
        (Shape.InnerExit != Shape.OuterLatch &&
         !hasOnlyTilingControl(
             Shape.InnerExit, {}, Shape.OuterLatch)) ||
        !hasOnlyTilingControl(
            Shape.OuterLatch,
            {dynamic_cast<Instruction *>(
                OuterInduction->UpdateValue)},
            Shape.OuterHeader) ||
        !BasicBlock::isUncondBr(
            Shape.InnerLatch->getBranchTerminator()) ||
        Shape.InnerLatch->getBranchTerminator()->getSuccessor(0) !=
            Shape.InnerHeader) {
        return false;
    }
    //3.全部后代循环必须有限、规范且单出口；数据相关条件分支属于顺序屏障并使候选失败。
    std::unordered_set<BasicBlock *> LoopExitControls;
    for (Loop *Nested : Outer->getLoopsInPostOrder()) {
        auto Trip = SE.analyzeLoopTripCount(Nested);
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!Nested || !Trip || !Trip->Valid || !Trip->ProvenNoWrap ||
            !SE.isKnownFiniteLoop(Nested) ||
            !Nested->getLoopSimplifyForm(PreHeader, Latch) ||
            !PreHeader || !Latch || !LCSSAPass::isLCSSAForm(Nested) ||
            !hasSingleExitEdge(Nested, *Trip) ||
            !hasCompleteIterationPath(Nested, *Trip, Latch, PDT) ||
            !DT.dominates(Nested->getHeader(), Latch)) {
            return false;
        }
        LoopExitControls.insert(Trip->ExitingBlock);
    }
    for (BasicBlock *BB : Outer->getBlocksIncludingSubLoops()) {
        if (!BB || !BB->getTerminator()) return false;
        BrInst *Branch = BB->getBranchTerminator();
        if (BasicBlock::isCondBr(Branch) &&
            !LoopExitControls.count(BB)) {
            return false;
        }
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<CallInst *>(I) ||
                dynamic_cast<AllocaInst *>(I) ||
                dynamic_cast<RetInst *>(I) ||
                (!dynamic_cast<LoadInst *>(I) &&
                 !dynamic_cast<StoreInst *>(I) &&
                 instructionMayFaultOrProducePoison(I))) {
                return false;
            }
        }
    }
    //4.访问摘要不得含未知内存；每个标量访问都必须精确证明地址、执行域和对象边界。
    const LoopAccessSummary &Summary = LA.getSummary(Outer);
    if (Summary.HasUnknownMemory || !Summary.HasWrite ||
        Summary.Accesses.empty() ||
        Summary.Accesses.size() != Summary.Footprints.size()) {
        return false;
    }
    unsigned LoadCount = 0;
    unsigned StoreCount = 0;
    LoadInst *OnlyLoad = nullptr;
    StoreInst *OnlyStore = nullptr;
    for (std::size_t Index = 0; Index < Summary.Accesses.size(); ++Index) {
        const LoopMemoryAccess *Access = Summary.Accesses[Index];
        const LoopAccessFootprint &Footprint = Summary.Footprints[Index];
        if (!Access || !Access->Inst || !Footprint.Exact ||
            Footprint.Access != Access || Footprint.ElementSize == 0 ||
            (!dynamic_cast<LoadInst *>(Access->Inst) &&
             !dynamic_cast<StoreInst *>(Access->Inst))) {
            return false;
        }
        if (auto *Load = dynamic_cast<LoadInst *>(Access->Inst)) {
            ++LoadCount;
            OnlyLoad = Load;
        }
        if (auto *Store = dynamic_cast<StoreInst *>(Access->Inst)) {
            ++StoreCount;
            OnlyStore = Store;
        }
    }
    //5.单load直达单store的纯复制交给后续idiom/仿射视图消除，避免分块阻断更强替换。
    if (LoadCount == 1 && StoreCount == 1 && OnlyLoad && OnlyStore &&
        OnlyStore->getValueOperand() == OnlyLoad &&
        OnlyLoad->hasExactlyOneUse()) {
        return false;
    }
    return true;
}
//估算一个仿射标量访问在二维tile中触及的缓存行数量。
std::optional<std::int64_t>
UnrollAndJamPass::estimateTileCacheLines(
    std::int64_t OuterStride, std::int64_t InnerStride,
    std::size_t ElementBytes, std::size_t Alignment,
    std::int64_t OuterTile, std::int64_t InnerTile,
    std::int64_t CacheLineBytes) {
    if (ElementBytes == 0 || OuterTile <= 0 || InnerTile <= 0 ||
        CacheLineBytes <= 0 ||
        ElementBytes > static_cast<std::size_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    auto Magnitude = [](std::int64_t Value) -> __int128 {
        return Value < 0 ? -static_cast<__int128>(Value)
                         : static_cast<__int128>(Value);
    };
    //未知的缓存行内相位取对齐约束允许的最坏值，使工作集估计只会偏大。
    const std::size_t LineBytes =
        static_cast<std::size_t>(CacheLineBytes);
    const std::size_t AddressAlignment =
        std::max<std::size_t>(1, Alignment);
    const std::size_t PhaseStep =
        std::gcd(AddressAlignment, LineBytes);
    const __int128 MaxStartPhase =
        static_cast<__int128>(LineBytes - PhaseStep);
    auto WorstCaseLines = [CacheLineBytes, MaxStartPhase](
                              __int128 Bytes) -> __int128 {
        return (MaxStartPhase + Bytes + CacheLineBytes - 1) /
               CacheLineBytes;
    };
    const __int128 Outer = Magnitude(OuterStride);
    const __int128 Inner = Magnitude(InnerStride);
    const __int128 Element = static_cast<__int128>(ElementBytes);
    const __int128 ElementLines = WorstCaseLines(Element);
    __int128 Lines = 0;
    if (Outer == 0 && Inner == 0) {
        Lines = ElementLines;
    } else if (Outer == 0) {
        if (Inner < CacheLineBytes) {
            Lines = WorstCaseLines(
                Inner * (InnerTile - 1) + Element);
        } else {
            Lines = static_cast<__int128>(InnerTile) * ElementLines;
        }
    } else if (Inner == 0) {
        if (Outer < CacheLineBytes) {
            Lines = WorstCaseLines(
                Outer * (OuterTile - 1) + Element);
        } else {
            Lines = static_cast<__int128>(OuterTile) * ElementLines;
        }
    } else if (Outer < CacheLineBytes && Inner >= CacheLineBytes) {
        const __int128 ShortSpan =
            Outer * (OuterTile - 1) + Element;
        Lines = static_cast<__int128>(InnerTile) *
                WorstCaseLines(ShortSpan);
    } else if (Inner < CacheLineBytes && Outer >= CacheLineBytes) {
        const __int128 ShortSpan =
            Inner * (InnerTile - 1) + Element;
        Lines = static_cast<__int128>(OuterTile) *
                WorstCaseLines(ShortSpan);
    } else if (Outer < CacheLineBytes && Inner < CacheLineBytes) {
        Lines = WorstCaseLines(
            Outer * (OuterTile - 1) +
            Inner * (InnerTile - 1) + Element);
    } else {
        Lines = static_cast<__int128>(OuterTile) * InnerTile *
                ElementLines;
    }
    if (Lines <= 0 || Lines > std::numeric_limits<std::int64_t>::max())
        return std::nullopt;
    return static_cast<std::int64_t>(Lines);
}
//使用同一缓存行与工作集模型选择两轴都整除的tile。
std::optional<LoopTileChoice> UnrollAndJamPass::selectTile(
    const LoopTilingShape &Shape, LoopAccessInfo &LA,
    const UnrollAndJamConfig &C) {
    if (!Shape.Outer || !Shape.Inner || Shape.OuterTripCount < 4 ||
        Shape.InnerTripCount < 4 || C.CacheLineBytes <= 0 ||
        C.TileWorkingSetBytes < C.CacheLineBytes ||
        C.MaxTileSize < 2 || C.MinTileSavedCacheLines <= 0) {
        return std::nullopt;
    }
    const LoopAccessSummary &Summary = LA.getSummary(Shape.Outer);
    if (Summary.HasUnknownMemory || Summary.Accesses.empty() ||
        Summary.Accesses.size() != Summary.Footprints.size()) {
        return std::nullopt;
    }
    //同一根对象的多个静态访问可能覆盖重叠缓存行；首版不猜测它们的并集大小。
    std::unordered_set<Value *> ModeledRoots;
    for (const LoopAccessFootprint &Footprint : Summary.Footprints) {
        if (!Footprint.Base ||
            !ModeledRoots.insert(Footprint.Base).second) {
            return std::nullopt;
        }
    }
    //给出单轴扫描触及缓存行数的相位无关下界，只用下界证明原顺序超过预算。
    auto EstimateScanLowerLines = [&](std::int64_t Stride,
                                      std::size_t ElementBytes,
                                      std::int64_t Count)
        -> std::optional<std::int64_t> {
        if (ElementBytes == 0 || Count <= 0 ||
            ElementBytes > static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        const __int128 Magnitude = Stride < 0
            ? -static_cast<__int128>(Stride)
            : static_cast<__int128>(Stride);
        const __int128 Element =
            static_cast<__int128>(ElementBytes);
        __int128 Lines = 1;
        if (Magnitude >= C.CacheLineBytes) {
            Lines = Count;
        } else if (Magnitude != 0) {
            const __int128 Span =
                Magnitude * (Count - 1) + Element;
            Lines = (Span + C.CacheLineBytes - 1) /
                    C.CacheLineBytes;
        } else {
            Lines = (Element + C.CacheLineBytes - 1) /
                    C.CacheLineBytes;
        }
        if (Lines <= 0 ||
            Lines > std::numeric_limits<std::int64_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(Lines);
    };
    //1.完整未分块工作集本来就能落入预算时不增加循环层级。
    __int128 FullWorkingSet = 0;
    for (std::size_t Index = 0; Index < Summary.Accesses.size(); ++Index) {
        const LoopMemoryAccess *Access = Summary.Accesses[Index];
        const LoopAccessFootprint &Footprint = Summary.Footprints[Index];
        auto OuterStride = Access && Access->Inst
            ? LA.getByteStride(
                Access->Inst, Shape.Outer, Shape.Outer)
            : std::nullopt;
        auto InnerStride = Access && Access->Inst
            ? LA.getByteStride(
                Access->Inst, Shape.Outer, Shape.Inner)
            : std::nullopt;
        if (!Access || !Footprint.Exact || !OuterStride ||
            !InnerStride || Footprint.ElementSize == 0) {
            return std::nullopt;
        }
        auto Lines = estimateTileCacheLines(
            *OuterStride, *InnerStride, Footprint.ElementSize,
            Footprint.Alignment,
            Shape.OuterTripCount, Shape.InnerTripCount,
            C.CacheLineBytes);
        if (!Lines) return std::nullopt;
        FullWorkingSet += static_cast<__int128>(*Lines) *
                          C.CacheLineBytes;
    }
    if (FullWorkingSet <= C.TileWorkingSetBytes)
        return std::nullopt;
    LoopTileChoice Best;
    __int128 BestScore = 0;
    //2.候选完全由常量次数的整除因子产生，边缘tile与动态min均不进入首版实现。
    const std::int64_t MaxOuter = std::min(
        C.MaxTileSize, Shape.OuterTripCount / 2);
    const std::int64_t MaxInner = std::min(
        C.MaxTileSize, Shape.InnerTripCount / 2);
    for (std::int64_t OuterTile = 2;
         OuterTile <= MaxOuter; ++OuterTile) {
        if (Shape.OuterTripCount % OuterTile != 0) continue;
        for (std::int64_t InnerTile = 2;
             InnerTile <= MaxInner; ++InnerTile) {
            if (Shape.InnerTripCount % InnerTile != 0) continue;
            __int128 WorkingSet = 0;
            __int128 SavedLines = 0;
            for (std::size_t Index = 0;
                 Index < Summary.Accesses.size(); ++Index) {
                const LoopMemoryAccess *Access = Summary.Accesses[Index];
                const LoopAccessFootprint &Footprint =
                    Summary.Footprints[Index];
                auto OuterStride = LA.getByteStride(
                    Access->Inst, Shape.Outer, Shape.Outer);
                auto InnerStride = LA.getByteStride(
                    Access->Inst, Shape.Outer, Shape.Inner);
                auto TileLines = estimateTileCacheLines(
                    *OuterStride, *InnerStride,
                    Footprint.ElementSize, Footprint.Alignment,
                    OuterTile, InnerTile,
                    C.CacheLineBytes);
                if (!TileLines) {
                    WorkingSet = C.TileWorkingSetBytes + 1;
                    break;
                }
                WorkingSet += static_cast<__int128>(*TileLines) *
                              C.CacheLineBytes;
                if (!Access->Reads) continue;
                auto SingleOuterLowerLines = EstimateScanLowerLines(
                    *InnerStride, Footprint.ElementSize, InnerTile);
                auto FullScanLowerLines = EstimateScanLowerLines(
                    *InnerStride, Footprint.ElementSize,
                    Shape.InnerTripCount);
                if (!SingleOuterLowerLines || !FullScanLowerLines ||
                    static_cast<__int128>(*FullScanLowerLines) *
                        C.CacheLineBytes <= C.TileWorkingSetBytes) {
                    continue;
                }
                const __int128 OuterMagnitude = *OuterStride < 0
                    ? -static_cast<__int128>(*OuterStride)
                    : static_cast<__int128>(*OuterStride);
                const __int128 InnerMagnitude = *InnerStride < 0
                    ? -static_cast<__int128>(*InnerStride)
                    : static_cast<__int128>(*InnerStride);
                __int128 BaselineLines = 0;
                //2.1 外轴不变的长扫描可由同一tile中的相邻外迭代复用。
                if (*OuterStride == 0 && *InnerStride != 0) {
                    BaselineLines =
                        static_cast<__int128>(OuterTile) *
                        *SingleOuterLowerLines;
                }
                //2.2 内轴跨行而外轴落在相邻元素时，分块把相邻列重新聚集到同一缓存行。
                if (OuterMagnitude > 0 &&
                    OuterMagnitude <= Footprint.ElementSize &&
                    InnerMagnitude >= C.CacheLineBytes) {
                    const __int128 ElementLines =
                        (static_cast<__int128>(Footprint.ElementSize) +
                         C.CacheLineBytes - 1) /
                        C.CacheLineBytes;
                    BaselineLines = std::max(
                        BaselineLines,
                        static_cast<__int128>(OuterTile) *
                            InnerTile * ElementLines);
                }
                if (BaselineLines > *TileLines)
                    SavedLines += BaselineLines - *TileLines;
            }
            if (WorkingSet > C.TileWorkingSetBytes ||
                SavedLines < C.MinTileSavedCacheLines) {
                continue;
            }
            const __int128 OuterTiles =
                Shape.OuterTripCount / OuterTile;
            const __int128 InnerTiles =
                Shape.InnerTripCount / InnerTile;
            const __int128 TileCount = OuterTiles * InnerTiles;
            //2.3 精确计算变换前后规范控制指令的动态次数，再以保守流量权重扣除新增开销。
            const __int128 OuterTrip = Shape.OuterTripCount;
            const __int128 InnerTrip = Shape.InnerTripCount;
            const __int128 OriginalControl =
                2 * (OuterTrip + 1) + OuterTrip +
                2 * OuterTrip * (InnerTrip + 1) +
                2 * OuterTrip * InnerTrip +
                (Shape.InnerExit != Shape.OuterLatch
                     ? 3 * OuterTrip
                     : 2 * OuterTrip);
            const __int128 TiledControl =
                2 * (OuterTiles + 1) + OuterTiles +
                2 * OuterTiles * (InnerTiles + 1) +
                2 * TileCount + 2 * TileCount + 2 * OuterTiles +
                2 * TileCount * (OuterTile + 1) +
                2 * OuterTrip * InnerTiles +
                2 * OuterTrip * InnerTiles * (InnerTile + 1) +
                2 * OuterTrip * InnerTrip +
                (Shape.InnerExit != Shape.OuterLatch
                     ? 3 * OuterTrip * InnerTiles
                     : 2 * OuterTrip * InnerTiles);
            const __int128 ExtraControl =
                std::max<__int128>(0, TiledControl - OriginalControl);
            const __int128 TotalSavedBytes =
                TileCount * SavedLines * C.CacheLineBytes;
            const __int128 ControlCost =
                ExtraControl *
                static_cast<__int128>(2 * sizeof(std::int32_t));
            const __int128 Score = TotalSavedBytes - ControlCost;
            if (Score <= 0) continue;
            const bool Better = Score > BestScore ||
                (Score == BestScore &&
                 (Best.WorkingSetBytes == 0 ||
                  WorkingSet < Best.WorkingSetBytes));
            if (!Better) continue;
            BestScore = Score;
            Best.OuterTile = OuterTile;
            Best.InnerTile = InnerTile;
            Best.WorkingSetBytes =
                static_cast<std::int64_t>(WorkingSet);
            Best.SavedCacheLines =
                static_cast<std::int64_t>(SavedLines);
        }
    }
    return BestScore > 0
        ? std::optional<LoopTileChoice>(Best)
        : std::nullopt;
}
//把原两层循环复用为point-i/point-j，并在其外增加两个整除tile循环。
bool UnrollAndJamPass::applyTiling(
    Function &F, LoopTilingShape &Shape,
    const LoopTileChoice &Choice,
    std::vector<BasicBlock *> &NewHeaders) {
    NewHeaders.clear();
    if (!Shape.Outer || !Shape.Inner || !Shape.OuterIV ||
        !Shape.InnerIV || !Shape.OuterCmp || !Shape.InnerCmp ||
        Choice.OuterTile < 2 || Choice.InnerTile < 2 ||
        Shape.OuterTripCount % Choice.OuterTile != 0 ||
        Shape.InnerTripCount % Choice.InnerTile != 0 ||
        Shape.OuterIV->getIRType() != Shape.InnerIV->getIRType() ||
        !Shape.OuterPreBranch || !Shape.OuterHeaderBranch ||
        !Shape.InnerPreBranch) {
        return false;
    }
    auto Ty = Shape.OuterIV->getIRType();
    Value *OuterTileValue = Value::makeIntegerForType(
        Choice.OuterTile, Ty);
    Value *InnerTileValue = Value::makeIntegerForType(
        Choice.InnerTile, Ty);
    if (!OuterTileValue || !InnerTileValue) return false;
    //1.预提交重新核验全部将被改写的边、PHI入口和比较上界仍与候选快照一致。
    if (Shape.OuterPreBranch->getSuccessor(0) != Shape.OuterHeader ||
        Shape.OuterHeaderBranch->getFalseSuccessor() !=
            Shape.OuterExit ||
        Shape.OuterIV->getIncomingValue(
            Shape.OuterStartIncomingIndex) != Shape.OuterStartValue ||
        Shape.OuterIV->getIncomingBlock(
            Shape.OuterStartIncomingIndex) != Shape.OuterPreHeader ||
        Shape.InnerIV->getIncomingValue(
            Shape.InnerStartIncomingIndex) != Shape.InnerStartValue ||
        Shape.InnerIV->getIncomingBlock(
            Shape.InnerStartIncomingIndex) != Shape.InnerPreHeader ||
        Shape.OuterCmp->getOperand(Shape.OuterBoundOperandIndex) !=
            Shape.OuterBoundValue ||
        Shape.InnerCmp->getOperand(Shape.InnerBoundOperandIndex) !=
            Shape.InnerBoundValue) {
        return false;
    }
    auto *TileIHeader = new BasicBlock();
    auto *TileJPreHeader = new BasicBlock();
    auto *TileJHeader = new BasicBlock();
    auto *PointIPreHeader = new BasicBlock();
    auto *TileJLatch = new BasicBlock();
    auto *TileILatch = new BasicBlock();
    TileIHeader->setIdent(F.makeUniqueBlockIdent(
        Shape.OuterHeader->getIdent() + ".tile.i.header"));
    TileJPreHeader->setIdent(F.makeUniqueBlockIdent(
        Shape.InnerHeader->getIdent() + ".tile.j.preheader"));
    TileJHeader->setIdent(F.makeUniqueBlockIdent(
        Shape.InnerHeader->getIdent() + ".tile.j.header"));
    PointIPreHeader->setIdent(F.makeUniqueBlockIdent(
        Shape.OuterHeader->getIdent() + ".tile.point.i.preheader"));
    TileJLatch->setIdent(F.makeUniqueBlockIdent(
        Shape.InnerHeader->getIdent() + ".tile.j.latch"));
    TileILatch->setIdent(F.makeUniqueBlockIdent(
        Shape.OuterHeader->getIdent() + ".tile.i.latch"));
    //2.新tile轴沿用原轴类型、起点与上界；整除证明保证每个point边界均不超过原上界。
    auto *TileIIV = new PhiInst(Ty);
    TileIIV->setIdent(Shape.OuterIV->getIdent() + ".tile");
    auto *TileICmp = new BinaryInst(
        TileIIV, BinaryInst::L, Shape.OuterBoundValue);
    TileIHeader->pushBack(TileIIV);
    TileIHeader->pushBack(TileICmp);
    TileIHeader->genCondInst(
        TileICmp, TileJPreHeader, Shape.OuterExit);
    TileJPreHeader->genUnCondInst(TileJHeader);
    auto *TileJIV = new PhiInst(Ty);
    TileJIV->setIdent(Shape.InnerIV->getIdent() + ".tile");
    auto *TileJCmp = new BinaryInst(
        TileJIV, BinaryInst::L, Shape.InnerBoundValue);
    TileJHeader->pushBack(TileJIV);
    TileJHeader->pushBack(TileJCmp);
    TileJHeader->genCondInst(
        TileJCmp, PointIPreHeader, TileILatch);
    auto *PointIEnd = new BinaryInst(
        TileIIV, BinaryInst::ADD, OuterTileValue);
    PointIPreHeader->pushBack(PointIEnd);
    PointIPreHeader->genUnCondInst(Shape.OuterHeader);
    auto *TileJNext = new BinaryInst(
        TileJIV, BinaryInst::ADD, InnerTileValue);
    TileJLatch->pushBack(TileJNext);
    TileJLatch->genUnCondInst(TileJHeader);
    auto *TileINext = new BinaryInst(
        TileIIV, BinaryInst::ADD, OuterTileValue);
    TileILatch->pushBack(TileINext);
    TileILatch->genUnCondInst(TileIHeader);
    TileIIV->addIncoming(Shape.OuterStartValue, Shape.OuterPreHeader);
    TileIIV->addIncoming(TileINext, TileILatch);
    TileJIV->addIncoming(Shape.InnerStartValue, TileJPreHeader);
    TileJIV->addIncoming(TileJNext, TileJLatch);
    auto *PointJEnd = new BinaryInst(
        TileJIV, BinaryInst::ADD, InnerTileValue);
    //3.全部可失败检查已结束；挂接新块后集中重定向原控制，不保留半提交路径。
    F.pushBlock(TileIHeader);
    F.pushBlock(TileJPreHeader);
    F.pushBlock(TileJHeader);
    F.pushBlock(PointIPreHeader);
    F.pushBlock(TileJLatch);
    F.pushBlock(TileILatch);
    const bool InsertedPointJEnd =
        Instruction::insertBefore(Shape.InnerPreBranch, PointJEnd);
    const bool RedirectedEntry =
        Shape.OuterPreBranch->setSuccessor(0, TileIHeader);
    const bool RedirectedPointIExit =
        Shape.OuterHeaderBranch->setSuccessor(1, TileJLatch);
    const bool ReplacedOuterBound = Shape.OuterCmp->replaceOperand(
        Shape.OuterBoundOperandIndex, PointIEnd);
    const bool ReplacedInnerBound = Shape.InnerCmp->replaceOperand(
        Shape.InnerBoundOperandIndex, PointJEnd);
    Shape.OuterIV->setIncomingValue(
        Shape.OuterStartIncomingIndex, TileIIV);
    const bool ReplacedOuterPred = Shape.OuterIV->setIncomingBlock(
        Shape.OuterStartIncomingIndex, PointIPreHeader);
    Shape.InnerIV->setIncomingValue(
        Shape.InnerStartIncomingIndex, TileJIV);
    assert(InsertedPointJEnd && RedirectedEntry &&
           RedirectedPointIExit && ReplacedOuterBound &&
           ReplacedInnerBound && ReplacedOuterPred &&
           "validated two-dimensional tiling commit failed");
    (void)InsertedPointJEnd;
    (void)RedirectedEntry;
    (void)RedirectedPointIExit;
    (void)ReplacedOuterBound;
    (void)ReplacedInnerBound;
    (void)ReplacedOuterPred;
    NewHeaders = {TileIHeader, TileJHeader,
                  Shape.OuterHeader, Shape.InnerHeader};
    return true;
}
//2.1 外循环不允许任何值活出，因为 jam 会在同一内循环树中并列多个外轴 lane，目前没有为外部消费者建版本合流。
bool UnrollAndJamPass::hasLoopLiveOut(Loop *L) {
    if (!L) return true;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return true;
        for (Instruction *I : *BB) {
            if (!I) return true;
            for (Use *U : I->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (!UserI || !UserI->getParent() ||
                    !L->contains(UserI->getParent())) {
                    return true;
                }
            }
        }
    }
    return false;
}
//检查循环头是否只包含指定的规范归纳PHI。
bool UnrollAndJamPass::hasOnlyCanonicalHeaderPhi(Loop *L, PhiInst *IV) {
    if (!L || !L->getHeader() || !IV) return false;
    const auto Phis = L->getHeader()->collectPhiNodes();
    return Phis.size() == 1 && Phis.front() == IV;
}
//检查循环头除规范归纳量和允许的归约外是否只承担循环控制。
bool UnrollAndJamPass::hasControlOnlyHeader(
    Loop *L, PhiInst *IV, const LoopTripCountInfo &Trip,
    bool AllowReductionPhis) {
    if (!L || !L->getHeader() || !IV || !Trip.ExitCmp) {
        return false;
    }
    auto *Branch = L->getHeader()->getBranchTerminator();
    const bool HeaderTested = Trip.ExitCmp->getParent() == L->getHeader();
    if ((HeaderTested &&
         (!BasicBlock::isCondBr(Branch) ||
          Branch->getCondition() != Trip.ExitCmp)) ||
        (!HeaderTested && !BasicBlock::isUncondBr(Branch))) {
        return false;
    }
    for (Instruction *I : *L->getHeader()) {
        if (I == IV || I == Branch ||
            (HeaderTested && I == Trip.ExitCmp)) continue;
        if (AllowReductionPhis && dynamic_cast<PhiInst *>(I)) continue;
        return false;
    }
    return true;
}
//2.2 递归收集将被所有 lane 共享的内循环树控制。每一轴必须有有限、正步长的规范 IV，且域不能引用外轴。
bool UnrollAndJamPass::collectSharedLoopControl(
    UnrollAndJamShape &Shape, Loop *Current, SCEVResult &SE,
    RangeInfo &RI, DominatorTree &DT,
    const UnrollAndJamConfig &C, unsigned Depth) {
    if (!Current || !Shape.Outer || Depth > C.MaxSharedNestDepth) {
        return false;
    }
    auto Trip = SE.analyzeLoopTripCount(Current);
    auto Induction = SE.getCanonicalInduction(Current);
    const bool RuntimeNoWrap = Trip && Trip->IsRuntime &&
        SE.proveRuntimeNoWrapAtPreheader(*Trip, RI, DT);
    if (Current == Shape.Inner)
        Shape.InnerRuntimeNoWrapProven = RuntimeNoWrap;
    if (!Trip || !Trip->Valid || !Trip->Start || !Trip->Step ||
        !Trip->Bound || !Trip->ExitCmp || !Trip->ExitingBlock ||
        (!Trip->ProvenNoWrap && !RuntimeNoWrap) ||
        !Induction || !Induction->Valid || !Induction->Phi ||
        !Induction->UpdateValue || !Induction->ConstantStep ||
        *Induction->ConstantStep <= 0 ||
        Trip->Start->mentionsLoop(Shape.Outer) ||
        Trip->Step->mentionsLoop(Shape.Outer) ||
        Trip->Bound->mentionsLoop(Shape.Outer) ||
        !hasControlOnlyHeader(Current, Induction->Phi, *Trip,
                              true)) {
        return false;
    }
    auto *Update = dynamic_cast<Instruction *>(Induction->UpdateValue);
    if (!Update) return false;
    Shape.SharedLoops.push_back(Current);
    Shape.SharedExitingBlocks.insert(Trip->ExitingBlock);
    Shape.Control.insert(Induction->Phi);
    Shape.Control.insert(Update);
    Shape.Control.insert(Trip->ExitCmp);
    for (Loop *SubLoop : Current->getSubLoops()) {
        if (!collectSharedLoopControl(
                Shape, SubLoop, SE, RI, DT, C, Depth + 1))
            return false;
    }
    return true;
}
//收集外循环和完整共享内循环树中不得为各lane克隆的控制指令。
bool UnrollAndJamPass::collectControl(
    UnrollAndJamShape &Shape, SCEVResult &SE, RangeInfo &RI,
    DominatorTree &DT,
    const UnrollAndJamConfig &C) {
    if (!Shape.Outer || !Shape.Inner || !Shape.OuterInduction.Phi ||
        !Shape.InnerInduction.Phi || !Shape.OuterInduction.UpdateValue ||
        !Shape.InnerInduction.UpdateValue || !Shape.OuterTrip.ExitCmp ||
        !Shape.InnerTrip.ExitCmp) {
        return false;
    }
    Shape.Control.insert(Shape.OuterInduction.Phi);
    Shape.Control.insert(Shape.InnerInduction.Phi);
    if (auto *I = dynamic_cast<Instruction *>(
            Shape.OuterInduction.UpdateValue)) {
        Shape.Control.insert(I);
    } else {
        return false;
    }
    if (auto *I = dynamic_cast<Instruction *>(
            Shape.InnerInduction.UpdateValue)) {
        Shape.Control.insert(I);
    } else {
        return false;
    }
    Shape.Control.insert(Shape.OuterTrip.ExitCmp);
    Shape.SharedExitingBlocks.insert(Shape.OuterTrip.ExitingBlock);
    if (!collectSharedLoopControl(
            Shape, Shape.Inner, SE, RI, DT, C, 1))
        return false;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB || !BB->getTerminator()) return false;
        Shape.Control.insert(BB->getTerminator());
    }
    return true;
}
//2.3 固定外 IV 回边入口和退出比较的被比较操作数，只接受语义等价于 IV < Bound 的正向循环。
bool UnrollAndJamPass::analyzeOuterControl(UnrollAndJamShape &Shape) {
    PhiInst *IV = Shape.OuterInduction.Phi;
    if (!IV || !Shape.Outer || !Shape.OuterLatch ||
        !Shape.OuterTrip.ExitCmp) {
        return false;
    }
    bool FoundBackedge = false;
    for (unsigned Index = 0; Index < IV->getNumIncomingValues(); ++Index) {
        BasicBlock *Pred = IV->getIncomingBlock(Index);
        if (!Pred || !IV->getIncomingValue(Index)) return false;
        if (!Shape.Outer->contains(Pred)) continue;
        if (FoundBackedge || Pred != Shape.OuterLatch ||
            IV->getIncomingValue(Index) !=
                Shape.OuterInduction.UpdateValue) {
            return false;
        }
        FoundBackedge = true;
        Shape.OuterBackedgeIncomingIndex = Index;
    }
    if (!FoundBackedge) return false;
    BinaryInst *Cmp = Shape.OuterTrip.ExitCmp;
    BinaryInst::Operation Predicate = Shape.OuterTrip.ContinueOnTrue
        ? Cmp->getOp()
        : BinaryInst::negatePredicate(Cmp->getOp());
    Value *LHS = Cmp->getOperand(0);
    Value *RHS = Cmp->getOperand(1);
    Value *Compared = nullptr;
    if (Shape.OuterTrip.ComparePhase == LoopExitComparePhase::HeaderPhi)
        Compared = IV;
    else if (Shape.OuterTrip.ComparePhase ==
             LoopExitComparePhase::BackedgeUpdate)
        Compared = Shape.OuterInduction.UpdateValue;
    else
        return false;
    if (RHS == Compared) {
        std::swap(LHS, RHS);
        Predicate = BinaryInst::reversePredicate(Predicate);
        Shape.OuterExitComparedOperand = 1;
    } else {
        Shape.OuterExitComparedOperand = 0;
    }
    if (LHS != Compared || Predicate != BinaryInst::L) return false;
    Shape.OuterExitUsesUpdate = Compared == Shape.OuterInduction.UpdateValue;
    auto *Update = dynamic_cast<Instruction *>(
        Shape.OuterInduction.UpdateValue);
    for (Use *U : Update->getUses()) {
        User *UserValue = U ? U->getUser() : nullptr;
        if (UserValue == IV ||
            (Shape.OuterExitUsesUpdate && UserValue == Cmp)) {
            continue;
        }
        return false;
    }
    return true;
}
//确认循环恰好通过迭代次数分析记录的一条退出边离开。
bool UnrollAndJamPass::hasSingleExitEdge(
    Loop *L, const LoopTripCountInfo &Trip) {
    if (!L || !Trip.ExitingBlock || !Trip.ExitBlock ||
        !L->contains(Trip.ExitingBlock) || L->contains(Trip.ExitBlock)) {
        return false;
    }
    const auto ExitBlocks = L->getExitBlocks();
    if (ExitBlocks.size() != 1 || ExitBlocks.front() != Trip.ExitBlock)
        return false;
    unsigned ExitEdgeCount = 0;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (BasicBlock *Succ : BB->getSuccessors()) {
            if (!Succ || L->contains(Succ)) continue;
            ++ExitEdgeCount;
            if (BB != Trip.ExitingBlock || Succ != Trip.ExitBlock)
                return false;
        }
    }
    return ExitEdgeCount == 1 &&
           Trip.ExitingBlock->countEdgesToSucc(Trip.ExitBlock) == 1;
}
//2.4 要求每个进入循环的迭代都到达 latch，从而保证把外步长放大 Factor 倍不会跳过部分路径的状态更新。
bool UnrollAndJamPass::hasCompleteIterationPath(
    Loop *L, const LoopTripCountInfo &Trip, BasicBlock *Latch,
    const PostDominatorTree &PDT) {
    if (!L || !L->getHeader() || !Latch || !Trip.ExitingBlock ||
        (Trip.ExitingBlock != L->getHeader() &&
         Trip.ExitingBlock != Latch) ||
        Latch->countEdgesToSucc(L->getHeader()) != 1) {
        return false;
    }
    auto *ExitBranch = Trip.ExitingBlock->getBranchTerminator();
    if (!BasicBlock::isCondBr(ExitBranch)) return false;
    BasicBlock *Continue = Trip.ContinueOnTrue
        ? ExitBranch->getTrueSuccessor()
        : ExitBranch->getFalseSuccessor();
    if (!Continue || !L->contains(Continue)) return false;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        if (BB == L->getHeader() && Trip.ExitingBlock == BB &&
            BB != Latch) {
            continue;
        }
        if (!PDT.postDominates(Latch, BB)) return false;
    }
    return PDT.postDominates(Latch, Continue);
}
//综合验证候选嵌套的规范形态、必经路径、LCSSA和副作用约束。
bool UnrollAndJamPass::validateShape(
    const UnrollAndJamShape &Shape, const LoopInfo &LI,
    SCEVResult &SE,
    const DominatorTree &DT, const PostDominatorTree &PDT,
    const UnrollAndJamConfig &C) {
    if (!Shape.Outer || !Shape.Inner ||
        Shape.Inner->getParentLoop() != Shape.Outer ||
        Shape.Outer->getSubLoops().size() != 1 ||
        Shape.Outer->getSubLoops().front() != Shape.Inner ||
        Shape.SharedLoops.empty() || !Shape.OuterPreHeader ||
        !Shape.OuterLatch || !Shape.InnerPreHeader || !Shape.InnerLatch ||
        !LCSSAPass::isLCSSAForm(Shape.Outer) ||
        !LCSSAPass::isLCSSAForm(Shape.Inner) ||
        hasLoopLiveOut(Shape.Outer) || Shape.Blocks.empty() ||
        Shape.Blocks.size() > C.MaxNestBlocks) {
        return false;
    }
    for (Loop *Shared : Shape.SharedLoops) {
        auto Trip = SE.analyzeLoopTripCount(Shared);
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!Trip || !Trip->Valid ||
            !Shared->getLoopSimplifyForm(PreHeader, Latch) ||
            !PreHeader || !Latch || !LCSSAPass::isLCSSAForm(Shared) ||
            !hasSingleExitEdge(Shared, *Trip) ||
            !hasCompleteIterationPath(Shared, *Trip, Latch, PDT) ||
            !DT.dominates(Shared->getHeader(), Latch) ||
            !PDT.isReachableToExit(Shared->getHeader())) {
            return false;
        }
    }
    if (!hasSingleExitEdge(Shape.Outer, Shape.OuterTrip) ||
        !hasSingleExitEdge(Shape.Inner, Shape.InnerTrip) ||
        !hasCompleteIterationPath(Shape.Outer, Shape.OuterTrip,
                                  Shape.OuterLatch, PDT) ||
        !hasCompleteIterationPath(Shape.Inner, Shape.InnerTrip,
                                  Shape.InnerLatch, PDT)) {
        return false;
    }
    if (!DT.dominates(Shape.Outer->getHeader(),
                      Shape.Inner->getHeader()) ||
        !DT.dominates(Shape.Inner->getHeader(), Shape.OuterLatch) ||
        !PDT.isReachableToExit(Shape.Outer->getHeader()) ||
        !PDT.isReachableToExit(Shape.Inner->getHeader())) {
        return false;
    }
    //2.4.1 除已识别的循环退出外不允许条件 CFG；调用、分配、返回和可故障非访存指令也全部拒绝。
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB || !BB->getTerminator()) return false;
        auto *Branch = BB->getBranchTerminator();
        if (BasicBlock::isCondBr(Branch) &&
            !Shape.SharedExitingBlocks.count(BB)) {
            return false;
        }
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<CallInst *>(I) ||
                dynamic_cast<AllocaInst *>(I) ||
                dynamic_cast<RetInst *>(I) ||
                (!dynamic_cast<LoadInst *>(I) &&
                 !dynamic_cast<StoreInst *>(I) &&
                 instructionMayFaultOrProducePoison(I))) {
                return false;
            }
            if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
                if (!Shape.Control.count(Phi) &&
                    !isSupportedLanePhi(Shape, LI, Phi)) {
                    return false;
                }
            }
        }
    }
    (void)LI;
    return true;
}
//判断循环内PHI能否为每个lane独立克隆并保持局部递推。
bool UnrollAndJamPass::isSupportedLanePhi(
    const UnrollAndJamShape &Shape, const LoopInfo &LI, PhiInst *Phi) {
    if (!Phi || !Phi->getParent() || !Shape.Outer ||
        Phi->getParent() == Shape.Outer->getHeader() ||
        !Shape.Outer->contains(Phi->getParent()) ||
        Phi->getNumIncomingValues() == 0 || !Phi->getIRType() ||
        !Phi->getIRType()->isScalar()) {
        return false;
    }
    Loop *Owner = LI.getLoopFor(Phi->getParent());
    if (!Owner) return false;
    for (unsigned Index = 0; Index < Phi->getNumIncomingValues(); ++Index) {
        BasicBlock *Pred = Phi->getIncomingBlock(Index);
        if (!Pred || !Phi->getIncomingValue(Index) ||
            !Shape.Outer->contains(Pred)) {
            return false;
        }
    }
    return true;
}
//判断指令是否直接依赖当前已发现的lane值集合。
bool UnrollAndJamPass::instructionDependsOnLane(
    Instruction *I,
    const std::unordered_map<Value *, Value *> &ValueMap) {
    if (!I) return false;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index)
        if (ValueMap.count(I->getOperand(Index))) return true;
    return false;
}
//3.1 查找可被多个 lane 共享的读取：它对外轴不变，却被内循环遍历，且结果流向 lane 依赖计算。
bool UnrollAndJamPass::hasShareableLaneRead(
    const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
    const std::unordered_set<Instruction *> &LaneValues) {
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            auto *Load = dynamic_cast<LoadInst *>(I);
            if (!Load || LaneValues.count(Load)) continue;
            auto OuterStride = LA.getByteStride(
                Load, Shape.Outer, Shape.Outer);
            if (!OuterStride || *OuterStride != 0) continue;
            bool TraversedBySharedLoop = false;
            for (Loop *Axis : Shape.SharedLoops) {
                auto Stride = LA.getByteStride(Load, Shape.Outer, Axis);
                if (Stride && *Stride != 0) {
                    TraversedBySharedLoop = true;
                    break;
                }
            }
            if (!TraversedBySharedLoop) continue;
            for (Use *U : Load->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (UserI && LaneValues.count(UserI)) return true;
            }
        }
    }
    return false;
}
//统计直接内轴每次有效迭代必经路径上的可共享读取。
unsigned UnrollAndJamPass::countGuaranteedShareableLaneReads(
    const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
    const std::unordered_set<Instruction *> &LaneValues) {
    if (!Shape.Outer || !Shape.Inner) return 0;
    unsigned Count = 0;
    //只统计直接属于内轴的块，避免用更深层且可能零次执行的子循环读取
    //为直接内轴建立收益下界。
    for (BasicBlock *BB : Shape.Inner->getBlocks()) {
        if (!BB) return 0;
        for (Instruction *I : *BB) {
            auto *Load = dynamic_cast<LoadInst *>(I);
            if (!Load || LaneValues.count(Load)) continue;
            auto OuterStride = LA.getByteStride(
                Load, Shape.Outer, Shape.Outer);
            auto InnerStride = LA.getByteStride(
                Load, Shape.Outer, Shape.Inner);
            if (!OuterStride || *OuterStride != 0 || !InnerStride ||
                *InnerStride == 0) {
                continue;
            }
            bool FeedsLane = false;
            for (Use *U : Load->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (UserI && LaneValues.count(UserI)) {
                    FeedsLane = true;
                    break;
                }
            }
            if (FeedsLane) ++Count;
        }
    }
    return Count;
}
//3.1.1 长距离合并收益要求 lane 轴按元素大小相邻，而某共享内轴跨越足够长的内存区间。
bool UnrollAndJamPass::hasLaneCoalescingBenefit(
    const UnrollAndJamShape &Shape, LoopAccessInfo &LA, SCEVResult &SE,
    const std::unordered_set<Instruction *> &LaneValues,
    const UnrollAndJamConfig &C) {
    if (C.CacheLineBytes <= 0 || C.MinCoalescedAxisTripCount <= 0 ||
        C.MinLaneTraversalBytes <= 0)
        return false;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            auto *Load = dynamic_cast<LoadInst *>(I);
            if (!Load || !LaneValues.count(Load) || !Load->getIRType() ||
                !Load->getIRType()->isScalar()) {
                continue;
            }
            const std::size_t ElementBytes = Load->getIRType()->getSize();
            if (ElementBytes == 0 ||
                ElementBytes >
                    static_cast<std::size_t>(
                        std::numeric_limits<std::int64_t>::max())) {
                continue;
            }
            const std::int64_t ElementStride =
                static_cast<std::int64_t>(ElementBytes);
            auto OuterStride = LA.getByteStride(
                Load, Shape.Outer, Shape.Outer);
            if (!OuterStride ||
                (*OuterStride != ElementStride &&
                 *OuterStride != -ElementStride)) {
                continue;
            }
            for (Loop *Axis : Shape.SharedLoops) {
                auto Trip = SE.analyzeLoopTripCount(Axis);
                auto AxisStride = LA.getByteStride(
                    Load, Shape.Outer, Axis);
                if (!Trip || !Trip->Valid || !Trip->ConstantTripCount ||
                    *Trip->ConstantTripCount <
                        C.MinCoalescedAxisTripCount ||
                    !AxisStride ||
                    (*AxisStride < C.CacheLineBytes &&
                     *AxisStride > -C.CacheLineBytes)) {
                    continue;
                }
                const __int128 StrideMagnitude = *AxisStride < 0
                    ? -static_cast<__int128>(*AxisStride)
                    : static_cast<__int128>(*AxisStride);
                const __int128 TraversalBytes =
                    StrideMagnitude * *Trip->ConstantTripCount;
                if (TraversalBytes < C.MinLaneTraversalBytes) continue;
                return true;
            }
        }
    }
    return false;
}
//3.1.2 滑动窗口收益要求外轴和某共享内轴具有相同元素步长，且所有 lane 能落入同一缓存行。
bool UnrollAndJamPass::hasSlidingWindowBenefit(
    const UnrollAndJamShape &Shape, LoopAccessInfo &LA, SCEVResult &SE,
    const std::unordered_set<Instruction *> &LaneValues, int Factor,
    const UnrollAndJamConfig &C) {
    if (!isSupportedFactor(Factor) || C.CacheLineBytes <= 0)
        return false;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            auto *Load = dynamic_cast<LoadInst *>(I);
            if (!Load || !LaneValues.count(Load) || !Load->getIRType() ||
                !Load->getIRType()->isScalar()) {
                continue;
            }
            const std::size_t ElementBytes = Load->getIRType()->getSize();
            if (ElementBytes == 0 ||
                ElementBytes > static_cast<std::size_t>(
                                   std::numeric_limits<std::int64_t>::max())) {
                continue;
            }
            auto OuterStride = LA.getByteStride(
                Load, Shape.Outer, Shape.Outer);
            if (!OuterStride) continue;
            const std::int64_t ElementStride =
                static_cast<std::int64_t>(ElementBytes);
            if (*OuterStride != ElementStride &&
                *OuterStride != -ElementStride) {
                continue;
            }
            const __int128 Magnitude = *OuterStride < 0
                ? -static_cast<__int128>(*OuterStride)
                : static_cast<__int128>(*OuterStride);
            const __int128 LaneSpan =
                static_cast<__int128>(Factor - 1) * Magnitude +
                ElementBytes;
            if (LaneSpan > C.CacheLineBytes) continue;
            for (Loop *Axis : Shape.SharedLoops) {
                auto Trip = SE.analyzeLoopTripCount(Axis);
                auto AxisStride = LA.getByteStride(
                    Load, Shape.Outer, Axis);
                if (!Trip || !Trip->Valid || !Trip->ConstantTripCount ||
                    *Trip->ConstantTripCount <= 1 || !AxisStride ||
                    *AxisStride != *OuterStride) {
                    continue;
                }
                return true;
            }
        }
    }
    return false;
}
//3.2 沿 PHI→FADD/FSUB→PHI 链识别严格浮点归约；每个 lane 保留独立递推，不跨 lane 重新结合。
bool UnrollAndJamPass::isStrictFPRecurrencePath(
    Value *Current, PhiInst *Phi, std::unordered_set<Value *> &Active,
    unsigned Depth) {
    if (!Current || !Phi || Depth > 16) return false;
    if (Current == Phi) return true;
    if (!Active.insert(Current).second) return false;
    bool ReachesPhi = false;
    if (auto *Binary = dynamic_cast<BinaryInst *>(Current)) {
        if (Binary->instType == Instruction::FADD ||
            Binary->instType == Instruction::FSUB) {
            ReachesPhi = isStrictFPRecurrencePath(
                Binary->getOperand(0), Phi, Active, Depth + 1) ||
                isStrictFPRecurrencePath(
                    Binary->getOperand(1), Phi, Active, Depth + 1);
        }
    } else if (auto *ForwardPhi = dynamic_cast<PhiInst *>(Current)) {
        for (unsigned Index = 0;
             Index < ForwardPhi->getNumIncomingValues(); ++Index) {
            if (isStrictFPRecurrencePath(
                    ForwardPhi->getIncomingValue(Index), Phi,
                    Active, Depth + 1)) {
                ReachesPhi = true;
                break;
            }
        }
    }
    Active.erase(Current);
    return ReachesPhi;
}
//统计lane克隆闭包中的严格浮点归约数量。
unsigned UnrollAndJamPass::countStrictFPReductions(
    const UnrollAndJamShape &Shape) {
    unsigned Count = 0;
    for (Instruction *I : Shape.LaneInstructions) {
        auto *Phi = dynamic_cast<PhiInst *>(I);
        if (!Phi || !Phi->getIRType() || !Phi->getIRType()->isFloat())
            continue;
        bool IsStrictRecurrence = false;
        for (unsigned Index = 0; Index < Phi->getNumIncomingValues();
             ++Index) {
            std::unordered_set<Value *> Active;
            if (isStrictFPRecurrencePath(
                    Phi->getIncomingValue(Index), Phi, Active)) {
                IsStrictRecurrence = true;
                break;
            }
        }
        if (IsStrictRecurrence) ++Count;
    }
    return Count;
}
//估算指定展开因子的代码增长和整数、浮点寄存器压力。
bool UnrollAndJamPass::fitsRegisterBudget(
    const UnrollAndJamShape &Shape, int Factor,
    const UnrollAndJamConfig &C) {
    if (!isSupportedFactor(Factor) || C.FPRBudget == 0 ||
        C.GPRBudget == 0) {
        return false;
    }
    const __int128 Growth =
        static_cast<__int128>(Factor - 1) *
        Shape.LaneInstructions.size();
    if (Growth > C.MaxClonedInstructions) return false;
    unsigned RawLaneFPValues = 0;
    unsigned LaneAddressDepth = 0;
    unsigned LaneIntegerPhis = 0;
    for (Instruction *I : Shape.LaneInstructions) {
        if (!I || !I->getIRType()) continue;
        if (I->getIRType()->isFloat() &&
            (dynamic_cast<PhiInst *>(I) || dynamic_cast<LoadInst *>(I) ||
             dynamic_cast<BinaryInst *>(I))) {
            ++RawLaneFPValues;
        } else if (I->getIRType()->isPointer()) {
            ++LaneAddressDepth;
        } else if (I->getIRType()->isInt() &&
                   dynamic_cast<PhiInst *>(I)) {
            ++LaneIntegerPhis;
        }
    }
    //3.3 寄存器预算：地址链按最多两级同时活跃估算，浮点 load/计算/PHI 和严格归约都计入每 lane 压力。
    const unsigned StrictReductions = countStrictFPReductions(Shape);
    const unsigned LaneFPValues = StrictReductions == 0
        ? RawLaneFPValues
        : StrictReductions + 2;
    const unsigned GPRPerLane =
        1 + LaneIntegerPhis + std::min(2u, LaneAddressDepth);
    const __int128 ProjectedFPR =
        4 + static_cast<__int128>(Factor) * LaneFPValues;
    const __int128 ProjectedGPR =
        4 + static_cast<__int128>(Factor) * GPRPerLane;
    return ProjectedFPR <= C.FPRBudget && ProjectedGPR <= C.GPRBudget;
}
//检查展开因子是否属于实现支持的2、4或8。
bool UnrollAndJamPass::isSupportedFactor(int Factor) {
    return Factor == 2 || Factor == 4 || Factor == 8;
}
//使用可确定消除的动态工作和统一静态增长上限评估运行期jam因子。
bool UnrollAndJamPass::evaluateRuntimeFactor(
    const UnrollAndJamShape &Shape, LoopAccessInfo &LA, int Factor,
    const UnrollAndJamConfig &C, std::int64_t &MinInnerTrip) {
    MinInnerTrip = 0;
    if (!Shape.OuterTrip.IsRuntime ||
        !Shape.OuterRuntimeNoWrapProven ||
        !Shape.OuterTrip.TripCount || !Shape.OuterPreHeader ||
        !isSupportedFactor(Factor) || Factor > C.MaxFactor ||
        C.EstimatedSharedLoadCost == 0 ||
        !fitsRegisterBudget(Shape, Factor, C)) {
        return false;
    }
    std::unordered_set<Instruction *> LaneValues(
        Shape.LaneInstructions.begin(), Shape.LaneInstructions.end());
    const unsigned SharedLoads = countGuaranteedShareableLaneReads(
        Shape, LA, LaneValues);
    if (SharedLoads == 0) return false;
    //运行期内轴要么具有正的常量次数，要么同样已经证明不回绕且次数可在
    //外预头物化；后一种情况由执行阶段生成收益阈值守卫。
    if (Shape.InnerTrip.ConstantTripCount) {
        if (*Shape.InnerTrip.ConstantTripCount <= 0)
            return false;
    } else if (!Shape.InnerTrip.IsRuntime ||
               !Shape.InnerRuntimeNoWrapProven ||
               !Shape.InnerTrip.TripCount ||
               !Shape.InnerTripMaterializableAtOuterPreHeader) {
        return false;
    }
    LoopUnrollPass SCEVCostModel;
    const int OuterSCEVCost =
        SCEVCostModel.estimateSCEVCost(Shape.OuterTrip.TripCount);
    const int InnerSCEVCost = Shape.InnerTrip.ConstantTripCount
        ? 0
        : SCEVCostModel.estimateSCEVCost(Shape.InnerTrip.TripCount);
    if (OuterSCEVCost < 0 || InnerSCEVCost < 0 ||
        OuterSCEVCost >= 1000000 || InnerSCEVCost >= 1000000) {
        return false;
    }
    //静态增长统一计入完整标量尾副本、全部额外lane和运行期表达式/分派。
    __int128 NestInstructions = 0;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB) return false;
        NestInstructions += BB->size();
    }
    const __int128 LaneGrowth =
        static_cast<__int128>(Factor - 1) *
        Shape.LaneInstructions.size();
    const __int128 SetupGrowth =
        static_cast<__int128>(C.EstimatedRuntimeDispatchCost) +
        OuterSCEVCost + InnerSCEVCost +
        (Shape.InnerTrip.ConstantTripCount ? 0 : 2);
    const __int128 TotalGrowth =
        NestInstructions + LaneGrowth + SetupGrowth;
    if (C.MaxRuntimeClonedInstructions == 0 || TotalGrowth <= 0 ||
        TotalGrowth > C.MaxRuntimeClonedInstructions) {
        return false;
    }
    //每个完整jam组至少消除Factor-1份共享load和一份规范内循环控制；
    //用“仅有一个主组”计算阈值，使更多外迭代只会增加净收益。
    const __int128 SavingPerInnerIteration =
        static_cast<__int128>(Factor - 1) *
        (static_cast<__int128>(SharedLoads) *
             C.EstimatedSharedLoadCost +
         C.EstimatedInnerControlCost);
    const __int128 FixedDynamicCost =
        static_cast<__int128>(C.EstimatedRuntimeDispatchCost) +
        OuterSCEVCost + InnerSCEVCost +
        (Shape.InnerTrip.ConstantTripCount ? 0 : 2);
    if (SavingPerInnerIteration <= 0) return false;
    const __int128 RequiredSaving = FixedDynamicCost +
        std::max<unsigned>(1, C.MinRuntimeNetBenefit);
    const __int128 RequiredTrip =
        (RequiredSaving + SavingPerInnerIteration - 1) /
        SavingPerInnerIteration;
    if (RequiredTrip <= 0 ||
        RequiredTrip > std::numeric_limits<std::int64_t>::max()) {
        return false;
    }
    MinInnerTrip = static_cast<std::int64_t>(RequiredTrip);
    if (Shape.InnerTrip.ConstantTripCount &&
        *Shape.InnerTrip.ConstantTripCount < MinInnerTrip) {
        return false;
    }
    return true;
}
//结合共享读取、访存局部性、归约延迟和寄存器预算选择展开因子。
std::optional<int> UnrollAndJamPass::selectFactor(
    const UnrollAndJamShape &Shape, LoopAccessInfo &LA, SCEVResult &SE,
    const UnrollAndJamConfig &C, std::int64_t *RuntimeMinInnerTrip) {
    if (RuntimeMinInnerTrip) *RuntimeMinInnerTrip = 0;
    if (C.MaxFactor < 2)
        return std::nullopt;
    std::unordered_set<Instruction *> LaneValues(
        Shape.LaneInstructions.begin(), Shape.LaneInstructions.end());
    const bool Shareable = hasShareableLaneRead(Shape, LA, LaneValues);
    if (!Shareable) return std::nullopt;
    const bool RuntimeOuter = Shape.OuterTrip.IsRuntime;
    const bool LongTraversal = !RuntimeOuter &&
        hasLaneCoalescingBenefit(Shape, LA, SE, LaneValues, C);
    const unsigned StrictReductions = countStrictFPReductions(Shape);
    auto IsProfitable = [&](int Factor) {
        if (RuntimeOuter) {
            std::int64_t RequiredInnerTrip = 0;
            if (!evaluateRuntimeFactor(
                    Shape, LA, Factor, C, RequiredInnerTrip)) {
                return false;
            }
            if (RuntimeMinInnerTrip)
                *RuntimeMinInnerTrip = RequiredInnerTrip;
            return true;
        }
        if (!isSupportedFactor(Factor) || Factor > C.MaxFactor ||
            !Shape.OuterTrip.ConstantTripCount ||
            *Shape.OuterTrip.ConstantTripCount < Factor ||
            !fitsRegisterBudget(Shape, Factor, C)) {
            return false;
        }
        const bool Sliding = StrictReductions != 0 &&
            hasSlidingWindowBenefit(
                Shape, LA, SE, LaneValues, Factor, C);
        return LongTraversal || Sliding;
    };
    if (C.Factor != 0)
        return IsProfitable(C.Factor)
            ? std::optional<int>(C.Factor)
            : std::nullopt;
    int LatencyFactor = 2;
    while (LatencyFactor < static_cast<int>(C.EstimatedFPAddLatency) &&
           LatencyFactor < 8) {
        LatencyFactor *= 2;
    }
    if (StrictReductions != 0) {
        for (int Factor = LatencyFactor; Factor >= 2; Factor /= 2)
            if (IsProfitable(Factor)) return Factor;
    }
    //3.4 自动选择时，严格浮点归约从覆盖目标延迟的较大因子向下试探；无延迟链时仅保守尝试 2 路。
    if (IsProfitable(2)) return 2;
    return std::nullopt;
}
//从外归纳变量出发构造需要为每个额外lane克隆的指令闭包。
bool UnrollAndJamPass::collectLaneInstructions(
    UnrollAndJamShape &Shape, const UnrollAndJamConfig &C) {
    std::unordered_map<Value *, Value *> LaneValues;
    LaneValues[Shape.OuterInduction.Phi] = Shape.OuterInduction.Phi;
    //2.5 从外 IV 开始做依赖闭包：store 始终视为 lane 负载，其余指令在使用已标记值时加入。
    bool Changed = true;
    while (Changed) {
        Changed = false;
        for (BasicBlock *BB : Shape.Blocks) {
            for (Instruction *I : *BB) {
                if (!I || Shape.Control.count(I) || LaneValues.count(I))
                    continue;
                if (dynamic_cast<StoreInst *>(I) ||
                    instructionDependsOnLane(I, LaneValues)) {
                    LaneValues[I] = I;
                    Changed = true;
                }
            }
        }
    }
    //2.5.1 共享循环控制不得反向依赖 lane 值，否则各 lane 不再共享同一内循环域。
    for (Instruction *Control : Shape.Control) {
        if (!Control || Control == Shape.OuterInduction.UpdateValue ||
            Control == Shape.OuterTrip.ExitCmp ||
            Control == Shape.OuterInduction.Phi) {
            continue;
        }
        if (instructionDependsOnLane(Control, LaneValues)) return false;
    }
    for (BasicBlock *BB : Shape.Blocks) {
        for (Instruction *I : *BB) {
            if (!I || Shape.Control.count(I) || !LaneValues.count(I))
                continue;
            if (I->getParent() == Shape.Outer->getHeader()) {
                return false;
            }
            Shape.LaneInstructions.push_back(I);
            if (Shape.LaneInstructions.size() > C.MaxClonedInstructions)
                return false;
        }
    }
    return !Shape.LaneInstructions.empty();
}
//从相邻循环建立通过结构、控制、依赖和闭包验证的候选形状。
std::optional<UnrollAndJamShape> UnrollAndJamPass::buildShape(
    Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
    LoopAccessInfo &LA, RangeInfo &RI, DominatorTree &DT,
    PostDominatorTree &PDT,
    const UnrollAndJamConfig &C) {
    //2.候选入口先由 LoopAccessAnalysis 证明直接嵌套矩形且不同外迭代无依赖。
    auto IsUniqueObject = [](Value *Root) {
        if (auto *VariableRoot = dynamic_cast<Variable *>(Root)) {
            return VariableRoot->vartag == Variable::GlobalVariable ||
                   VariableRoot->vartag == Variable::GlobalConstant;
        }
        return dynamic_cast<AllocaInst *>(Root) != nullptr;
    };
    auto ProvesOuterIndependent = [&]() {
        if (!Outer || !LA.isRectangularNest(Outer, Inner)) return false;
        if (LA.areIterationsIndependent(Outer, Outer)) return true;
        const LoopAccessSummary &Summary = LA.getSummary(Outer);
        if (Summary.HasUnknownMemory) return false;
        constexpr std::size_t MaxFallbackAccessPairs = 8192;
        std::size_t ExaminedPairs = 0;
        for (std::size_t FirstIndex = 0;
             FirstIndex < Summary.Accesses.size(); ++FirstIndex) {
            for (std::size_t SecondIndex = FirstIndex;
                 SecondIndex < Summary.Accesses.size(); ++SecondIndex) {
                const LoopMemoryAccess *First =
                    Summary.Accesses[FirstIndex];
                const LoopMemoryAccess *Second =
                    Summary.Accesses[SecondIndex];
                if (!First || !Second ||
                    (!First->Writes && !Second->Writes)) {
                    continue;
                }
                if (++ExaminedPairs > MaxFallbackAccessPairs)
                    return false;
                LoopAccessPairKind Kind = LA.classifyPair(
                    Outer, Outer, *First, Outer, Outer, *Second);
                if (Kind != LoopAccessPairKind::Unknown) continue;
                //不同的全局对象或栈分配在语言语义上具有唯一存储；只补充
                //这一精确NoAlias事实，同一对象的Unknown仍严格拒绝。
                if (First->Root && Second->Root &&
                    First->Root != Second->Root &&
                    IsUniqueObject(First->Root) &&
                    IsUniqueObject(Second->Root)) {
                    continue;
                }
                return false;
            }
        }
        return true;
    };
    if (!Outer || !Inner ||
        !LA.isRectangularNest(Outer, Inner) ||
        !ProvesOuterIndependent()) {
        return std::nullopt;
    }
    auto OuterTrip = SE.analyzeLoopTripCount(Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Inner);
    auto OuterInduction = SE.getCanonicalInduction(Outer);
    auto InnerInduction = SE.getCanonicalInduction(Inner);
    if (!OuterTrip || !InnerTrip || !OuterInduction || !InnerInduction ||
        !OuterTrip->Valid || !InnerTrip->Valid ||
        !OuterInduction->ConstantStep ||
        *OuterInduction->ConstantStep <= 0 ||
        !hasOnlyCanonicalHeaderPhi(Outer, OuterInduction->Phi) ||
        !hasControlOnlyHeader(Outer, OuterInduction->Phi, *OuterTrip,
                              false)) {
        return std::nullopt;
    }
    UnrollAndJamShape Shape;
    Shape.Outer = Outer;
    Shape.Inner = Inner;
    Shape.OuterTrip = *OuterTrip;
    Shape.InnerTrip = *InnerTrip;
    Shape.OuterInduction = *OuterInduction;
    Shape.InnerInduction = *InnerInduction;
    if (!Outer->getLoopSimplifyForm(Shape.OuterPreHeader,
                                    Shape.OuterLatch) ||
        !Inner->getLoopSimplifyForm(Shape.InnerPreHeader,
                                    Shape.InnerLatch)) {
        return std::nullopt;
    }
    //2.0 常量外轴沿用既有无回绕事实；运行期外轴必须在预头用Range证明，
    //并且总次数表达式能够在同一位置安全物化，不能依赖循环内值或未定义行为。
    const bool ConstantOuter = OuterTrip->ConstantTripCount &&
        *OuterTrip->ConstantTripCount >= 2 && OuterTrip->ProvenNoWrap &&
        SE.isKnownNoWrapInductionValue(Outer, OuterInduction->Phi);
    const bool RuntimeOuter = OuterTrip->IsRuntime &&
        OuterTrip->TripCount &&
        (OuterTrip->ProvenNoWrap ||
         SE.proveRuntimeNoWrapAtPreheader(*OuterTrip, RI, DT)) &&
        LoopUnrollPass::canMaterializeRuntimeSCEV(
            OuterTrip->TripCount, Outer, Shape.OuterPreHeader, DT);
    if (!ConstantOuter && !RuntimeOuter) return std::nullopt;
    Shape.OuterRuntimeNoWrapProven = RuntimeOuter;
    //2.6 将控制、外回边语义、CFG/LCSSA 形状和 lane 闭包串联校验，任一步不完整都放弃整个候选。
    Shape.Blocks = Outer->getBlocksInFunctionOrder();
    if (!collectControl(Shape, SE, RI, DT, C))
        return std::nullopt;
    if (Shape.InnerTrip.IsRuntime && Shape.InnerTrip.TripCount &&
        Shape.InnerRuntimeNoWrapProven) {
        Shape.InnerTripMaterializableAtOuterPreHeader =
            LoopUnrollPass::canMaterializeRuntimeSCEV(
                Shape.InnerTrip.TripCount, Outer,
                Shape.OuterPreHeader, DT);
    }
    if (!analyzeOuterControl(Shape) ||
        !validateShape(Shape, LI, SE, DT, PDT, C) ||
        !collectLaneInstructions(Shape, C)) {
        return std::nullopt;
    }
    return Shape;
}
//解除并销毁提交前计划持有的全部游离指令。
void UnrollAndJamPass::destroyClonePlan(UnrollAndJamClonePlan &Plan) {
    //4.3 失败回滚只处理游离对象：先清空操作数解除 use 链，再销毁每 lane 克隆、lane IV 和新外更新。
    for (auto &Lane : Plan.Lanes) {
        for (auto &Pair : Lane.InstructionPairs)
            if (Pair.second) Pair.second->clearOperands();
        for (auto &Pair : Lane.InstructionPairs)
            delete Pair.second;
        Lane.InstructionPairs.clear();
        if (auto *LaneInstruction =
                dynamic_cast<Instruction *>(Lane.LaneIV)) {
            LaneInstruction->clearOperands();
            delete LaneInstruction;
        }
        Lane.LaneIV = nullptr;
        Lane.ValueMap.clear();
    }
    Plan.Lanes.clear();
    if (Plan.NewOuterUpdate) {
        Plan.NewOuterUpdate->clearOperands();
        delete Plan.NewOuterUpdate;
        Plan.NewOuterUpdate = nullptr;
    }
    Plan.Factor = 1;
}
//按照当前lane的值映射重新绑定全部克隆操作数。
bool UnrollAndJamPass::remapClonedOperands(
    UnrollAndJamLanePlan &Lane) {
    for (const auto &Pair : Lane.InstructionPairs) {
        Instruction *Source = Pair.first;
        Instruction *Clone = Pair.second;
        if (!Source || !Clone ||
            Source->getNumOperands() != Clone->getNumOperands()) {
            return false;
        }
        for (std::size_t Index = 0; Index < Source->getNumOperands(); ++Index) {
            Value *Operand = Source->getOperand(Index);
            auto It = Lane.ValueMap.find(Operand);
            Value *Mapped = It == Lane.ValueMap.end() ? Operand : It->second;
            if (!Mapped || (Clone->getOperand(Index) != Mapped &&
                            !Clone->replaceOperand(Index, Mapped))) {
                return false;
            }
        }
    }
    return true;
}
//4.2 对元素紧邻的 lane 内存访问，将克隆 GEP 规范化为原地址加减 LaneIndex，保持正/反向步长一致。
bool UnrollAndJamPass::canonicalizeLaneMemoryAddresses(
    const UnrollAndJamShape &Shape, LoopAccessInfo &LA,
    UnrollAndJamLanePlan &Lane, int LaneIndex) {
    if (!Shape.Outer || LaneIndex <= 0) return false;
    std::unordered_set<Instruction *> LaneValues(
        Shape.LaneInstructions.begin(), Shape.LaneInstructions.end());
    for (const auto &Pair : Lane.InstructionPairs) {
        auto *SourceGEP = dynamic_cast<GetElementPtrInst *>(Pair.first);
        auto *CloneGEP = dynamic_cast<GetElementPtrInst *>(Pair.second);
        if (!SourceGEP || !CloneGEP) continue;
        int Direction = 0;
        bool Conflict = false;
        for (Use *U : SourceGEP->getUses()) {
            auto *Memory = dynamic_cast<Instruction *>(
                U ? U->getUser() : nullptr);
            if (!Memory || !LaneValues.count(Memory)) continue;
            std::shared_ptr<IRType> ElementType;
            if (auto *Load = dynamic_cast<LoadInst *>(Memory)) {
                if (Load->getPointerOperand() != SourceGEP) continue;
                ElementType = Load->getIRType();
            } else if (auto *Store = dynamic_cast<StoreInst *>(Memory)) {
                if (Store->getPointerOperand() != SourceGEP ||
                    !Store->getValueOperand()) {
                    continue;
                }
                ElementType = Store->getValueOperand()->getIRType();
            } else {
                continue;
            }
            if (!ElementType || !ElementType->isScalar() ||
                ElementType->getSize() == 0 ||
                ElementType->getSize() > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                continue;
            }
            auto Stride = LA.getByteStride(
                Memory, Shape.Outer, Shape.Outer);
            const std::int64_t ElementBytes =
                static_cast<std::int64_t>(ElementType->getSize());
            int ThisDirection = 0;
            if (Stride && *Stride == ElementBytes)
                ThisDirection = 1;
            else if (Stride && *Stride == -ElementBytes)
                ThisDirection = -1;
            else
                continue;
            if (Direction != 0 && Direction != ThisDirection) {
                Conflict = true;
                break;
            }
            Direction = ThisDirection;
        }
        if (Conflict || Direction == 0) continue;
        const __int128 WideIndex =
            static_cast<__int128>(Direction) * LaneIndex;
        if (WideIndex < std::numeric_limits<std::int32_t>::min() ||
            WideIndex > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        std::vector<Value *> Indices = {
            ConstInt::newConstInt(static_cast<int>(WideIndex), 4)};
        if (!CloneGEP->reset(SourceGEP, Indices)) return false;
    }
    return true;
}
//离线构造额外lane的归纳量、指令克隆、地址映射和新外循环更新。
std::optional<UnrollAndJamClonePlan>
UnrollAndJamPass::buildClonePlan(
    const UnrollAndJamShape &Shape, LoopAccessInfo &LA, int Factor,
    const UnrollAndJamConfig &C) {
    if (!Shape.OuterInduction.Phi || !Shape.OuterInduction.ConstantStep ||
        !isSupportedFactor(Factor) || Factor > C.MaxFactor) {
        return std::nullopt;
    }
    const std::int64_t Step = *Shape.OuterInduction.ConstantStep;
    const __int128 WideNewStep =
        static_cast<__int128>(Step) * Factor;
    if (WideNewStep <= 0 ||
        WideNewStep > std::numeric_limits<std::int32_t>::max()) {
        return std::nullopt;
    }
    const std::int64_t NewStep = static_cast<std::int64_t>(WideNewStep);
    auto Ty = Shape.OuterInduction.Phi->getIRType();
    if (!Ty || !Ty->isInt() || Ty->isBool())
        return std::nullopt;
    //4.计划阶段先在宽类型中验证 Factor*Step 和 lane 偏移不溢出，所有新指令在提交前不挂到基本块。
    UnrollAndJamClonePlan Plan;
    Plan.Factor = Factor;
    Plan.Lanes.reserve(static_cast<std::size_t>(Factor - 1));
    for (int LaneIndex = 1; LaneIndex < Factor; ++LaneIndex) {
        const __int128 WideOffset =
            static_cast<__int128>(Step) * LaneIndex;
        if (WideOffset <= 0 ||
            WideOffset > std::numeric_limits<std::int32_t>::max()) {
            destroyClonePlan(Plan);
            return std::nullopt;
        }
        Plan.Lanes.emplace_back();
        UnrollAndJamLanePlan &Lane = Plan.Lanes.back();
        //4.1 用外 IV + LaneIndex*Step 构造 lane IV，偏移常量严格沿用 PHI 的真实整数位宽。
        Value *LaneOffset = Value::makeIntegerForType(
            static_cast<std::int64_t>(WideOffset), Ty);
        if (!LaneOffset) {
            destroyClonePlan(Plan);
            return std::nullopt;
        }
        Lane.LaneIV = new BinaryInst(
            Shape.OuterInduction.Phi, BinaryInst::ADD, LaneOffset);
        Lane.ValueMap[Shape.OuterInduction.Phi] = Lane.LaneIV;
        for (Instruction *Source : Shape.LaneInstructions) {
            //4.1.1 按原函数顺序克隆 lane 闭包，随克隆扩展 ValueMap，再做一轮完整 operand 重映射。
            Instruction *Clone = Source
                ? Source->clone(Lane.ValueMap)
                : nullptr;
            if (!Clone) {
                destroyClonePlan(Plan);
                return std::nullopt;
            }
            Clone->setIdent(Source->getIdent() + ".jam" +
                            std::to_string(LaneIndex));
            Lane.InstructionPairs.emplace_back(Source, Clone);
            Lane.ValueMap[Source] = Clone;
        }
        if (!remapClonedOperands(Lane)) {
            destroyClonePlan(Plan);
            return std::nullopt;
        }
        if (!canonicalizeLaneMemoryAddresses(
                Shape, LA, Lane, LaneIndex)) {
            destroyClonePlan(Plan);
            return std::nullopt;
        }
    }
    Value *NewStepValue = Value::makeIntegerForType(NewStep, Ty);
    if (!NewStepValue) {
        destroyClonePlan(Plan);
        return std::nullopt;
    }
    Plan.NewOuterUpdate = new BinaryInst(
        Shape.OuterInduction.Phi, BinaryInst::ADD, NewStepValue);
    return Plan;
}
//提交前重新验证源指令、挂接点、PHI入边和全部游离克隆。
bool UnrollAndJamPass::validatePlanCommit(
    const UnrollAndJamShape &Shape,
    const UnrollAndJamClonePlan &Plan) {
    //5.提交预检确认所有新指令仍游离、源指令和挂接点仍存活，且外 IV/退出比较仍引用原更新。
    if (!isSupportedFactor(Plan.Factor) ||
        Plan.Lanes.size() != static_cast<std::size_t>(Plan.Factor - 1) ||
        !Plan.NewOuterUpdate ||
        Plan.NewOuterUpdate->getParent() || !Shape.Outer ||
        !Shape.Outer->getHeader() || !Shape.OuterLatch ||
        !Shape.OuterLatch->getTerminator() ||
        !Shape.OuterInduction.Phi ||
        Shape.OuterBackedgeIncomingIndex >=
            Shape.OuterInduction.Phi->getNumIncomingValues() ||
        Shape.OuterInduction.Phi->getIncomingValue(
            Shape.OuterBackedgeIncomingIndex) !=
            Shape.OuterInduction.UpdateValue) {
        return false;
    }
    if (Shape.OuterExitUsesUpdate) {
        if (!Shape.OuterTrip.ExitCmp ||
            Shape.OuterTrip.ExitCmp->getParent() != Shape.OuterLatch ||
            Shape.OuterExitComparedOperand >=
                Shape.OuterTrip.ExitCmp->getNumOperands() ||
            Shape.OuterTrip.ExitCmp->getOperand(
                Shape.OuterExitComparedOperand) !=
                Shape.OuterInduction.UpdateValue) {
            return false;
        }
    }
    for (const auto &Lane : Plan.Lanes) {
        auto *LaneIV = dynamic_cast<Instruction *>(Lane.LaneIV);
        if (!LaneIV || LaneIV->getParent()) return false;
        for (const auto &Pair : Lane.InstructionPairs) {
            if (!Pair.first || !Pair.first->getParent() || !Pair.second ||
                Pair.second->getParent() ||
                !Pair.first->getParent()->getTerminator()) {
                return false;
            }
        }
        if (Lane.InstructionPairs.size() !=
            Shape.LaneInstructions.size()) {
            return false;
        }
    }
    return true;
}
//集中挂接全部lane克隆并把外循环更新步长放大为Factor倍。
bool UnrollAndJamPass::executePlan(UnrollAndJamShape &Shape,
                                   UnrollAndJamClonePlan &Plan) {
    if (!validatePlanCommit(Shape, Plan)) return false;
    //5.1 集中挂接：lane IV 放在外头 PHI 后，克隆 PHI 放在所属块 PHI 区，其余负载放在各自终结指令前。
    for (auto &Lane : Plan.Lanes) {
        auto *LaneIV = dynamic_cast<Instruction *>(Lane.LaneIV);
        Shape.Outer->getHeader()->insertAfterPhiNodes(LaneIV);
        Lane.LaneIV = nullptr;
        for (auto &Pair : Lane.InstructionPairs) {
            if (dynamic_cast<PhiInst *>(Pair.second))
                Pair.first->getParent()->insertAfterPhiNodes(Pair.second);
            else
                Pair.first->getParent()->insertBeforeTerminator(Pair.second);
            Pair.second = nullptr;
        }
        Lane.InstructionPairs.clear();
        Lane.ValueMap.clear();
    }
    //5.2 将新的 Factor*Step 更新插到 latch，同时重定向外 IV 回边和使用 update 的退出比较。
    if (Shape.OuterExitUsesUpdate &&
        Shape.OuterTrip.ExitCmp->getParent() == Shape.OuterLatch) {
        Instruction::insertBefore(Shape.OuterTrip.ExitCmp,
                                  Plan.NewOuterUpdate);
    } else {
        Shape.OuterLatch->insertBeforeTerminator(Plan.NewOuterUpdate);
    }
    Instruction *NewUpdate = Plan.NewOuterUpdate;
    Plan.NewOuterUpdate = nullptr;
    Shape.OuterInduction.Phi->setIncomingValue(
        Shape.OuterBackedgeIncomingIndex, NewUpdate);
    if (Shape.OuterExitUsesUpdate) {
        Shape.OuterTrip.ExitCmp->replaceOperand(
            Shape.OuterExitComparedOperand, NewUpdate);
    }
    Plan.Lanes.clear();
    return true;
}
//为运行期外轴构造jam主段和保持原形的标量尾段。
bool UnrollAndJamPass::executeRuntimePlan(
    Function &F, UnrollAndJamShape &Shape,
    UnrollAndJamClonePlan &Plan, SCEVResult &SE,
    const DominatorTree &DT, std::int64_t MinInnerTrip,
    const UnrollAndJamConfig &C) {
    (void)C;
    if (!Shape.Outer || !Shape.Inner ||
        !Shape.OuterTrip.IsRuntime ||
        !Shape.OuterRuntimeNoWrapProven ||
        !Shape.OuterTrip.TripCount ||
        !Shape.OuterPreHeader || !Shape.OuterLatch ||
        !Shape.OuterTrip.ExitingBlock ||
        !Shape.OuterTrip.ExitBlock ||
        !Shape.OuterTrip.ExitCmp ||
        !Shape.OuterInduction.Phi ||
        !Shape.OuterInduction.StartValue ||
        !Shape.OuterInduction.ConstantStep ||
        *Shape.OuterInduction.ConstantStep <= 0 ||
        MinInnerTrip <= 0 || !validatePlanCommit(Shape, Plan)) {
        return false;
    }
    BasicBlock *Header = Shape.Outer->getHeader();
    BasicBlock *PreHeader = Shape.OuterPreHeader;
    BasicBlock *Exiting = Shape.OuterTrip.ExitingBlock;
    BasicBlock *Exit = Shape.OuterTrip.ExitBlock;
    BinaryInst *ExitCmp = Shape.OuterTrip.ExitCmp;
    auto *PreBranch = PreHeader->getBranchTerminator();
    auto *ExitBranch = Exiting->getBranchTerminator();
    if (!Header || Header->getParent() != &F ||
        !BasicBlock::isUncondBr(PreBranch) ||
        PreBranch->getSuccessor(0) != Header ||
        !BasicBlock::isCondBr(ExitBranch) ||
        ExitBranch->getCondition() != ExitCmp ||
        ExitCmp->getNumOperands() != 2 ||
        Shape.OuterExitComparedOperand > 1 ||
        !IRMath::isPowerOf2(Plan.Factor)) {
        return false;
    }
    unsigned MainExitSuccessor = 2;
    for (unsigned Index = 0;
         Index < ExitBranch->getSuccessorCount(); ++Index) {
        if (ExitBranch->getSuccessor(Index) != Exit) continue;
        if (MainExitSuccessor != 2) return false;
        MainExitSuccessor = Index;
    }
    if (MainExitSuccessor == 2) return false;
    const unsigned BoundOperandIndex =
        1 - Shape.OuterExitComparedOperand;
    Value *OriginalBound = ExitCmp->getOperand(BoundOperandIndex);
    auto IVType = Shape.OuterInduction.Phi->getIRType();
    if (!OriginalBound || !IVType || !IVType->isInt() ||
        IVType->isBool() ||
        !Shape.OuterInduction.StartValue->getIRType() ||
        !IRType::hasSameShape(
            IVType, Shape.OuterInduction.StartValue->getIRType()) ||
        !Shape.OuterTrip.TripCount->Ty ||
        !IRType::hasSameShape(IVType, Shape.OuterTrip.TripCount->Ty)) {
        return false;
    }
    //运行期形态只接受唯一的预头初值边，尾副本才能从MainStart精确续接。
    unsigned StartIncomingIndex =
        Shape.OuterInduction.Phi->getNumIncomingValues();
    for (unsigned Index = 0;
         Index < Shape.OuterInduction.Phi->getNumIncomingValues();
         ++Index) {
        BasicBlock *Pred =
            Shape.OuterInduction.Phi->getIncomingBlock(Index);
        if (Pred != PreHeader) continue;
        if (StartIncomingIndex !=
            Shape.OuterInduction.Phi->getNumIncomingValues() ||
            Shape.OuterInduction.Phi->getIncomingValue(Index) !=
                Shape.OuterInduction.StartValue) {
            return false;
        }
        StartIncomingIndex = Index;
    }
    if (StartIncomingIndex ==
        Shape.OuterInduction.Phi->getNumIncomingValues()) {
        return false;
    }
    //出口PHI只允许携带在外预头已经可用的值；循环内活跃值已由形状门禁拒绝。
    std::vector<std::pair<PhiInst *, Value *>> ExitPhiInputs;
    for (PhiInst *Phi : Exit->collectPhiNodes()) {
        if (!Phi || Phi->countIncomingFromPred(Exiting) != 1)
            return false;
        Value *Incoming = Phi->getIncomingFromPred(Exiting);
        if (!Incoming || !Incoming->getIRType() || !Phi->getIRType() ||
            !IRType::hasSameShape(
                Incoming->getIRType(), Phi->getIRType())) {
            return false;
        }
        bool Available = Incoming->isConst() || Incoming->isParam() ||
                         Incoming->isGlobal();
        if (auto *I = dynamic_cast<Instruction *>(Incoming)) {
            Available = I->getParent() &&
                !Shape.Outer->contains(I->getParent()) &&
                (I->getParent() == PreHeader ||
                 DT.dominates(I->getParent(), PreHeader));
        }
        if (!Available) return false;
        ExitPhiInputs.emplace_back(Phi, Incoming);
    }
    if (!LoopUnrollPass::canMaterializeRuntimeSCEV(
            Shape.OuterTrip.TripCount, Shape.Outer, PreHeader, DT)) {
        return false;
    }
    const bool DynamicInner = !Shape.InnerTrip.ConstantTripCount;
    if (DynamicInner &&
        (!Shape.InnerTrip.IsRuntime ||
         !Shape.InnerRuntimeNoWrapProven ||
         !Shape.InnerTripMaterializableAtOuterPreHeader ||
         !Shape.InnerTrip.TripCount ||
         !Shape.InnerTrip.TripCount->Ty ||
         !Shape.InnerTrip.TripCount->Ty->isInt() ||
         Shape.InnerTrip.TripCount->Ty->isBool())) {
        return false;
    }
    std::unordered_set<Instruction *> OriginalPreHeaderInstructions;
    for (Instruction *I : *PreHeader)
        if (I) OriginalPreHeaderInstructions.insert(I);
    //先克隆未经jam改写的完整外循环区域，确保尾段保留原步长、原边界和原内循环。
    CloneBlockRegionOptions CloneOptions;
    CloneOptions.MakeBlockIdent = [&F](
        const BasicBlock *Source, std::size_t) {
        const std::string Base = Source
            ? Source->getIdent() + ".jam.runtime.tail"
            : "jam.runtime.tail";
        return F.makeUniqueBlockIdent(Base);
    };
    auto TailRegion = cloneBasicBlockRegion(
        F, Shape.Blocks, {}, CloneOptions);
    if (!TailRegion) return false;
    auto DiscardTail = [&]() {
        for (BasicBlock *BB : TailRegion->ClonedBlocks) {
            if (!BB) continue;
            for (Instruction *I : *BB)
                if (I) I->clearOperands();
        }
        for (BasicBlock *BB : TailRegion->ClonedBlocks)
            if (BB) BB->clear();
        for (BasicBlock *BB : TailRegion->ClonedBlocks)
            delete BB;
        TailRegion->ClonedBlocks.clear();
    };
    auto DiscardRuntimeSetup = [&]() {
        DiscardTail();
        std::vector<Instruction *> Added;
        for (Instruction *I : *PreHeader) {
            if (I && !OriginalPreHeaderInstructions.count(I))
                Added.push_back(I);
        }
        for (auto It = Added.rbegin(); It != Added.rend(); ++It)
            (*It)->clearOperands();
        for (auto It = Added.rbegin(); It != Added.rend(); ++It) {
            (*It)->eraseFromParent();
            delete *It;
        }
    };
    BasicBlock *TailHeader = TailRegion->getClonedBlock(Header);
    BasicBlock *TailExiting = TailRegion->getClonedBlock(Exiting);
    auto *TailIV = dynamic_cast<PhiInst *>(
        TailRegion->getClonedInstruction(Shape.OuterInduction.Phi));
    auto *TailExitCmp = dynamic_cast<BinaryInst *>(
        TailRegion->getClonedInstruction(ExitCmp));
    auto *TailExitBranch = TailExiting
        ? TailExiting->getBranchTerminator() : nullptr;
    if (!TailHeader || !TailExiting || !TailIV || !TailExitCmp ||
        !BasicBlock::isCondBr(TailExitBranch) ||
        TailExitBranch->getCondition() != TailExitCmp ||
        TailExitCmp->getNumOperands() != 2 ||
        TailExitCmp->getOperand(BoundOperandIndex) != OriginalBound) {
        DiscardTail();
        return false;
    }
    unsigned TailExitSuccessor = 2;
    for (unsigned Index = 0;
         Index < TailExitBranch->getSuccessorCount(); ++Index) {
        if (TailExitBranch->getSuccessor(Index) != Exit) continue;
        if (TailExitSuccessor != 2) {
            DiscardTail();
            return false;
        }
        TailExitSuccessor = Index;
    }
    unsigned TailStartIncomingIndex = TailIV->getNumIncomingValues();
    for (unsigned Index = 0; Index < TailIV->getNumIncomingValues();
         ++Index) {
        if (TailIV->getIncomingBlock(Index) != PreHeader) continue;
        if (TailStartIncomingIndex != TailIV->getNumIncomingValues()) {
            DiscardTail();
            return false;
        }
        TailStartIncomingIndex = Index;
    }
    if (TailExitSuccessor == 2 ||
        TailStartIncomingIndex == TailIV->getNumIncomingValues()) {
        DiscardTail();
        return false;
    }
    //物化T并计算M=T-(T%Factor)；二次幂因子使用AND，避免有符号余数的额外语义风险。
    Value *TripCount = LoopUnrollPass::materializeRuntimeSCEV(
        Shape.OuterTrip.TripCount, Shape.Outer, PreHeader, DT);
    if (!TripCount || !TripCount->getIRType() ||
        !IRType::hasSameShape(TripCount->getIRType(), IVType)) {
        DiscardRuntimeSetup();
        return false;
    }
    Value *Mask = Value::makeIntegerForType(
        Plan.Factor - 1, TripCount->getIRType());
    Value *ZeroTrip = Value::makeIntegerForType(
        0, TripCount->getIRType());
    if (!Mask || !ZeroTrip) {
        DiscardRuntimeSetup();
        return false;
    }
    auto *Remainder = new BinaryInst(
        TripCount, BinaryInst::AND, Mask);
    BasicBlock::insertBeforeTerminator(PreHeader, Remainder);
    auto *MainTrip = new BinaryInst(
        TripCount, BinaryInst::SUB, Remainder);
    BasicBlock::insertBeforeTerminator(PreHeader, MainTrip);
    Value *EffectiveMainTrip = MainTrip;
    if (DynamicInner) {
        Value *InnerTrip = LoopUnrollPass::materializeRuntimeSCEV(
            Shape.InnerTrip.TripCount, Shape.Outer, PreHeader, DT);
        if (!InnerTrip || !InnerTrip->getIRType() ||
            !InnerTrip->getIRType()->isInt() ||
            InnerTrip->getIRType()->isBool()) {
            DiscardRuntimeSetup();
            return false;
        }
        Value *Threshold = Value::makeIntegerForType(
            MinInnerTrip, InnerTrip->getIRType());
        if (!Threshold) {
            DiscardRuntimeSetup();
            return false;
        }
        auto *EnoughInnerWork = new BinaryInst(
            InnerTrip, BinaryInst::GE, Threshold);
        BasicBlock::insertBeforeTerminator(
            PreHeader, EnoughInnerWork);
        auto *SelectedMainTrip = new SelectInst(
            EnoughInnerWork, MainTrip, ZeroTrip);
        BasicBlock::insertBeforeTerminator(
            PreHeader, SelectedMainTrip);
        EffectiveMainTrip = SelectedMainTrip;
    }
    Value *MainOffset = EffectiveMainTrip;
    if (*Shape.OuterInduction.ConstantStep != 1) {
        Value *Step = Value::makeIntegerForType(
            *Shape.OuterInduction.ConstantStep,
            EffectiveMainTrip->getIRType());
        if (!Step) {
            DiscardRuntimeSetup();
            return false;
        }
        auto *ScaledMainTrip = new BinaryInst(
            EffectiveMainTrip, BinaryInst::MUL, Step);
        BasicBlock::insertBeforeTerminator(
            PreHeader, ScaledMainTrip);
        MainOffset = ScaledMainTrip;
    }
    auto *MainStart = new BinaryInst(
        Shape.OuterInduction.StartValue, BinaryInst::ADD, MainOffset);
    BasicBlock::insertBeforeTerminator(PreHeader, MainStart);
    auto *TailTrip = new BinaryInst(
        TripCount, BinaryInst::SUB, EffectiveMainTrip);
    BasicBlock::insertBeforeTerminator(PreHeader, TailTrip);
    auto *HasMain = new BinaryInst(
        EffectiveMainTrip, BinaryInst::NE, ZeroTrip);
    BasicBlock::insertBeforeTerminator(PreHeader, HasMain);
    //TailDispatch同时接收“无主段”和“主段完成”两条路径；TailTrip为T时
    //执行完整标量循环，为T%Factor时仅执行余数，零时直接到原出口。
    auto *TailDispatch = new BasicBlock();
    TailDispatch->setIdent(F.makeUniqueBlockIdent(
        Header->getIdent() + ".jam.runtime.dispatch"));
    auto *HasTail = new BinaryInst(
        TailTrip, BinaryInst::NE, ZeroTrip);
    TailDispatch->pushBack(HasTail);
    TailDispatch->pushBack(
        new BrInst(HasTail, TailHeader, Exit));
    //先提交只改块内SSA的lane计划；它已在克隆前通过完整预检，失败时原CFG
    //仍未改变，并可删除尾副本与全部预头物化指令。
    if (!executePlan(Shape, Plan)) {
        for (Instruction *I : *TailDispatch)
            if (I) I->clearOperands();
        TailDispatch->clear();
        delete TailDispatch;
        DiscardRuntimeSetup();
        return false;
    }
    F.pushBlock(TailDispatch);
    //以下提交点之前，原循环CFG仍未改变；其后操作全部已由上面的索引、
    //类型、前驱和分支形态预检覆盖，不再保留可失败分支。
    TailIV->setIncomingBlock(TailStartIncomingIndex, TailDispatch);
    TailIV->setIncomingValue(TailStartIncomingIndex, MainStart);
    ExitCmp->replaceOperand(BoundOperandIndex, MainStart);
    ExitBranch->setSuccessor(MainExitSuccessor, TailDispatch);
    for (const auto &Entry : ExitPhiInputs) {
        PhiInst *Phi = Entry.first;
        Value *Incoming = Entry.second;
        for (unsigned Index = 0; Index < Phi->getNumIncomingValues();
             ++Index) {
            if (Phi->getIncomingBlock(Index) == Exiting) {
                Phi->setIncomingBlock(Index, TailDispatch);
                break;
            }
        }
        Phi->addIncoming(Incoming, TailExiting);
    }
    BasicBlock::replaceTerminator(
        PreHeader, new BrInst(HasMain, Header, TailDispatch));
    SE.clear();
    return true;
}
//后序枚举相邻循环，处理余数后提交一次合法且有收益的展开并合并。
PassResult UnrollAndJamPass::run(Function &F,
                                 FunctionAnalysisPassManager &FAM) {
    //1.先获取二维分块与jam共同需要的循环、标量、访存和支配分析。
    unsigned &Count = TransformCounts[&F];
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LA = FAM.getResult<LoopAccessAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominantAnalysis>(F);
    const auto &TargetCost = FAM.getResult<TargetCostAnalysis>(F);
    //非RISC-V完整保留接入TTI前的Config行为；RV64GC才从
    //目标模型生成本轮有效配置。
    UnrollAndJamConfig EffectiveConfig = Config;
    if (TargetCost.isRISCV64()) {
        const unsigned TargetFPRBudget =
            TargetCost.getActiveRegisterBudget(
                TargetRegisterClass::FloatingPoint);
        const unsigned TargetGPRBudget =
            TargetCost.getActiveRegisterBudget(
                TargetRegisterClass::GeneralPurpose);
        //零表示直接使用目标预算；显式值是用户上限，
        //不得放宽RV64GC的保守活跃寄存器预算。
        EffectiveConfig.FPRBudget = EffectiveConfig.FPRBudget == 0
            ? TargetFPRBudget
            : std::min(EffectiveConfig.FPRBudget, TargetFPRBudget);
        EffectiveConfig.GPRBudget = EffectiveConfig.GPRBudget == 0
            ? TargetGPRBudget
            : std::min(EffectiveConfig.GPRBudget, TargetGPRBudget);
        if (EffectiveConfig.CacheLineBytes == 0 ||
            EffectiveConfig.CacheLineBytes == 64)
            EffectiveConfig.CacheLineBytes = TargetCost.getCacheLineSize();
        if (EffectiveConfig.EstimatedFPAddLatency == 0 ||
            EffectiveConfig.EstimatedFPAddLatency == 3) {
            const int Cost = TargetCost.getArithmeticCost(
                Instruction::FADD, FLOATType::NewFloat().get(),
                TargetCostKind::Throughput);
            if (Cost > 0 && Cost < TargetCostInfo::InvalidCost)
                EffectiveConfig.EstimatedFPAddLatency =
                    static_cast<unsigned>(Cost);
        }
        if (EffectiveConfig.EstimatedSharedLoadCost == 0 ||
            EffectiveConfig.EstimatedSharedLoadCost == 2) {
            const int Cost = TargetCost.getMemoryCost(
                TargetMemoryOperation::Load,
                FLOATType::NewFloat().get(), 4, false,
                TargetCostKind::Throughput);
            if (Cost > 0 && Cost < TargetCostInfo::InvalidCost)
                EffectiveConfig.EstimatedSharedLoadCost =
                    static_cast<unsigned>(Cost);
        }
        if (EffectiveConfig.EstimatedInnerControlCost == 0 ||
            EffectiveConfig.EstimatedInnerControlCost == 1) {
            const int Cost = TargetCost.getBranchCost(
                true, TargetCostKind::Throughput);
            if (Cost > 0 && Cost < TargetCostInfo::InvalidCost)
                EffectiveConfig.EstimatedInnerControlCost =
                    static_cast<unsigned>(Cost);
        }
        if (EffectiveConfig.EstimatedRuntimeDispatchCost == 0 ||
            EffectiveConfig.EstimatedRuntimeDispatchCost == 8) {
            const int BranchCost = TargetCost.getBranchCost(
                true, TargetCostKind::Throughput);
            const int ArithmeticCost = TargetCost.getArithmeticCost(
                Instruction::ADD, INTType::getInt32Ty().get(),
                TargetCostKind::Throughput);
            if (BranchCost > 0 && ArithmeticCost > 0 &&
                BranchCost < TargetCostInfo::InvalidCost &&
                ArithmeticCost < TargetCostInfo::InvalidCost) {
                EffectiveConfig.EstimatedRuntimeDispatchCost =
                    static_cast<unsigned>(
                        4 * (BranchCost + ArithmeticCost));
            }
        }
    }
    //1.1 分块优先于lane复制执行；成功后CFG改变，固定点下一轮会重建四层循环并继续评估jam。
    unsigned &TileCount = TileTransformCounts[&F];
    auto &AlreadyTiled = TiledHeaders[&F];
    if (TileCount < EffectiveConfig.MaxTileTransformsPerFunction) {
        for (Loop *Inner : LI.getLoopsInPostOrder()) {
            Loop *Outer = Inner ? Inner->getParentLoop() : nullptr;
            if (!Outer || !Outer->getHeader() || !Inner->getHeader() ||
                AlreadyTiled.count(Outer->getHeader()) ||
                AlreadyTiled.count(Inner->getHeader())) {
                continue;
            }
            LoopTilingShape TileShape;
            if (!buildTilingShape(
                    Outer, Inner, LA, SE, DT, PDT, TileShape)) {
                continue;
            }
            auto Choice = selectTile(TileShape, LA, EffectiveConfig);
            if (!Choice) continue;
            std::vector<BasicBlock *> NewHeaders;
            if (!applyTiling(F, TileShape, *Choice, NewHeaders))
                continue;
            for (BasicBlock *Header : NewHeaders)
                if (Header) AlreadyTiled.insert(Header);
            ++TileCount;
            return PassResult::changed(PreservationStatus::none());
        }
    }
    //1.2 jam函数级成功次数达到上限后停止，避免固定点持续复制lane。
    if (Count >= EffectiveConfig.MaxTransformsPerFunction)
        return PassResult::unchanged();
    auto &Headers = TransformedHeaders[&F];
    for (Loop *Inner : LI.getLoopsInPostOrder()) {
        //1.1 从内向外枚举直接父子循环，跳过已记录的外头；每次调用最多成功提交一个候选。
        Loop *Outer = Inner ? Inner->getParentLoop() : nullptr;
        if (!Outer || !Outer->getHeader() ||
            Headers.count(Outer->getHeader())) {
            continue;
        }
        auto Shape = buildShape(
            Outer, Inner, LI, SE, LA, RI, DT, PDT, EffectiveConfig);
        if (!Shape) continue;
        //3.形状合法后才评估共享读取、内存局部性、严格归约延迟与寄存器压力，得到唯一 Factor。
        std::int64_t RuntimeMinInnerTrip = 0;
        auto Factor = selectFactor(
            *Shape, LA, SE, EffectiveConfig, &RuntimeMinInnerTrip);
        if (!Factor) continue;
        if (Shape->OuterTrip.IsRuntime) {
            auto Plan = buildClonePlan(
                *Shape, LA, *Factor, EffectiveConfig);
            if (!Plan) continue;
            if (!executeRuntimePlan(
                    F, *Shape, *Plan, SE, DT,
                    RuntimeMinInnerTrip, EffectiveConfig)) {
                destroyClonePlan(*Plan);
                continue;
            }
            Headers.insert(Outer->getHeader());
            ++Count;
            //运行期jam增加标量尾副本和分派块，全部CFG、循环与支配分析失效。
            return PassResult::changed(PreservationStatus::none());
        }
        if (!Shape->OuterTrip.ConstantTripCount) continue;
        const std::int64_t Remainder =
            *Shape->OuterTrip.ConstantTripCount % *Factor;
        if (Remainder != 0) {
            //6.非整除 trip count 不直接 jam：余数在预算内时复用 LoopUnroll peeling 剥掉尾数，CFG 变更后立即返回并重算全部分析。
            if (Remainder < 0 ||
                Remainder > static_cast<std::int64_t>(
                                EffectiveConfig.MaxRemainderPeel)) {
                continue;
            }
            auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
            UnrollConfig PeelConfig;
            PeelConfig.AllowFull = false;
            PeelConfig.AllowPartial = false;
            PeelConfig.AllowRuntime = false;
            PeelConfig.AllowRemainder = false;
            PeelConfig.AllowPeeling = true;
            PeelConfig.MaxPeelCount =
                static_cast<int>(EffectiveConfig.MaxRemainderPeel);
            LoopUnrollPass Peeler(PeelConfig);
            UnrollDecision Decision;
            Decision.Kind = UnrollKind::Peeling;
            Decision.Count = static_cast<int>(Remainder);
            Decision.ShouldUnroll = true;
            Decision.IsPeeling = true;
            Decision.TripCountIsProven = true;
            const LoopUnrollResult Result = Peeler.executePeeling(
                Shape->Outer, Decision, SE, LI, DT, AA);
            if (Result != LoopUnrollResult::Unmodified) {
                return PassResult::changed(PreservationStatus::none());
            }
            continue;
        }
        auto Plan = buildClonePlan(
            *Shape, LA, *Factor, EffectiveConfig);
        if (!Plan) continue;
        if (!executePlan(*Shape, *Plan)) {
            //4.3 执行前预检失败时计划仍未挂接，可完整销毁并继续尝试下一嵌套。
            destroyClonePlan(*Plan);
            continue;
        }
        Headers.insert(Outer->getHeader());
        ++Count;
        PreservationStatus PA;
        //6.1 jam 只在现有块内插入指令并替换 SSA 更新，不改 CFG，因此保留支配、后支配和循环结构。
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        return PassResult::changed(std::move(PA));
    }
    return PassResult::unchanged();
}
