#include "ir/opt/LoopInterchange.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/CFG.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

bool LoopInterchangePass::expressionKeysEqual(
    const SCEVExpr *A, const SCEVExpr *B) {
    return A && B && !A->Key.empty() && A->Key == B->Key;
}

unsigned LoopInterchangePass::stridePenalty(int64_t Stride) {
    uint64_t Magnitude = Stride < 0
        ? static_cast<uint64_t>(-(Stride + 1)) + 1
        : static_cast<uint64_t>(Stride);
    unsigned Penalty = 0;
    while (Magnitude != 0) {
        ++Penalty;
        Magnitude >>= 1;
    }
    return Penalty;
}

bool LoopInterchangePass::isInstructionInBlocks(
    Instruction *I, const std::unordered_set<BasicBlock *> &Blocks) {
    return I && I->getParent() && Blocks.count(I->getParent()) != 0;
}

void LoopInterchangePass::destroyDetachedInstructions(
    std::vector<Instruction *> &Instructions) {
    for (Instruction *I : Instructions)
        if (I) I->clearOperands();
    for (Instruction *I : Instructions)
        delete I;
    Instructions.clear();
}

std::size_t LoopInterchangePass::HeaderPairHash::operator()(
    const HeaderPair &Pair) const {
    std::size_t A = std::hash<BasicBlock *>{}(Pair.Outer);
    std::size_t B = std::hash<BasicBlock *>{}(Pair.Inner);
    return A ^ (B + 0x9e3779b97f4a7c15ULL + (A << 6) + (A >> 2));
}

bool LoopInterchangePass::sameIterationDomain(
    Loop *Outer, Loop *Inner, SCEVResult &SE,
    PhiInst *&OuterIV, PhiInst *&InnerIV) {
    OuterIV = nullptr;
    InnerIV = nullptr;
    if (!Outer || !Inner) return false;

    auto OuterDesc = SE.getCanonicalInduction(Outer);
    auto InnerDesc = SE.getCanonicalInduction(Inner);
    auto OuterTrip = SE.analyzeLoopTripCount(Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Inner);
    if (!OuterDesc || !InnerDesc || !OuterDesc->Valid || !InnerDesc->Valid ||
        !OuterTrip || !InnerTrip || !OuterTrip->Valid || !InnerTrip->Valid ||
        !OuterTrip->TripCount || !InnerTrip->TripCount ||
        !OuterDesc->Phi || !InnerDesc->Phi ||
        !OuterDesc->ConstantStep || !InnerDesc->ConstantStep ||
        *OuterDesc->ConstantStep == 0 ||
        *OuterDesc->ConstantStep != *InnerDesc->ConstantStep) {
        return false;
    }
    if (!SE.isKnownFiniteLoop(Outer) ||
        !SE.isKnownFiniteLoop(Inner)) {
        return false;
    }

    // Equal trip counts alone are not sufficient: two translated domains can
    // have the same size but enumerate different IV values.  Match start and
    // step as well, using SCEV's interned canonical spelling.
    if (!expressionKeysEqual(OuterDesc->Start, InnerDesc->Start) ||
        !expressionKeysEqual(OuterDesc->Step, InnerDesc->Step) ||
        !expressionKeysEqual(OuterTrip->TripCount, InnerTrip->TripCount) ||
        OuterTrip->MayBeZeroTrip != InnerTrip->MayBeZeroTrip) {
        return false;
    }

    if (!OuterDesc->Phi->getIRType() || !InnerDesc->Phi->getIRType() ||
        !OuterDesc->Phi->getIRType()->hasSameShape(
            *InnerDesc->Phi->getIRType())) {
        return false;
    }

    OuterIV = OuterDesc->Phi;
    InnerIV = InnerDesc->Phi;
    return true;
}

bool LoopInterchangePass::hasOnlyCanonicalHeaderPhi(Loop *L, PhiInst *IV) {
    if (!L || !L->getHeader() || !IV || IV->getParent() != L->getHeader())
        return false;
    const auto Phis = L->getHeader()->collectPhiNodes();
    return Phis.size() == 1 && Phis.front() == IV;
}

