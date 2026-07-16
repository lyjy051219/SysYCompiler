#include "ir/opt/Tiling.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/CFG.hpp"

#include <limits>
#include <utility>

LoopTilingPass::LoopTilingPass(TilingConfig C) : Config(std::move(C)) {}

bool LoopTilingPass::getNormalizedBound(TilingAxis &Axis) {
    BinaryInst *Cmp = Axis.Trip.ExitCmp;
    if (!Axis.IV || !Cmp) return false;
    BinaryInst::Operation Predicate = Axis.Trip.ContinueOnTrue
        ? Cmp->getOp()
        : BinaryInst::negatePredicate(Cmp->getOp());
    Value *LHS = Cmp->getOperand(0);
    Value *RHS = Cmp->getOperand(1);
    Value *Compared = nullptr;
    if (Axis.Trip.ComparePhase == LoopExitComparePhase::HeaderPhi)
        Compared = Axis.IV;
    else if (Axis.Trip.ComparePhase ==
             LoopExitComparePhase::BackedgeUpdate)
        Compared = Axis.Induction.UpdateValue;
    if (!Compared) return false;
    if (RHS == Compared) {
        std::swap(LHS, RHS);
        Predicate = BinaryInst::reversePredicate(Predicate);
        Axis.BoundOperandIndex = 0;
    } else {
        Axis.BoundOperandIndex = 1;
    }
    if (LHS != Compared || Predicate != BinaryInst::L || !RHS)
        return false;
    Axis.Bound = RHS;
    return true;
}

bool LoopTilingPass::getOutsideIncoming(TilingAxis &Axis,
                                        BasicBlock *PreHeader) {
    if (!Axis.L || !Axis.IV || !PreHeader) return false;
    bool Found = false;
    for (unsigned Index = 0; Index < Axis.IV->getNumIncomingValues(); ++Index) {
        BasicBlock *Pred = Axis.IV->getIncomingBlock(Index);
        Value *Incoming = Axis.IV->getIncomingValue(Index);
        if (!Pred || !Incoming) return false;
        if (Axis.L->contains(Pred)) continue;
        if (Found || Pred != PreHeader) return false;
        Found = true;
        Axis.OutsideIncomingIndex = Index;
        Axis.Start = Incoming;
    }
    return Found;
}

bool LoopTilingPass::hasOnlyCanonicalHeaderPhi(const TilingAxis &Axis) {
    if (!Axis.L || !Axis.L->getHeader() || !Axis.IV) return false;
    const auto Phis = Axis.L->getHeader()->collectPhiNodes();
    return Phis.size() == 1 && Phis.front() == Axis.IV;
}

bool LoopTilingPass::hasControlOnlyHeader(const TilingAxis &Axis) {
    if (!Axis.L || !Axis.L->getHeader() || !Axis.IV ||
        !Axis.Trip.ExitCmp ||
        Axis.Trip.ExitCmp->getParent() != Axis.L->getHeader()) {
        return false;
    }
    auto *Branch = Axis.L->getHeader()->getBranchTerminator();
    if (!BasicBlock::isCondBr(Branch) ||
        Branch->getCondition() != Axis.Trip.ExitCmp) {
        return false;
    }
    for (Instruction *I : *Axis.L->getHeader()) {
        if (I != Axis.IV && I != Axis.Trip.ExitCmp && I != Branch)
            return false;
    }
    return true;
}

bool LoopTilingPass::hasLoopLiveOut(Loop *L) {
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

bool LoopTilingPass::hasDirectObservableOuterWork(
    Loop *Outer, const LoopInfo &LI) {
    if (!Outer) return true;
    for (BasicBlock *BB : Outer->getBlocks()) {
        if (!BB || LI.getLoopFor(BB) != Outer) continue;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<CallInst *>(I) ||
                dynamic_cast<LoadInst *>(I) ||
                dynamic_cast<StoreInst *>(I) ||
                dynamic_cast<AllocaInst *>(I) ||
                dynamic_cast<RetInst *>(I)) {
                return true;
            }
        }
    }
    return false;
}

