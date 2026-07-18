#include "ir/opt/UnrollAndJam.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "ir/opt/LoopUnroll.hpp"
#include "lib/CFG.hpp"

#include <algorithm>
#include <limits>
#include <utility>

UnrollAndJamPass::UnrollAndJamPass(UnrollAndJamConfig C)
    : Config(std::move(C)) {}

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

bool UnrollAndJamPass::hasOnlyCanonicalHeaderPhi(Loop *L, PhiInst *IV) {
    if (!L || !L->getHeader() || !IV) return false;
    const auto Phis = L->getHeader()->collectPhiNodes();
    return Phis.size() == 1 && Phis.front() == IV;
}

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

bool UnrollAndJamPass::collectSharedLoopControl(
    UnrollAndJamShape &Shape, Loop *Current, SCEVResult &SE,
    const UnrollAndJamConfig &C, unsigned Depth) {
    if (!Current || !Shape.Outer || Depth > C.MaxSharedNestDepth ||
        !SE.isKnownFiniteLoop(Current)) {
        return false;
    }
    auto Trip = SE.analyzeLoopTripCount(Current);
    auto Induction = SE.getCanonicalInduction(Current);
    if (!Trip || !Trip->Valid || !Trip->Start || !Trip->Step ||
        !Trip->Bound || !Trip->ExitCmp || !Trip->ExitingBlock ||
        !Induction || !Induction->Valid || !Induction->Phi ||
        !Induction->UpdateValue || !Induction->ConstantStep ||
        *Induction->ConstantStep <= 0 ||
        Trip->Start->mentionsLoop(Shape.Outer) ||
        Trip->Step->mentionsLoop(Shape.Outer) ||
        Trip->Bound->mentionsLoop(Shape.Outer) ||
        !hasControlOnlyHeader(Current, Induction->Phi, *Trip,
                              /*AllowReductionPhis=*/true)) {
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
        if (!collectSharedLoopControl(Shape, SubLoop, SE, C, Depth + 1))
            return false;
    }
    return true;
}

bool UnrollAndJamPass::collectControl(
    UnrollAndJamShape &Shape, SCEVResult &SE,
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
    if (!collectSharedLoopControl(Shape, Shape.Inner, SE, C, 1))
        return false;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB || !BB->getTerminator()) return false;
        Shape.Control.insert(BB->getTerminator());
    }
    return true;
}

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

bool UnrollAndJamPass::instructionDependsOnLane(
    Instruction *I,
    const std::unordered_map<Value *, Value *> &ValueMap) {
    if (!I) return false;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index)
        if (ValueMap.count(I->getOperand(Index))) return true;
    return false;
}

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

    // 地址计算是短生命周期链，只按两级同时活跃临时量估算；浮点
    // load/mul/add/PHI 全部计入，防止八路归约造成隐蔽的寄存器溢出。
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

bool UnrollAndJamPass::isSupportedFactor(int Factor) {
    return Factor == 2 || Factor == 4 || Factor == 8;
}