bool LoopInterchangePass::hasTriangularDescendant(
    Loop *Inner, SCEVResult &SE) {
    if (!Inner) return true;
    for (Loop *Sub : Inner->getSubLoops()) {
        if (!Sub || !SE.isKnownFiniteLoop(Sub)) return true;
        auto Desc = SE.getCanonicalInduction(Sub);
        auto Trip = SE.analyzeLoopTripCount(Sub);
        if (!Desc || !Trip || !Desc->Valid || !Trip->Valid ||
            !Desc->Start || !Trip->TripCount) {
            return true;
        }
        // A descendant whose start/count changes with the selected inner axis
        // is triangular or otherwise non-rectangular.  A role permutation of
        // only the two outer axes would strand that dependent schedule.
        if (Desc->Start->mentionsLoop(Inner) ||
            Trip->TripCount->mentionsLoop(Inner) ||
            hasTriangularDescendant(Sub, SE)) {
            return true;
        }
    }
    return false;
}

bool LoopInterchangePass::isPureSinkInstruction(Instruction *I) {
    if (!I || dynamic_cast<PhiInst *>(I) || dynamic_cast<LoadInst *>(I) ||
        dynamic_cast<StoreInst *>(I) || dynamic_cast<CallInst *>(I) ||
        dynamic_cast<AllocaInst *>(I) || dynamic_cast<BrInst *>(I) ||
        dynamic_cast<RetInst *>(I) || dceInstructionMustBePreserved(I) ||
        instructionMayFaultOrProducePoison(I)) {
        return false;
    }

    return dynamic_cast<GetElementPtrInst *>(I) ||
           dynamic_cast<BinaryInst *>(I) ||
           dynamic_cast<BitCastInst *>(I) ||
           dynamic_cast<ZextInst *>(I) ||
           dynamic_cast<SextInst *>(I) ||
           dynamic_cast<TruncInst *>(I);
}

bool LoopInterchangePass::collectSinkablePrefix(Candidate &C) {
    if (!C.Outer || !C.Inner || !C.OuterIV || !C.InnerIV ||
        !C.InnerPreHeader) {
        return false;
    }

    std::unordered_set<Instruction *> Marked;
    for (Instruction *I : *C.InnerPreHeader) {
        if (!I || dynamic_cast<BrInst *>(I)) continue;
        bool DependsOnOuter = false;
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            Value *Operand = I->getOperand(Index);
            auto *OperandI = dynamic_cast<Instruction *>(Operand);
            if (Operand == C.OuterIV || (OperandI && Marked.count(OperandI))) {
                DependsOnOuter = true;
                break;
            }
        }
        if (!DependsOnOuter) continue;
        if (!isPureSinkInstruction(I) ||
            C.SinkablePrefix.size() >= MaxSinkInstructions) {
            return false;
        }
        Marked.insert(I);
        C.SinkablePrefix.push_back(I);
    }

    // Every value we sink must be private to the selected inner subtree (or
    // feed another member of the same prefix).  Otherwise retaining the old
    // computation and cloning a role-swapped copy would give two consumers
    // different interpretations of the same source-level axis.
    for (Instruction *I : C.SinkablePrefix) {
        for (Use *U : I->getUses()) {
            auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
            if (!UserI) return false;
            // A header PHI consumes its outside value on the preheader edge;
            // a clone inserted in the header body cannot dominate that use.
            if (dynamic_cast<PhiInst *>(UserI) &&
                UserI->getParent() == C.Inner->getHeader()) {
                return false;
            }
            if (Marked.count(UserI) || isInstructionInBlocks(UserI, C.InnerBlocks))
                continue;
            return false;
        }
    }
    return true;
}