bool LoopTilingPass::isValueAvailableAt(Value *V, BasicBlock *BB,
                                        const DominatorTree &DT) {
    if (!V || !BB || Value::isUndef(V)) return false;
    if (V->isConst()) return true;
    if (auto *Var = dynamic_cast<Variable *>(V))
        return Var->isParam() || Var->isGlobal();
    auto *I = dynamic_cast<Instruction *>(V);
    return I && I->getParent() && DT.dominates(I->getParent(), BB);
}

bool LoopTilingPass::descendantDomainsAreRectangular(
    Loop *Current, Loop *TiledOuter, Loop *TiledInner, SCEVResult &SE) {
    if (!Current || !TiledOuter || !TiledInner) return false;
    for (Loop *SubLoop : Current->getSubLoops()) {
        auto Trip = SE.analyzeLoopTripCount(SubLoop);
        if (!Trip || !Trip->Valid || !Trip->Start || !Trip->Step ||
            !Trip->Bound || !SE.isKnownFiniteLoop(SubLoop) ||
            Trip->Start->mentionsLoop(TiledOuter) ||
            Trip->Start->mentionsLoop(TiledInner) ||
            Trip->Step->mentionsLoop(TiledOuter) ||
            Trip->Step->mentionsLoop(TiledInner) ||
            Trip->Bound->mentionsLoop(TiledOuter) ||
            Trip->Bound->mentionsLoop(TiledInner) ||
            !descendantDomainsAreRectangular(SubLoop, TiledOuter,
                                             TiledInner, SE)) {
            return false;
        }
    }
    return true;
}

