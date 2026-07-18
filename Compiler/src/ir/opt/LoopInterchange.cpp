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

namespace {

bool controlSliceContainsUndef(
    Value *Root, const std::unordered_set<Value *> &CanonicalPhis,
    std::unordered_set<Value *> &Active,
    std::unordered_set<Value *> &KnownSafe, unsigned Depth = 0) {
    if (!Root || Depth >= 64) return true;
    if (CanonicalPhis.count(Root)) return false;
    if (Root->isUndef()) return true;
    if (dynamic_cast<BasicBlock *>(Root)) return false;

    auto *I = dynamic_cast<Instruction *>(Root);
    if (!I) return false;
    if (KnownSafe.count(Root)) return false;
    if (!Active.insert(Root).second) return true;

    bool ContainsUndef = false;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (dynamic_cast<BasicBlock *>(Operand)) continue;
        if (controlSliceContainsUndef(
                Operand, CanonicalPhis, Active, KnownSafe, Depth + 1)) {
            ContainsUndef = true;
            break;
        }
    }
    Active.erase(Root);
    if (!ContainsUndef) KnownSafe.insert(Root);
    return ContainsUndef;
}

bool loopDomainContainsUndef(Loop *L, SCEVResult &SE) {
    struct DomainControl {
        PhiInst *Phi = nullptr;
        Value *Start = nullptr;
        Value *Update = nullptr;
        Value *ExitCmp = nullptr;
    };

    std::vector<DomainControl> Domains;
    std::unordered_set<Value *> CanonicalPhis;
    for (Loop *Current = L; Current; Current = Current->getParentLoop()) {
        auto Desc = SE.getCanonicalInduction(Current);
        auto Trip = SE.analyzeLoopTripCount(Current);
        if (!Desc || !Trip || !Desc->Valid || !Trip->Valid ||
            !Desc->Phi || !Desc->StartValue || !Desc->UpdateValue ||
            !Trip->ExitCmp) {
            return true;
        }
        Domains.push_back(
            {Desc->Phi, Desc->StartValue, Desc->UpdateValue,
             Trip->ExitCmp});
        CanonicalPhis.insert(Desc->Phi);
    }

    // 规范 PHI 作为已单独检查的递归边界；其起点、更新和退出比较仍逐一
    // 追溯。这样既不会在合法回边上无限递归，也不会让 undef 来源穿过
    // 循环控制链并在交换后获得不同的动态求值次数。
    std::unordered_set<Value *> Active;
    std::unordered_set<Value *> KnownSafe;
    for (const DomainControl &Domain : Domains) {
        if (controlSliceContainsUndef(
                Domain.Start, CanonicalPhis, Active, KnownSafe) ||
            controlSliceContainsUndef(
                Domain.Update, CanonicalPhis, Active, KnownSafe) ||
            controlSliceContainsUndef(
                Domain.ExitCmp, CanonicalPhis, Active, KnownSafe)) {
            return true;
        }
    }
    return false;
}

} // 匿名命名空间

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
        !SE.isKnownFiniteLoop(Inner) ||
        loopDomainContainsUndef(Outer, SE) ||
        loopDomainContainsUndef(Inner, SE)) {
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

bool LoopInterchangePass::hasUnsupportedDescendantDomain(
    Loop *Inner, SCEVResult &SE) {
    if (!Inner) return true;
    for (Loop *Sub : Inner->getSubLoops()) {
        if (!Sub || !SE.isKnownFiniteLoop(Sub)) return true;
        auto Desc = SE.getCanonicalInduction(Sub);
        auto Trip = SE.analyzeLoopTripCount(Sub);
        if (!Desc || !Trip || !Desc->Valid || !Trip->Valid ||
            !Desc->Start || !Desc->Step || !Trip->TripCount ||
            !Trip->Bound || loopDomainContainsUndef(Sub, SE)) {
            return true;
        }
        // 角色交换会改写整个内层子树中的归纳变量使用，后代循环依赖被交换轴
        // 的起点或上界也会随之改写，因此三角域本身不是拒绝理由。这里只拒绝
        // 无法由 SCEV 完整描述的后代域，最终合法性仍由循环依赖分析证明。
        if (hasUnsupportedDescendantDomain(Sub, SE)) {
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

bool LoopInterchangePass::buildRotatedNestCandidate(
    Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
    DominatorTree &DT, PostDominatorTree &PDT,
    LoopDependenceInfo &LD, RotatedNestCandidate &C) {
    C = RotatedNestCandidate{};
    if (!Outer || !Inner || Inner->getParentLoop() != Outer ||
        Outer->getSubLoops().size() != 1 ||
        Outer->getSubLoops().front() != Inner || !Inner->isInnermost() ||
        !LCSSAPass::isLCSSAForm(Outer) ||
        !LCSSAPass::isLCSSAForm(Inner) ||
        !SE.isKnownFiniteLoop(Outer) ||
        !SE.isKnownFiniteLoop(Inner)) {
        return false;
    }

    BasicBlock *Guard = nullptr;
    BasicBlock *OuterLatch = nullptr;
    BasicBlock *Prefix = nullptr;
    BasicBlock *InnerBody = nullptr;
    // 外循环可能由零次执行保护边直接跳到唯一出口。此时保护块并非经典
    // 单后继 preheader，出口也会多出保护块前驱，但头块仍必须只有一个
    // 循环内回边和一个循环外入口；其余边形状在下方逐项核验。
    auto getGuardedLoopForm = [](Loop *L, BasicBlock *&Entry,
                                 BasicBlock *&Latch) {
        Entry = nullptr;
        Latch = nullptr;
        if (!L || !L->getHeader()) return false;
        for (BasicBlock *Pred : L->getHeader()->getPredecessors()) {
            if (!Pred) continue;
            BasicBlock *&Slot = L->contains(Pred) ? Latch : Entry;
            if (Slot) return false;
            Slot = Pred;
        }
        return Entry && Latch &&
               Entry->countEdgesToSucc(L->getHeader()) == 1 &&
               Latch->countEdgesToSucc(L->getHeader()) == 1;
    };
    if (!getGuardedLoopForm(Outer, Guard, OuterLatch) ||
        !Inner->getLoopSimplifyForm(Prefix, InnerBody) ||
        !Guard || !OuterLatch || !Prefix || !InnerBody ||
        Prefix == Outer->getHeader() ||
        !Outer->contains(Prefix) || Inner->contains(Prefix)) {
        return false;
    }

    BasicBlock *OuterHeader = Outer->getHeader();
    BasicBlock *InnerHeader = Inner->getHeader();
    auto OuterDesc = SE.getCanonicalInduction(Outer);
    auto InnerDesc = SE.getCanonicalInduction(Inner);
    auto OuterTrip = SE.analyzeLoopTripCount(Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Inner);
    if (!OuterHeader || !InnerHeader ||
        !OuterDesc || !InnerDesc || !OuterTrip || !InnerTrip ||
        !OuterDesc->Valid || !InnerDesc->Valid ||
        !OuterTrip->Valid || !InnerTrip->Valid ||
        !OuterDesc->Phi || !InnerDesc->Phi ||
        !OuterDesc->Start || !OuterDesc->Step ||
        !InnerDesc->Start || !InnerDesc->Step ||
        !OuterTrip->Bound || !InnerTrip->Bound ||
        !OuterTrip->ExitCmp || !InnerTrip->ExitCmp ||
        !OuterDesc->StartValue || !InnerDesc->StartValue ||
        !OuterDesc->UpdateValue || !InnerDesc->UpdateValue ||
        OuterTrip->ExitingBlock != OuterHeader ||
        InnerTrip->ExitingBlock != InnerHeader ||
        InnerTrip->ExitBlock != OuterLatch ||
        !OuterTrip->ExitBlock ||
        loopDomainContainsUndef(Outer, SE) ||
        loopDomainContainsUndef(Inner, SE)) {
        return false;
    }
    auto *InnerUpdate = dynamic_cast<Instruction *>(
        InnerDesc->UpdateValue);
    BasicBlock *Exit = OuterTrip->ExitBlock;
    if (OuterDesc->Start->mentionsLoop(Inner) ||
        OuterDesc->Step->mentionsLoop(Inner) ||
        OuterTrip->Bound->mentionsLoop(Inner) ||
        InnerDesc->Start->mentionsLoop(Outer) ||
        InnerDesc->Step->mentionsLoop(Outer) ||
        InnerTrip->Bound->mentionsLoop(Outer)) {
        return false;
    }

    if (!hasOnlyCanonicalHeaderPhi(Outer, OuterDesc->Phi) ||
        !hasOnlyCanonicalHeaderPhi(Inner, InnerDesc->Phi) ||
        !Prefix->collectPhiNodes().empty() ||
        !InnerBody->collectPhiNodes().empty() ||
        !OuterLatch->collectPhiNodes().empty() ||
        !Exit->collectPhiNodes().empty()) {
        return false;
    }

    const auto OuterExits = Outer->getExitBlocks();
    const auto InnerExits = Inner->getExitBlocks();
    if (OuterExits.size() != 1 || OuterExits.front() != Exit ||
        InnerExits.size() != 1 || InnerExits.front() != OuterLatch) {
        return false;
    }

    const auto &OuterDirectBlocks = Outer->getBlocks();
    const auto InnerBlocks = Inner->getBlocksIncludingSubLoops();
    if (OuterDirectBlocks.size() != 3 || InnerBlocks.size() != 2)
        return false;
    std::unordered_set<BasicBlock *> OuterDirect(
        OuterDirectBlocks.begin(), OuterDirectBlocks.end());
    std::unordered_set<BasicBlock *> InnerBlockSet(
        InnerBlocks.begin(), InnerBlocks.end());
    if (!OuterDirect.count(OuterHeader) || !OuterDirect.count(Prefix) ||
        !OuterDirect.count(OuterLatch) ||
        !InnerBlockSet.count(InnerHeader) ||
        !InnerBlockSet.count(InnerBody) ||
        LI.getLoopFor(OuterHeader) != Outer ||
        LI.getLoopFor(Prefix) != Outer ||
        LI.getLoopFor(OuterLatch) != Outer ||
        LI.getLoopFor(InnerHeader) != Inner ||
        LI.getLoopFor(InnerBody) != Inner) {
        return false;
    }

    auto *GuardBr = Guard->getBranchTerminator();
    auto *OuterHeaderBr = OuterHeader->getBranchTerminator();
    auto *PrefixBr = Prefix->getBranchTerminator();
    auto *InnerHeaderBr = InnerHeader->getBranchTerminator();
    auto *InnerBodyBr = InnerBody->getBranchTerminator();
    auto *OuterLatchBr = OuterLatch->getBranchTerminator();
    if ((!BasicBlock::isUncondBr(GuardBr) &&
         !BasicBlock::isCondBr(GuardBr)) ||
        !BasicBlock::isCondBr(OuterHeaderBr) ||
        !BasicBlock::isUncondBr(PrefixBr) ||
        !BasicBlock::isCondBr(InnerHeaderBr) ||
        !BasicBlock::isUncondBr(InnerBodyBr) ||
        !BasicBlock::isUncondBr(OuterLatchBr) ||
        PrefixBr->getSuccessor(0) != InnerHeader ||
        InnerBodyBr->getSuccessor(0) != InnerHeader ||
        OuterLatchBr->getSuccessor(0) != OuterHeader ||
        Guard->countEdgesToSucc(OuterHeader) != 1 ||
        OuterHeader->countEdgesToSucc(Prefix) != 1 ||
        Prefix->countEdgesToSucc(InnerHeader) != 1 ||
        InnerBody->countEdgesToSucc(InnerHeader) != 1 ||
        OuterHeaderBr->getCondition() != OuterTrip->ExitCmp ||
        InnerHeaderBr->getCondition() != InnerTrip->ExitCmp ||
        !InnerUpdate || InnerUpdate->getParent() != InnerHeader) {
        return false;
    }
    if (GuardBr->isConditional()) {
        BasicBlock *Other = GuardBr->getSuccessor(0) == OuterHeader
            ? GuardBr->getSuccessor(1) : GuardBr->getSuccessor(0);
        if (Other != Exit) return false;
    } else if (GuardBr->getSuccessor(0) != OuterHeader) {
        return false;
    }

    const unsigned OuterContinueIndex =
        OuterTrip->ContinueOnTrue ? 0U : 1U;
    const unsigned OuterExitIndex = OuterContinueIndex ^ 1U;
    const unsigned InnerContinueIndex =
        InnerTrip->ContinueOnTrue ? 0U : 1U;
    const unsigned InnerExitIndex = InnerContinueIndex ^ 1U;
    if (OuterHeaderBr->getSuccessor(OuterContinueIndex) != Prefix ||
        OuterHeaderBr->getSuccessor(OuterExitIndex) != Exit ||
        InnerHeaderBr->getSuccessor(InnerContinueIndex) != InnerBody ||
        InnerHeaderBr->getSuccessor(InnerExitIndex) != OuterLatch ||
        OuterLatchBr->getSuccessor(0) != OuterHeader) {
        return false;
    }

    auto findIncoming = [](PhiInst *Phi, BasicBlock *Pred,
                           Value *Expected, unsigned &Index) {
        if (!Phi || !Pred || !Expected ||
            Phi->countIncomingFromPred(Pred) != 1) {
            return false;
        }
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
            if (Phi->getIncomingBlock(I) != Pred) continue;
            if (Phi->getIncomingValue(I) != Expected) return false;
            Index = I;
            return true;
        }
        return false;
    };

    unsigned OuterBackedgeIndex = 0;
    if (OuterDesc->Phi->getNumIncomingValues() != 2 ||
        InnerDesc->Phi->getNumIncomingValues() != 2 ||
        !findIncoming(OuterDesc->Phi, Guard, OuterDesc->StartValue,
                      C.OuterStartIndex) ||
        !findIncoming(OuterDesc->Phi, OuterLatch,
                      OuterDesc->UpdateValue, OuterBackedgeIndex) ||
        !findIncoming(InnerDesc->Phi, Prefix,
                      InnerDesc->StartValue, C.InnerStartIndex) ||
        !findIncoming(InnerDesc->Phi, InnerBody,
                      InnerDesc->UpdateValue, C.InnerBackedgeIndex)) {
        return false;
    }

    auto availableAtGuard = [&](Value *V) {
        auto *I = dynamic_cast<Instruction *>(V);
        if (!I || !I->getParent()) return true;
        BasicBlock *DefBlock = I->getParent();
        if (DefBlock != Guard) return DT.dominates(DefBlock, Guard);
        Instruction *Terminator = Guard->getTerminator();
        return I != Terminator && I->comesBefore(Terminator);
    };

    // 交换会让旧内层条件先于旧外层头执行，因此无论 SCEV 是否推断零次
    // 迭代，都精确保留原外层的第一次首部比较。这里只接受直接使用头部
    // PHI 的比较，避免用数学 trip count 代替固定位宽 IR 的真实谓词。
    Value *CmpLHS = OuterTrip->ExitCmp->getOperand(0);
    Value *CmpRHS = OuterTrip->ExitCmp->getOperand(1);
    const bool PhiOnLHS = CmpLHS == OuterDesc->Phi;
    const bool PhiOnRHS = CmpRHS == OuterDesc->Phi;
    Value *FirstBound = PhiOnLHS ? CmpRHS : CmpLHS;
    if (OuterTrip->ComparePhase != LoopExitComparePhase::HeaderPhi ||
        OuterTrip->IndVar != OuterDesc->Phi ||
        !BinaryInst::isCompareOp(OuterTrip->ExitCmp->getOp()) ||
        PhiOnLHS == PhiOnRHS ||
        !availableAtGuard(OuterDesc->StartValue) ||
        !availableAtGuard(FirstBound)) {
        return false;
    }

    if (!availableAtGuard(OuterDesc->StartValue) ||
        !availableAtGuard(InnerDesc->StartValue) ||
        !DT.dominates(OuterHeader, Prefix) ||
        !DT.dominates(Prefix, InnerHeader) ||
        !DT.dominates(OuterHeader, InnerHeader) ||
        !DT.dominates(InnerHeader, InnerBody) ||
        !DT.dominates(InnerHeader, OuterLatch) ||
        !PDT.isReachableToExit(OuterHeader) ||
        !PDT.isReachableToExit(InnerHeader)) {
        return false;
    }

    // 交换后旧内层头会成为新外层头，其中的控制计算从“每个外轴一次”
    // 变为“每个内轴一次”。只接受无访存、不可观察且不会故障的控制指令，
    // 并验证所有跨块定义在保护块处已可用，避免 CFG 重排制造 use-before-def。
    for (Instruction *I : *InnerHeader) {
        if (!I || dynamic_cast<PhiInst *>(I) ||
            dynamic_cast<BrInst *>(I)) {
            continue;
        }
        if (!dynamic_cast<BinaryInst *>(I) &&
            !dynamic_cast<BitCastInst *>(I) &&
            !dynamic_cast<ZextInst *>(I) &&
            !dynamic_cast<SextInst *>(I) &&
            !dynamic_cast<TruncInst *>(I)) {
            return false;
        }
        if (dceInstructionMustBePreserved(I) ||
            instructionMayFaultOrProducePoison(I)) {
            return false;
        }
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            Value *Operand = I->getOperand(Index);
            if (Operand == OuterDesc->Phi) return false;
            auto *Def = dynamic_cast<Instruction *>(Operand);
            if (!Def || !Def->getParent()) continue;
            if (Def->getParent() == InnerHeader) {
                if (Def != InnerDesc->Phi && !Def->comesBefore(I))
                    return false;
                continue;
            }
            if (!DT.dominates(Def->getParent(), Guard))
                return false;
        }
    }

    // 物理交换会改变循环体的执行次序，动态分配、调用和未知可观察
    // 指令不能只依靠内存依赖摘要放行。
    for (BasicBlock *BB : Outer->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<AllocaInst *>(I) ||
                dynamic_cast<CallInst *>(I) ||
                dynamic_cast<RetInst *>(I)) {
                return false;
            }
            if (!dynamic_cast<LoadInst *>(I) &&
                !dynamic_cast<StoreInst *>(I) &&
                !dynamic_cast<PhiInst *>(I) &&
                !dynamic_cast<BrInst *>(I) &&
                dceInstructionMustBePreserved(I)) {
                return false;
            }
        }
    }

    std::vector<Instruction *> PrefixLoads;
    for (BasicBlock *BB : OuterDirectBlocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<CallInst *>(I) ||
                dynamic_cast<StoreInst *>(I) ||
                dynamic_cast<AllocaInst *>(I) ||
                dynamic_cast<RetInst *>(I)) {
                return false;
            }
            if (auto *Load = dynamic_cast<LoadInst *>(I)) {
                if (BB != Prefix) return false;
                PrefixLoads.push_back(Load);
                continue;
            }
            if (dynamic_cast<PhiInst *>(I) || dynamic_cast<BrInst *>(I))
                continue;
            if (dceInstructionMustBePreserved(I) ||
                instructionMayFaultOrProducePoison(I)) {
                return false;
            }
        }
    }
    if (PrefixLoads.empty() || LD.hasUnknownMemory(Outer)) return false;

    // Prefix 改为随新内层轴重复执行。它的定义只能供仍被 Prefix
    // 支配的块使用，且同块定义必须保持先定义后使用。
    for (Instruction *I : *Prefix) {
        if (!I || dynamic_cast<BrInst *>(I)) continue;
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            auto *Def = dynamic_cast<Instruction *>(I->getOperand(Index));
            if (!Def || !Def->getParent()) continue;
            if (Def->getParent() == Prefix) {
                if (!Def->comesBefore(I)) return false;
                continue;
            }
            if (Def == OuterDesc->Phi) continue;
            if (!DT.dominates(Def->getParent(), Guard)) return false;
        }
        for (Use *U : I->getUses()) {
            auto *User = dynamic_cast<Instruction *>(
                U ? U->getUser() : nullptr);
            if (!User || !User->getParent()) return false;
            if (User->getParent() == Prefix) {
                if (!I->comesBefore(User)) return false;
                continue;
            }
            if (User->getParent() != InnerBody &&
                User->getParent() != OuterLatch) {
                return false;
            }
        }
    }

    const auto &Accesses = LD.getMemoryAccesses(Outer);
    for (Instruction *Load : PrefixLoads) {
        for (const LoopMemoryAccess &Access : Accesses) {
            if (!Access.Writes || !Access.Inst ||
                !Access.Inst->getParent() ||
                !Inner->contains(Access.Inst->getParent())) {
                continue;
            }
            if (!LD.isInvariantAccessDisjointFromLoopWrite(
                    Load, Access.Inst, Outer, Inner)) {
                return false;
            }
        }
    }

    for (const LoopDependenceRecord &Record : LD.getDependences(Outer)) {
        if (!Record.Source || !Record.Sink ||
            !Record.Source->Inst || !Record.Sink->Inst) {
            return false;
        }

        bool OuterEqual = false;
        bool InnerEqual = false;
        for (std::size_t I = 0;
             I < Record.Loops.size() && I < Record.DirectionMasks.size();
             ++I) {
            if (Record.Loops[I] == Outer)
                OuterEqual =
                    Record.DirectionMasks[I] == LoopDepSameIteration;
            if (Record.Loops[I] == Inner)
                InnerEqual =
                    Record.DirectionMasks[I] == LoopDepSameIteration;
        }
        // Prefix 的区间不相交证明只说明固定旧外轴时重复读取稳定，
        // 不能代替跨旧外轴的标准方向向量门禁。
        if (!OuterEqual && !InnerEqual) return false;
    }

    long long Benefit = 0;
    for (const LoopMemoryAccess &Access : Accesses) {
        if (!Access.Inst || !Access.Inst->getParent() ||
            !Inner->contains(Access.Inst->getParent())) {
            continue;
        }
        auto OuterStride = LD.getByteStride(Access.Inst, Outer);
        auto InnerStride = LD.getByteStride(Access.Inst, Inner);
        if (!OuterStride || !InnerStride) continue;
        const long long Weight = Access.Writes ? 2 : 1;
        Benefit += Weight *
            (static_cast<long long>(stridePenalty(*InnerStride)) -
             static_cast<long long>(stridePenalty(*OuterStride)));
    }
    if (Benefit < MinLocalityBenefit) return false;

    C.Outer = Outer;
    C.Inner = Inner;
    C.Guard = Guard;
    C.OuterHeader = OuterHeader;
    C.Prefix = Prefix;
    C.InnerHeader = InnerHeader;
    C.InnerBody = InnerBody;
    C.OuterLatch = OuterLatch;
    C.Exit = Exit;
    C.OuterIV = OuterDesc->Phi;
    C.InnerIV = InnerDesc->Phi;
    C.OuterFirstCmp = OuterTrip->ExitCmp;
    C.OuterStartValue = OuterDesc->StartValue;
    C.OuterContinueOnTrue = OuterTrip->ContinueOnTrue;
    C.InnerContinueOnTrue = InnerTrip->ContinueOnTrue;
    C.LocalityBenefit = Benefit;
    return true;
}