bool LoopInterchangePass::collectRoleUses(Candidate &C) {
    if (!C.Outer || !C.Inner || !C.OuterIV || !C.InnerIV) return false;
    std::unordered_set<Instruction *> SinkSet(C.SinkablePrefix.begin(),
                                               C.SinkablePrefix.end());

    for (Use *U : C.OuterIV->getUses()) {
        auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
        if (!UserI || !UserI->getParent()) return false;
        if (C.OuterControl.count(UserI)) continue;
        if (SinkSet.count(UserI)) continue;
        if (!isInstructionInBlocks(UserI, C.InnerBlocks)) return false;
        if (C.InnerControl.count(UserI)) return false;
        // An inner-header PHI consumes its outside incoming value on the
        // preheader edge, where InnerIV is not available.
        if (dynamic_cast<PhiInst *>(UserI) &&
            UserI->getParent() == C.Inner->getHeader()) {
            return false;
        }
        C.OuterUses.push_back(U);
    }

    for (Use *U : C.InnerIV->getUses()) {
        auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
        if (!UserI || !UserI->getParent()) return false;
        if (C.InnerControl.count(UserI)) continue;
        if (!isInstructionInBlocks(UserI, C.InnerBlocks)) return false;
        C.InnerUses.push_back(U);
    }

    return !C.OuterUses.empty() && !C.InnerUses.empty();
}

bool LoopInterchangePass::collectControlSlice(
    Value *V, PhiInst *IV, Loop *L,
    std::unordered_set<Instruction *> &Control,
    std::unordered_set<Value *> &Active, unsigned Depth) {
    if (!V || !IV || !L || Depth > 64) return false;
    if (V == IV || V->isConst()) return true;

    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || !L->contains(I->getParent())) return true;
    if (!Active.insert(V).second) return false;
    if (!dynamic_cast<BinaryInst *>(I) && !dynamic_cast<PhiInst *>(I) &&
        !dynamic_cast<ZextInst *>(I) && !dynamic_cast<SextInst *>(I) &&
        !dynamic_cast<TruncInst *>(I)) {
        Active.erase(V);
        return false;
    }

    Control.insert(I);
    bool Valid = true;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (dynamic_cast<BasicBlock *>(Operand)) continue;
        if (!collectControlSlice(Operand, IV, L, Control, Active,
                                 Depth + 1)) {
            Valid = false;
            break;
        }
    }
    Active.erase(V);
    return Valid;
}

long long LoopInterchangePass::computeLocalityBenefit(
    const Candidate &C, LoopDependenceInfo &LD) {
    long long Benefit = 0;
    for (const LoopMemoryAccess &Access : LD.getMemoryAccesses(C.Outer)) {
        if (!Access.Inst || !isInstructionInBlocks(Access.Inst, C.InnerBlocks))
            continue;
        std::optional<int64_t> OuterStride =
            LD.getByteStride(Access.Inst, C.Outer);
        std::optional<int64_t> InnerStride =
            LD.getByteStride(Access.Inst, C.Inner);
        if (!OuterStride || !InnerStride) continue;

        const long long Weight = Access.Writes ? 2 : 1;
        Benefit += Weight *
            (static_cast<long long>(stridePenalty(*InnerStride)) -
             static_cast<long long>(stridePenalty(*OuterStride)));
    }
    return Benefit;
}

bool LoopInterchangePass::validateNestShape(
    const Candidate &C, const LoopInfo &LI, const DominatorTree &DT,
    const PostDominatorTree &PDT) {
    if (!C.Outer || !C.Inner || !C.OuterIV || !C.InnerIV ||
        C.Inner->getParentLoop() != C.Outer ||
        C.Outer->getSubLoops().size() != 1 ||
        C.Outer->getSubLoops().front() != C.Inner ||
        !LCSSAPass::isLCSSAForm(C.Outer) ||
        !LCSSAPass::isLCSSAForm(C.Inner)) {
        return false;
    }

    BasicBlock *OuterPreHeader = nullptr;
    BasicBlock *OuterLatch = nullptr;
    BasicBlock *InnerPreHeader = nullptr;
    BasicBlock *InnerLatch = nullptr;
    if (!C.Outer->getLoopSimplifyForm(OuterPreHeader, OuterLatch) ||
        !C.Inner->getLoopSimplifyForm(InnerPreHeader, InnerLatch) ||
        !OuterPreHeader || !OuterLatch || !InnerPreHeader || !InnerLatch ||
        InnerPreHeader != C.InnerPreHeader ||
        !C.Outer->contains(InnerPreHeader) ||
        C.Inner->contains(InnerPreHeader)) {
        return false;
    }

    // Every progressing outer iteration must pass through the inner loop.
    // The zero-trip edge of the outer loop is intentionally allowed.
    if (!DT.dominates(C.Outer->getHeader(), C.Inner->getHeader()) ||
        !DT.dominates(C.Inner->getHeader(), OuterLatch) ||
        !PDT.isReachableToExit(C.Outer->getHeader()) ||
        !PDT.isReachableToExit(C.Inner->getHeader())) {
        return false;
    }

    // Calls are barriers for this first implementation.  Direct outer-loop
    // stores/loads would be statements outside the permuted inner domain and
    // therefore make the nest imperfect in a semantically relevant way.
    for (BasicBlock *BB : C.Outer->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (dynamic_cast<CallInst *>(I)) return false;
            if (LI.getLoopFor(BB) == C.Outer &&
                (dynamic_cast<StoreInst *>(I) ||
                 dynamic_cast<LoadInst *>(I))) {
                return false;
            }
        }
    }
    return true;
}