bool LoopTilingPass::hasSingleExitEdge(
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

bool LoopTilingPass::hasCompleteIterationPath(
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

bool LoopTilingPass::validateNestShape(
    const TilingCandidate &Candidate, const LoopInfo &LI,
    const DominatorTree &DT, const PostDominatorTree &PDT,
    const TilingConfig &C) {
    Loop *Outer = Candidate.Outer.L;
    Loop *Inner = Candidate.Inner.L;
    if (!Outer || !Inner || Inner->getParentLoop() != Outer ||
        Outer->getSubLoops().size() != 1 ||
        Outer->getSubLoops().front() != Inner ||
        !Candidate.OuterPreHeader || !Candidate.OuterLatch ||
        !Candidate.InnerPreHeader || !Candidate.InnerLatch ||
        !Candidate.OuterExit || !LCSSAPass::isLCSSAForm(Outer) ||
        !LCSSAPass::isLCSSAForm(Inner) || hasLoopLiveOut(Outer) ||
        hasDirectObservableOuterWork(Outer, LI) ||
        !Candidate.OuterExit->collectPhiNodes().empty()) {
        return false;
    }
    if (!hasSingleExitEdge(Outer, Candidate.Outer.Trip) ||
        !hasSingleExitEdge(Inner, Candidate.Inner.Trip) ||
        !hasCompleteIterationPath(Outer, Candidate.Outer.Trip,
                                  Candidate.OuterLatch, PDT) ||
        !hasCompleteIterationPath(Inner, Candidate.Inner.Trip,
                                  Candidate.InnerLatch, PDT)) {
        return false;
    }
    if (!DT.dominates(Outer->getHeader(), Inner->getHeader()) ||
        !DT.dominates(Inner->getHeader(), Candidate.OuterLatch) ||
        !PDT.isReachableToExit(Outer->getHeader()) ||
        !PDT.isReachableToExit(Inner->getHeader())) {
        return false;
    }
    if (Candidate.NestBlocks.empty() ||
        Candidate.NestBlocks.size() > C.MaxNestBlocks) {
        return false;
    }
    std::size_t InstructionCount = 0;
    for (BasicBlock *BB : Candidate.NestBlocks) {
        if (!BB) return false;
        InstructionCount += static_cast<std::size_t>(BB->size());
        if (InstructionCount > C.MaxNestInstructions) return false;
        for (Instruction *I : *BB)
            if (!I || dynamic_cast<CallInst *>(I) ||
                (!dynamic_cast<LoadInst *>(I) &&
                 !dynamic_cast<StoreInst *>(I) &&
                 instructionMayFaultOrProducePoison(I))) {
                return false;
            }
    }
    auto *PreBr = Candidate.OuterPreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(PreBr) ||
        PreBr->getTrueSuccessor() != Outer->getHeader() ||
        Candidate.OuterPreHeader->countEdgesToSucc(Outer->getHeader()) != 1 ||
        Candidate.Outer.Trip.ExitingBlock->countEdgesToSucc(
            Candidate.OuterExit) != 1) {
        return false;
    }
    return true;
}

std::optional<TilingAxis> LoopTilingPass::analyzeAxis(
    Loop *L, BasicBlock *PreHeader, SCEVResult &SE,
    const TilingConfig &C) {
    if (!L || !PreHeader || C.TileSize <= 1 || !SE.isKnownFiniteLoop(L))
        return std::nullopt;
    auto Trip = SE.analyzeLoopTripCount(L);
    auto Induction = SE.getCanonicalInduction(L);
    if (!Trip || !Induction || !Trip->Valid || !Induction->Valid ||
        !Induction->Phi || !Induction->ConstantStep ||
        *Induction->ConstantStep <= 0 ||
        Trip->ComparePhase != LoopExitComparePhase::HeaderPhi ||
        Trip->ExitingBlock != Trip->Header ||
        Trip->Header == Trip->Latch) {
        return std::nullopt;
    }
    const __int128 Span = static_cast<__int128>(*Induction->ConstantStep) *
                          static_cast<__int128>(C.TileSize);
    if (Span <= 0 || Span > std::numeric_limits<std::int32_t>::max())
        return std::nullopt;

    TilingAxis Axis;
    Axis.L = L;
    Axis.Trip = *Trip;
    Axis.Induction = *Induction;
    Axis.IV = Induction->Phi;
    Axis.Step = *Induction->ConstantStep;
    Axis.TileSpan = static_cast<std::int64_t>(Span);
    if (!getOutsideIncoming(Axis, PreHeader) ||
        !getNormalizedBound(Axis) || !hasOnlyCanonicalHeaderPhi(Axis) ||
        !hasControlOnlyHeader(Axis)) {
        return std::nullopt;
    }
    auto Ty = Axis.IV->getIRType();
    if (!Ty || !Ty->isInt() || Ty->getIntegerBitWidth(32) != 32 ||
        !Axis.Bound->getIRType() ||
        !Ty->hasSameStorageType(*Axis.Bound->getIRType())) {
        return std::nullopt;
    }
    return Axis;
}

std::optional<TilingCandidate> LoopTilingPass::buildCandidate(
    Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
    LoopAccessInfo &LA, DominatorTree &DT, PostDominatorTree &PDT,
    const TilingConfig &C) {
    // This implementation tiles exactly the two selected axes.  If Inner has
    // a descendant reduction, changing only II/JJ leaves that reduction's
    // working set untouched and adds four control loops around every point.
    // Such nests need genuine three-level tiling/interchange, not this pass.
    if (!Inner || !Inner->isInnermost() ||
        !LA.isRectangularNest(Outer, Inner) ||
        !LA.areIterationsIndependent(Outer, Outer) ||
        !LA.areIterationsIndependent(Outer, Inner) ||
        !LA.hasCrossAxisReuse(Outer, Outer, Inner) ||
        !LA.hasCrossAxisReuse(Outer, Inner, Outer) ||
        !descendantDomainsAreRectangular(Inner, Outer, Inner, SE)) {
        return std::nullopt;
    }

    TilingCandidate Candidate;
    if (!Outer->getLoopSimplifyForm(Candidate.OuterPreHeader,
                                    Candidate.OuterLatch) ||
        !Inner->getLoopSimplifyForm(Candidate.InnerPreHeader,
                                    Candidate.InnerLatch)) {
        return std::nullopt;
    }
    auto OuterAxis = analyzeAxis(Outer, Candidate.OuterPreHeader, SE, C);
    auto InnerAxis = analyzeAxis(Inner, Candidate.InnerPreHeader, SE, C);
    if (!OuterAxis || !InnerAxis) return std::nullopt;
    const __int128 MinimumTripCount =
        static_cast<__int128>(C.TileSize) * C.MinTilesPerAxis;
    if (C.MinTilesPerAxis < 2 ||
        MinimumTripCount > std::numeric_limits<std::int64_t>::max() ||
        !OuterAxis->Trip.ConstantTripCount ||
        !InnerAxis->Trip.ConstantTripCount ||
        *OuterAxis->Trip.ConstantTripCount < MinimumTripCount ||
        *InnerAxis->Trip.ConstantTripCount < MinimumTripCount) {
        return std::nullopt;
    }
    Candidate.Outer = *OuterAxis;
    Candidate.Inner = *InnerAxis;
    Candidate.OuterExit = Candidate.Outer.Trip.ExitBlock;
    Candidate.NestBlocks = Outer->getBlocksInFunctionOrder();

    if (!isValueAvailableAt(Candidate.Outer.Start,
                            Candidate.OuterPreHeader, DT) ||
        !isValueAvailableAt(Candidate.Outer.Bound,
                            Candidate.OuterPreHeader, DT) ||
        !isValueAvailableAt(Candidate.Inner.Start,
                            Candidate.OuterPreHeader, DT) ||
        !isValueAvailableAt(Candidate.Inner.Bound,
                            Candidate.OuterPreHeader, DT) ||
        !validateNestShape(Candidate, LI, DT, PDT, C)) {
        return std::nullopt;
    }
    return Candidate;
}

Value *LoopTilingPass::makeTileEnd(BasicBlock *BB, Value *Tile,
                                   Value *Bound, std::int64_t Span) {
    if (!BB || !Tile || !Bound || Span <= 0 ||
        Span > std::numeric_limits<int>::max()) {
        return nullptr;
    }
    auto Ty = Tile->getIRType();
    const std::size_t Size = Ty ? Ty->getSize() : 4;
    Value *SpanValue = ConstInt::newConstInt(static_cast<int>(Span), Size);
    auto *Candidate = new BinaryInst(Tile, BinaryInst::ADD, SpanValue);
    auto *Wrapped = new BinaryInst(Candidate, BinaryInst::L, Tile);
    auto *PastBound = new BinaryInst(Candidate, BinaryInst::G, Bound);
    auto *Clamp = new BinaryInst(Wrapped, BinaryInst::OR, PastBound);
    auto *End = new SelectInst(Clamp, Bound, Candidate);
    BB->pushBack(Candidate);
    BB->pushBack(Wrapped);
    BB->pushBack(PastBound);
    BB->pushBack(Clamp);
    BB->pushBack(End);
    return End;
}

bool LoopTilingPass::validateCommitState(
    const TilingCandidate &Candidate) {
    if (!Candidate.Outer.L || !Candidate.Inner.L ||
        !Candidate.Outer.L->getHeader() ||
        !Candidate.Inner.L->getHeader() ||
        !Candidate.OuterPreHeader || !Candidate.Outer.Trip.ExitingBlock ||
        !Candidate.OuterExit || !Candidate.Outer.IV ||
        !Candidate.Inner.IV || !Candidate.Outer.Trip.ExitCmp ||
        !Candidate.Inner.Trip.ExitCmp ||
        Candidate.Outer.OutsideIncomingIndex >=
            Candidate.Outer.IV->getNumIncomingValues() ||
        Candidate.Inner.OutsideIncomingIndex >=
            Candidate.Inner.IV->getNumIncomingValues() ||
        Candidate.Outer.BoundOperandIndex >=
            Candidate.Outer.Trip.ExitCmp->getNumOperands() ||
        Candidate.Inner.BoundOperandIndex >=
            Candidate.Inner.Trip.ExitCmp->getNumOperands()) {
        return false;
    }
    auto *PreBranch = Candidate.OuterPreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(PreBranch) ||
        PreBranch->getTrueSuccessor() != Candidate.Outer.L->getHeader() ||
        Candidate.OuterPreHeader->countEdgesToSucc(
            Candidate.Outer.L->getHeader()) != 1 ||
        Candidate.Outer.Trip.ExitingBlock->countEdgesToSucc(
            Candidate.OuterExit) != 1) {
        return false;
    }
    return Candidate.Outer.IV->getIncomingValue(
               Candidate.Outer.OutsideIncomingIndex) ==
               Candidate.Outer.Start &&
           Candidate.Outer.IV->getIncomingBlock(
               Candidate.Outer.OutsideIncomingIndex) ==
               Candidate.OuterPreHeader &&
           Candidate.Inner.IV->getIncomingValue(
               Candidate.Inner.OutsideIncomingIndex) ==
               Candidate.Inner.Start &&
           Candidate.Inner.Trip.ExitCmp->getOperand(
               Candidate.Inner.BoundOperandIndex) ==
               Candidate.Inner.Bound &&
           Candidate.Outer.Trip.ExitCmp->getOperand(
               Candidate.Outer.BoundOperandIndex) ==
               Candidate.Outer.Bound;
}