std::optional<int> UnrollAndJamPass::selectFactor(
    const UnrollAndJamShape &Shape, LoopAccessInfo &LA, SCEVResult &SE,
    const UnrollAndJamConfig &C) {
    if (!Shape.OuterTrip.ConstantTripCount || C.MaxFactor < 2)
        return std::nullopt;

    std::unordered_set<Instruction *> LaneValues(
        Shape.LaneInstructions.begin(), Shape.LaneInstructions.end());
    const bool Shareable = hasShareableLaneRead(Shape, LA, LaneValues);
    if (!Shareable) return std::nullopt;
    const bool LongTraversal = hasLaneCoalescingBenefit(
        Shape, LA, SE, LaneValues, C);
    const unsigned StrictReductions = countStrictFPReductions(Shape);

    auto IsProfitable = [&](int Factor) {
        if (!isSupportedFactor(Factor) || Factor > C.MaxFactor ||
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
    // 没有严格浮点延迟链时沿用保守的两路选择，避免仅凭复制扩大压力。
    if (IsProfitable(2)) return 2;
    return std::nullopt;
}

bool UnrollAndJamPass::collectLaneInstructions(
    UnrollAndJamShape &Shape, const UnrollAndJamConfig &C) {
    std::unordered_map<Value *, Value *> LaneValues;
    LaneValues[Shape.OuterInduction.Phi] = Shape.OuterInduction.Phi;

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

std::optional<UnrollAndJamShape> UnrollAndJamPass::buildShape(
    Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
    LoopAccessInfo &LA, DominatorTree &DT, PostDominatorTree &PDT,
    const UnrollAndJamConfig &C) {
    if (!Outer || !Inner ||
        !LA.isRectangularNest(Outer, Inner) ||
        !LA.areIterationsIndependent(Outer, Outer)) {
        return std::nullopt;
    }
    auto OuterTrip = SE.analyzeLoopTripCount(Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Inner);
    auto OuterInduction = SE.getCanonicalInduction(Outer);
    auto InnerInduction = SE.getCanonicalInduction(Inner);
    if (!OuterTrip || !InnerTrip || !OuterInduction || !InnerInduction ||
        !OuterTrip->ConstantTripCount ||
        *OuterTrip->ConstantTripCount < 2 ||
        !OuterInduction->ConstantStep ||
        *OuterInduction->ConstantStep <= 0 ||
        !SE.isKnownNoWrapInductionValue(
            Outer, OuterInduction->Phi) ||
        !hasOnlyCanonicalHeaderPhi(Outer, OuterInduction->Phi) ||
        !hasControlOnlyHeader(Outer, OuterInduction->Phi, *OuterTrip,
                              /*AllowReductionPhis=*/false)) {
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
    Shape.Blocks = Outer->getBlocksInFunctionOrder();
    if (!collectControl(Shape, SE, C) || !analyzeOuterControl(Shape) ||
        !validateShape(Shape, LI, SE, DT, PDT, C) ||
        !collectLaneInstructions(Shape, C)) {
        return std::nullopt;
    }
    return Shape;
}

void UnrollAndJamPass::destroyClonePlan(UnrollAndJamClonePlan &Plan) {
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
    const std::size_t Size = Ty ? Ty->getSize() : 4;

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
        Lane.LaneIV = new BinaryInst(
            Shape.OuterInduction.Phi, BinaryInst::ADD,
            ConstInt::newConstInt(static_cast<int>(WideOffset), Size));
        Lane.ValueMap[Shape.OuterInduction.Phi] = Lane.LaneIV;

        for (Instruction *Source : Shape.LaneInstructions) {
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
    Plan.NewOuterUpdate = new BinaryInst(
        Shape.OuterInduction.Phi, BinaryInst::ADD,
        ConstInt::newConstInt(static_cast<int>(NewStep), Size));
    return Plan;
}

bool UnrollAndJamPass::validatePlanCommit(
    const UnrollAndJamShape &Shape,
    const UnrollAndJamClonePlan &Plan) {
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

bool UnrollAndJamPass::executePlan(UnrollAndJamShape &Shape,
                                   UnrollAndJamClonePlan &Plan) {
    if (!validatePlanCommit(Shape, Plan)) return false;

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

PassResult UnrollAndJamPass::run(Function &F,
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
        auto Shape = buildShape(Outer, Inner, LI, SE, LA, DT, PDT, Config);
        if (!Shape) continue;
        auto Factor = selectFactor(*Shape, LA, SE, Config);
        if (!Factor || !Shape->OuterTrip.ConstantTripCount) continue;

        const std::int64_t Remainder =
            *Shape->OuterTrip.ConstantTripCount % *Factor;
        if (Remainder != 0) {
            if (Remainder < 0 ||
                Remainder > static_cast<std::int64_t>(
                                Config.MaxRemainderPeel)) {
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
                static_cast<int>(Config.MaxRemainderPeel);
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

        auto Plan = buildClonePlan(*Shape, LA, *Factor, Config);
        if (!Plan) continue;
        if (!executePlan(*Shape, *Plan)) {
            destroyClonePlan(*Plan);
            continue;
        }
        Headers.insert(Outer->getHeader());
        ++Count;

        PreservationStatus PA;
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        return PassResult::changed(std::move(PA));
    }
    return PassResult::unchanged();
}