bool LoopInterchangePass::applyCandidate(Function &F, Candidate &C) {
    if (!C.OuterIV || !C.InnerIV || !C.Inner ||
        !C.Inner->getHeader()) {
        return false;
    }

    std::unordered_map<Value *, Value *> VMap;
    VMap[C.OuterIV] = C.InnerIV;
    std::vector<Instruction *> Clones;
    Clones.reserve(C.SinkablePrefix.size());
    for (Instruction *OldI : C.SinkablePrefix) {
        Instruction *NewI = OldI ? OldI->clone(VMap) : nullptr;
        if (!NewI) {
            destroyDetachedInstructions(Clones);
            return false;
        }
        NewI->setIdent(OldI->getIdent() + ".interchange");
        Clones.push_back(NewI);
        VMap[OldI] = NewI;
    }

    // Complete every fallible validation before attaching clones or changing
    // uses.  The commit phase below is then infallible and cannot leave a
    // partially interchanged loop while reporting unchanged.
    for (Instruction *OldI : C.SinkablePrefix) {
        auto It = VMap.find(OldI);
        if (It == VMap.end() ||
            !dynamic_cast<Instruction *>(It->second)) {
            destroyDetachedInstructions(Clones);
            return false;
        }
    }
    for (Use *U : C.OuterUses) {
        if (!U || U->getValue() != C.OuterIV || C.InnerIV == C.OuterIV) {
            destroyDetachedInstructions(Clones);
            return false;
        }
    }
    for (Use *U : C.InnerUses) {
        if (!U || U->getValue() != C.InnerIV || C.OuterIV == C.InnerIV) {
            destroyDetachedInstructions(Clones);
            return false;
        }
    }

    // insertAfterPhiNodes always inserts at the front of the non-PHI range;
    // inserting in reverse retains the prefix's dependency order.
    for (auto It = Clones.rbegin(); It != Clones.rend(); ++It) {
        if (!C.Inner->getHeader()->insertAfterPhiNodes(*It)) {
            // No mutation other than already attached pure clones occurred.
            // Detach/delete all of them before reporting failure.
            for (Instruction *Clone : Clones) {
                if (Clone && Clone->getParent()) Clone->eraseFromParent();
            }
            destroyDetachedInstructions(Clones);
            return false;
        }
    }

    for (Instruction *OldI : C.SinkablePrefix) {
        auto It = VMap.find(OldI);
        auto *Replacement = dynamic_cast<Instruction *>(It->second);
        for (Use *U : OldI->getUses()) {
            auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
            if (isInstructionInBlocks(UserI, C.InnerBlocks))
                U->replaceUseeWith(Replacement);
        }
    }

    // The snapshots were taken before either replacement, so changing one
    // IV's uses cannot pollute the worklist for the other IV.
    for (Use *U : C.OuterUses)
        U->replaceUseeWith(C.InnerIV);
    for (Use *U : C.InnerUses)
        U->replaceUseeWith(C.OuterIV);

    // A valid role swap often needs no sinkable-prefix clone.  In that common
    // case the old instruction-only suffix above left no observable
    // provenance at all, so IR audits incorrectly reported that interchange
    // never fired.  Mark the complete inner payload region after the
    // infallible commit.  Loop rotation may replace either selected header,
    // while at least one payload block must survive every semantics-preserving
    // cleanup.  Function order keeps the chosen identifiers deterministic;
    // identifiers themselves do not participate in CFG or SSA semantics.
    for (BasicBlock *BB : F) {
        if (!BB || !C.Inner->contains(BB)) continue;
        BB->setIdent(F.makeUniqueBlockIdent(
            BB->getIdent() + ".interchange"));
    }
    return true;
}