void LoopTilingPass::destroyTileBlocks(
    std::vector<BasicBlock *> &TileBlocks) {
    for (BasicBlock *BB : TileBlocks) {
        if (!BB) continue;
        for (Instruction *I : *BB)
            if (I) I->clearOperands();
    }
    for (BasicBlock *BB : TileBlocks)
        if (BB) BB->clear();
    for (BasicBlock *BB : TileBlocks)
        delete BB;
    TileBlocks.clear();
}

bool LoopTilingPass::executeTiling(Function &F,
                                   TilingCandidate &Candidate) {
    Loop *Outer = Candidate.Outer.L;
    if (!Outer || !Outer->getHeader() || !Candidate.Inner.L ||
        !Candidate.Inner.L->getHeader()) {
        return false;
    }

    auto *TileIHeader = new BasicBlock();
    auto *TileIBody = new BasicBlock();
    auto *TileJHeader = new BasicBlock();
    auto *TileJBody = new BasicBlock();
    auto *TileJLatch = new BasicBlock();
    auto *TileILatch = new BasicBlock();
    std::vector<BasicBlock *> TileBlocks = {
        TileIHeader, TileIBody, TileJHeader,
        TileJBody, TileJLatch, TileILatch};
    TileIHeader->setIdent(F.makeUniqueBlockIdent(
        Outer->getHeader()->getIdent() + ".tile.i.header"));
    TileIBody->setIdent(F.makeUniqueBlockIdent(
        Outer->getHeader()->getIdent() + ".tile.i.body"));
    TileJHeader->setIdent(F.makeUniqueBlockIdent(
        Outer->getHeader()->getIdent() + ".tile.j.header"));
    TileJBody->setIdent(F.makeUniqueBlockIdent(
        Outer->getHeader()->getIdent() + ".tile.j.body"));
    TileJLatch->setIdent(F.makeUniqueBlockIdent(
        Outer->getHeader()->getIdent() + ".tile.j.latch"));
    TileILatch->setIdent(F.makeUniqueBlockIdent(
        Outer->getHeader()->getIdent() + ".tile.i.latch"));

    auto *TileI = new PhiInst(Candidate.Outer.IV->getIRType());
    TileIHeader->pushBack(TileI);
    Value *IEnd = makeTileEnd(TileIBody, TileI, Candidate.Outer.Bound,
                              Candidate.Outer.TileSpan);
    if (!IEnd) {
        destroyTileBlocks(TileBlocks);
        return false;
    }
    TileI->addIncoming(Candidate.Outer.Start, Candidate.OuterPreHeader);
    TileI->addIncoming(IEnd, TileILatch);
    auto *TileICmp = new BinaryInst(TileI, BinaryInst::L,
                                    Candidate.Outer.Bound);
    TileIHeader->pushBack(TileICmp);
    TileIHeader->genCondInst(TileICmp, TileIBody, Candidate.OuterExit);
    TileIBody->genUnCondInst(TileJHeader);

    auto *TileJ = new PhiInst(Candidate.Inner.IV->getIRType());
    TileJHeader->pushBack(TileJ);
    Value *JEnd = makeTileEnd(TileJBody, TileJ, Candidate.Inner.Bound,
                              Candidate.Inner.TileSpan);
    if (!JEnd) {
        destroyTileBlocks(TileBlocks);
        return false;
    }
    TileJ->addIncoming(Candidate.Inner.Start, TileIBody);
    TileJ->addIncoming(JEnd, TileJLatch);
    auto *TileJCmp = new BinaryInst(TileJ, BinaryInst::L,
                                    Candidate.Inner.Bound);
    TileJHeader->pushBack(TileJCmp);
    TileJHeader->genCondInst(TileJCmp, TileJBody, TileILatch);
    TileJBody->genUnCondInst(Outer->getHeader());
    TileJLatch->genUnCondInst(TileJHeader);
    TileILatch->genUnCondInst(TileIHeader);

    if (!validateCommitState(Candidate)) {
        destroyTileBlocks(TileBlocks);
        return false;
    }

    BrInst *PreBranch = Candidate.OuterPreHeader->getBranchTerminator();
    BrInst *ExitBranch =
        Candidate.Outer.Trip.ExitingBlock->getBranchTerminator();
    unsigned ExitSuccessorIndex = 0;
    while (ExitSuccessorIndex < ExitBranch->getSuccessorCount() &&
           ExitBranch->getSuccessor(ExitSuccessorIndex) !=
               Candidate.OuterExit) {
        ++ExitSuccessorIndex;
    }
    if (ExitSuccessorIndex == ExitBranch->getSuccessorCount()) {
        destroyTileBlocks(TileBlocks);
        return false;
    }

    for (BasicBlock *BB : TileBlocks) F.pushBlock(BB);
    PreBranch->setSuccessor(0, TileIHeader);
    ExitBranch->setSuccessor(ExitSuccessorIndex, TileJLatch);

    Candidate.Outer.IV->setIncomingValue(
        Candidate.Outer.OutsideIncomingIndex, TileI);
    Candidate.Outer.IV->setIncomingBlock(
        Candidate.Outer.OutsideIncomingIndex, TileJBody);
    Candidate.Inner.IV->setIncomingValue(
        Candidate.Inner.OutsideIncomingIndex, TileJ);
    Candidate.Outer.Trip.ExitCmp->replaceOperand(
        Candidate.Outer.BoundOperandIndex, IEnd);
    Candidate.Inner.Trip.ExitCmp->replaceOperand(
        Candidate.Inner.BoundOperandIndex, JEnd);
    return true;
}

PassResult LoopTilingPass::run(Function &F,
                               FunctionAnalysisPassManager &FAM) {
    unsigned &Count = TransformCounts[&F];
    if (Count >= Config.MaxTransformsPerFunction)
        return PassResult::unchanged();

    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LA = FAM.getResult<LoopAccessAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominantAnalysis>(F);
    auto &Headers = TransformedHeaders[&F];

    for (Loop *Inner : LI.getLoopsInPostOrder()) {
        Loop *Outer = Inner ? Inner->getParentLoop() : nullptr;
        if (!Outer || !Outer->getHeader() ||
            Headers.count(Outer->getHeader())) {
            continue;
        }
        auto Candidate = buildCandidate(Outer, Inner, LI, SE, LA, DT, PDT,
                                        Config);
        if (!Candidate || !executeTiling(F, *Candidate)) continue;
        Headers.insert(Outer->getHeader());
        ++Count;
        return PassResult::changed(PreservationStatus::none());
    }
    return PassResult::unchanged();
}