bool LoopInterchangePass::applyRotatedNestCandidate(
    Function &F, RotatedNestCandidate &C) {
    if (!C.Guard || !C.OuterHeader || !C.Prefix || !C.InnerHeader ||
        !C.InnerBody || !C.OuterLatch || !C.Exit ||
        !C.OuterIV || !C.InnerIV) {
        return false;
    }

    if (!C.OuterFirstCmp || !C.OuterStartValue) {
        return false;
    }

    auto *GuardBr = C.Guard->getBranchTerminator();
    auto *OuterHeaderBr = C.OuterHeader->getBranchTerminator();
    auto *PrefixBr = C.Prefix->getBranchTerminator();
    auto *InnerHeaderBr = C.InnerHeader->getBranchTerminator();
    auto *InnerBodyBr = C.InnerBody->getBranchTerminator();
    auto *OuterLatchBr = C.OuterLatch->getBranchTerminator();
    auto findSuccessor = [](BrInst *Br, BasicBlock *Target,
                            unsigned &Index) {
        if (!Br || !Target) return false;
        bool Found = false;
        for (unsigned I = 0; I < Br->getSuccessorCount(); ++I) {
            if (Br->getSuccessor(I) != Target) continue;
            if (Found) return false;
            Found = true;
            Index = I;
        }
        return Found;
    };

    unsigned GuardToOuter = 0;
    unsigned OuterToPrefix = 0;
    unsigned OuterToExit = 0;
    unsigned PrefixToInner = 0;
    unsigned InnerBodyToHeader = 0;
    unsigned OuterLatchToHeader = 0;
    const unsigned OuterContinue =
        C.OuterContinueOnTrue ? 0U : 1U;
    const unsigned OuterExit = OuterContinue ^ 1U;
    unsigned InnerContinue = C.InnerContinueOnTrue ? 0U : 1U;
    unsigned InnerExit = InnerContinue ^ 1U;
    if (!findSuccessor(GuardBr, C.OuterHeader, GuardToOuter) ||
        !findSuccessor(OuterHeaderBr, C.Prefix, OuterToPrefix) ||
        !findSuccessor(OuterHeaderBr, C.Exit, OuterToExit) ||
        !findSuccessor(PrefixBr, C.InnerHeader, PrefixToInner) ||
        !findSuccessor(InnerBodyBr, C.InnerHeader,
                       InnerBodyToHeader) ||
        !findSuccessor(OuterLatchBr, C.OuterHeader,
                       OuterLatchToHeader) ||
        !InnerHeaderBr ||
        OuterToPrefix != OuterContinue || OuterToExit != OuterExit ||
        InnerHeaderBr->getSuccessor(InnerContinue) != C.InnerBody ||
        InnerHeaderBr->getSuccessor(InnerExit) != C.OuterLatch ||
        C.OuterIV->getIncomingBlock(C.OuterStartIndex) != C.Guard ||
        C.InnerIV->getIncomingBlock(C.InnerStartIndex) != C.Prefix ||
        C.InnerIV->getIncomingBlock(C.InnerBackedgeIndex) != C.InnerBody) {
        return false;
    }

    Value *FirstLHS = C.OuterFirstCmp->getOperand(0);
    Value *FirstRHS = C.OuterFirstCmp->getOperand(1);
    const bool PhiOnLHS = FirstLHS == C.OuterIV;
    const bool PhiOnRHS = FirstRHS == C.OuterIV;
    if (PhiOnLHS == PhiOnRHS) return false;
    if (PhiOnLHS) FirstLHS = C.OuterStartValue;
    else FirstRHS = C.OuterStartValue;

    auto *FirstCheck = new BasicBlock();
    auto *JPre = new BasicBlock();
    auto *KPre = new BasicBlock();
    auto *JLatch = new BasicBlock();
    JPre->setIdent(F.makeUniqueBlockIdent(
        C.InnerHeader->getIdent() + ".interchange.preheader"));
    KPre->setIdent(F.makeUniqueBlockIdent(
        C.OuterHeader->getIdent() + ".interchange.preheader"));
    JLatch->setIdent(F.makeUniqueBlockIdent(
        C.InnerHeader->getIdent() + ".interchange.latch"));
    FirstCheck->setIdent(F.makeUniqueBlockIdent(
        C.OuterHeader->getIdent() + ".interchange.firstcheck"));
    auto *FirstCmp = new BinaryInst(
        FirstLHS, C.OuterFirstCmp->getOp(), FirstRHS);
    FirstCheck->pushBack(FirstCmp);
    if (C.OuterContinueOnTrue)
        FirstCheck->genCondInst(FirstCmp, JPre, C.Exit);
    else
        FirstCheck->genCondInst(FirstCmp, C.Exit, JPre);
    JPre->genUnCondInst(C.InnerHeader);
    KPre->genUnCondInst(C.OuterHeader);
    JLatch->genUnCondInst(C.InnerHeader);

    // 所有可失败的结构与索引检查均在新块挂接前完成。
    // 提交阶段只替换已存在的边和 PHI 前驱，不重写起点、步长
    // 或上界，因此两个归纳变量的实际坐标不会偏移。
    F.pushBlock(FirstCheck);
    F.pushBlock(JPre);
    F.pushBlock(KPre);
    F.pushBlock(JLatch);

    auto rewriteEdge = [](BrInst *Br, unsigned Index,
                          BasicBlock *Target) {
        const bool Changed = Br && Br->setSuccessor(Index, Target);
        assert(Changed && "validated interchange edge rewrite failed");
        (void)Changed;
    };
    rewriteEdge(GuardBr, GuardToOuter, FirstCheck);
    rewriteEdge(InnerHeaderBr, InnerContinue, KPre);
    rewriteEdge(InnerHeaderBr, InnerExit, C.Exit);
    rewriteEdge(OuterHeaderBr, OuterExit, JLatch);
    rewriteEdge(PrefixBr, PrefixToInner, C.InnerBody);
    rewriteEdge(InnerBodyBr, InnerBodyToHeader, C.OuterLatch);

    const bool OuterIncomingChanged = C.OuterIV->setIncomingBlock(
        C.OuterStartIndex, KPre);
    const bool InnerStartChanged = C.InnerIV->setIncomingBlock(
        C.InnerStartIndex, JPre);
    const bool InnerBackedgeChanged = C.InnerIV->setIncomingBlock(
        C.InnerBackedgeIndex, JLatch);
    assert(OuterIncomingChanged && InnerStartChanged &&
           InnerBackedgeChanged &&
           "validated interchange PHI rewrite failed");
    (void)OuterIncomingChanged;
    (void)InnerStartChanged;
    (void)InnerBackedgeChanged;

    C.Prefix->setIdent(F.makeUniqueBlockIdent(
        C.Prefix->getIdent() + ".interchange.cfg"));
    C.InnerBody->setIdent(F.makeUniqueBlockIdent(
        C.InnerBody->getIdent() + ".interchange.cfg"));
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

        RotatedNestCandidate Rotated;
        if (buildRotatedNestCandidate(
                Outer, Inner, LI, SE, DT, PDT, LD, Rotated) &&
            applyRotatedNestCandidate(F, Rotated)) {
            Transformed.insert(Key);
            Transformed.insert(
                HeaderPair{Inner->getHeader(), Outer->getHeader()});
            ++TransformCount[&F];
            return PassResult::changed(PreservationStatus::none());
        }

        Candidate C;
        C.Outer = Outer;
        C.Inner = Inner;
        if (!sameIterationDomain(Outer, Inner, SE, C.OuterIV, C.InnerIV))
            continue;
        if (!hasOnlyCanonicalHeaderPhi(Outer, C.OuterIV) ||
            !hasOnlyCanonicalHeaderPhi(Inner, C.InnerIV))
            continue;
        if (hasUnsupportedDescendantDomain(Inner, SE))
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