PassResult LoopInterchangePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    if (TransformCount[&F] >= MaxInterchangesPerFunction)
        return PassResult::unchanged();

    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);

    for (Loop *Inner : LI.getLoopsInPostOrder()) {
        Loop *Outer = Inner ? Inner->getParentLoop() : nullptr;
        if (!Outer || !Outer->getHeader() || !Inner->getHeader()) continue;
        HeaderPair Key{Outer->getHeader(), Inner->getHeader()};
        if (Transformed.count(Key)) continue;

        Candidate C;
        C.Outer = Outer;
        C.Inner = Inner;
        if (!sameIterationDomain(Outer, Inner, SE, C.OuterIV, C.InnerIV))
            continue;
        if (!hasOnlyCanonicalHeaderPhi(Outer, C.OuterIV) ||
            !hasOnlyCanonicalHeaderPhi(Inner, C.InnerIV))
            continue;
        if (hasTriangularDescendant(Inner, SE))
            continue;

        BasicBlock *InnerLatch = nullptr;
        if (!Inner->getLoopSimplifyForm(C.InnerPreHeader, InnerLatch) ||
            !C.InnerPreHeader || !InnerLatch) {
            continue;
        }
        for (BasicBlock *BB : Inner->getBlocksIncludingSubLoops())
            if (BB) C.InnerBlocks.insert(BB);

        auto OuterDesc = SE.getCanonicalInduction(Outer);
        auto InnerDesc = SE.getCanonicalInduction(Inner);
        auto OuterTrip = SE.analyzeLoopTripCount(Outer);
        auto InnerTrip = SE.analyzeLoopTripCount(Inner);
        if (!OuterDesc || !InnerDesc || !OuterTrip || !InnerTrip ||
            !OuterTrip->ExitCmp || !InnerTrip->ExitCmp) {
            continue;
        }
        C.OuterControl.insert(C.OuterIV);
        C.InnerControl.insert(C.InnerIV);
        std::unordered_set<Value *> OuterControlActive;
        std::unordered_set<Value *> InnerControlActive;
        if (!collectControlSlice(OuterDesc->UpdateValue, C.OuterIV, Outer,
                                 C.OuterControl, OuterControlActive) ||
            !collectControlSlice(InnerDesc->UpdateValue, C.InnerIV, Inner,
                                 C.InnerControl, InnerControlActive)) {
            continue;
        }
        C.OuterControl.insert(OuterTrip->ExitCmp);
        C.InnerControl.insert(InnerTrip->ExitCmp);
        if (OuterTrip->ExitingBlock && OuterTrip->ExitingBlock->getTerminator())
            C.OuterControl.insert(OuterTrip->ExitingBlock->getTerminator());
        if (InnerTrip->ExitingBlock && InnerTrip->ExitingBlock->getTerminator())
            C.InnerControl.insert(InnerTrip->ExitingBlock->getTerminator());

        if (!validateNestShape(C, LI, DT, PDT))
            continue;
        if (LD.hasUnknownMemory(Outer))
            continue;
        if (!LD.isInterchangeLegal(Outer, Inner))
            continue;
        if (!collectSinkablePrefix(C))
            continue;
        if (!collectRoleUses(C))
            continue;

        C.LocalityBenefit = computeLocalityBenefit(C, LD);
        if (C.LocalityBenefit < MinLocalityBenefit) continue;

        if (!applyCandidate(F, C)) continue;
        Transformed.insert(Key);
        ++TransformCount[&F];

        PreservationStatus PA;
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.discard<SCEVAnalysis>();
        PA.discard<BasicAliasAnalysis>();
        PA.discard<MemorySSAAnalysis>();
        PA.discard<LoopDependenceAnalysis>();
        PA.discard<SideEffectAnalysis>();
        return PassResult::changed(std::move(PA));
    }

    return PassResult::unchanged();
}
