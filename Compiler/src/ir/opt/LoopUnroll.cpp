#include "ir/opt/LoopUnroll.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/IRUtils.hpp"
#include "lib/TraverseTools.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

bool LoopUnrollPass::isStructurallyLoopInvariant(
    Value *V, Loop *L, std::unordered_set<Value *> &Active) {
    // `undef` is represented in the constant hierarchy, but it is not a
    // repeatable value that can justify deleting later stores.  Reject it
    // before the generic constant case; recursive operand checks also reject
    // every pointer/value derived from undef.
    if (!V || !L || Value::isUndef(V)) return false;
    std::unordered_set<Value *> UndefActive;
    if (containsUndef(V, UndefActive, 0)) return false;
    if (dynamic_cast<ConstantSystem *>(V) ||
        dynamic_cast<Variable *>(V) ||
        dynamic_cast<Function *>(V) ||
        dynamic_cast<BuildInFunction *>(V)) {
        return true;
    }

    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || !L->contains(I->getParent())) return true;
    if (dynamic_cast<PhiInst *>(I) || dynamic_cast<LoadInst *>(I) ||
        dynamic_cast<CallInst *>(I) || dynamic_cast<AllocaInst *>(I)) {
        return false;
    }
    // Do not use an invariant expression that can be undefined for its
    // operands (division/remainder, FPTOSI, or an unproved shift) as evidence
    // that later stores repeat the first peeled value.  This keeps the memory
    // optimization independent of source-level undefined behaviour.
	if (instructionMayFaultOrProducePoison(I)) return false;

    bool PureProducer = dynamic_cast<GetElementPtrInst *>(I) ||
                        dynamic_cast<SelectInst *>(I) ||
                        dynamic_cast<BinaryInst *>(I) ||
                        I->instType == Instruction::TRUNC ||
                        I->instType == Instruction::ZEXT ||
                        I->instType == Instruction::SEXT ||
                        I->instType == Instruction::BITCAST;
    if (!PureProducer) return false;
    ScopedActiveMark ActiveMark(Active, V);
    if (!ActiveMark.inserted()) return false;

    const bool OperandsInvariant = forEachOperand(
        I, [L, &Active](size_t, Use *, Value *Operand) {
            if (dynamic_cast<BasicBlock *>(Operand)) return true;
            return isStructurallyLoopInvariant(Operand, L, Active);
        });
    return OperandsInvariant;
}

std::vector<StoreInst *>
LoopUnrollPass::collectPeelRedundantStoreCandidates(
    Loop *L, LoopInfo &LI, BasicAAResult &AA, const DominatorTree &DT) {
    std::vector<StoreInst *> Result;
    if (!L) return Result;

    BasicBlock *PreHeader = nullptr;
    BasicBlock *OuterLatch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, OuterLatch) ||
        !PreHeader || !OuterLatch) {
        return Result;
    }

    // If a candidate does not dominate the outer latch, its execution is
    // justified below by invariant direct-loop control.  That argument is
    // invalid when a nested-loop exit can reach the store during the same
    // outer iteration: different inner exits may skip/execute it.  Compute all
    // such downstream blocks once, without crossing the canonical outer
    // backedge.  Stores before a nested loop (the profitable common case)
    // remain eligible because the nested loop cannot reach backwards to them.
    std::vector<BasicBlock *> NestedRoots;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (BB && L->contains(BB) && LI.getLoopFor(BB) != L)
            NestedRoots.push_back(BB);
    }
    std::reverse(NestedRoots.begin(), NestedRoots.end());
    std::unordered_set<BasicBlock *> NestedControlledBlocks =
        collectReachableFromRootsCustomSucc<BasicBlock>(
            NestedRoots, [L, OuterLatch](BasicBlock *BB) {
                std::vector<BasicBlock *> Successors;
                if (!BB || !L->contains(BB)) return Successors;
                for (BasicBlock *Succ : BB->getSuccessors()) {
                    if (!Succ || !L->contains(Succ)) continue;
                    if (BB == OuterLatch && Succ == L->getHeader()) continue;
                    Successors.push_back(Succ);
                }
                std::reverse(Successors.begin(), Successors.end());
                return Successors;
            });

    // Every direct-loop control decision (apart from the loop exit itself)
    // must repeat across iterations.  Then a conditional store either runs
    // in both the peeled and residual iterations or in neither.
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB) return {};
        auto *Br = BB->getBranchTerminator();
        if (!Br || !Br->isConditional() || L->isExitingBlock(BB))
            continue;
        std::unordered_set<Value *> Active;
        if (!isStructurallyLoopInvariant(Br->getCondition(), L, Active))
            return {};
    }

    // Without a complete call ModRef proof here, calls conservatively block
    // the memory-state optimization (they do not block semantics-preserving
    // peeling itself).
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return {};
        for (Instruction *I : *BB)
            if (dynamic_cast<CallInst *>(I)) return {};
    }

    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB) return {};
        for (Instruction *I : *BB) {
            auto *SI = dynamic_cast<StoreInst *>(I);
            if (!SI) continue;

            // Either every completed peeled iteration executed this store, or
            // its conditional reachability is controlled solely by the
            // invariant direct-loop branches checked above.  Nested-loop
            // control is never guessed.
            if (!DT.dominates(BB, OuterLatch) &&
                NestedControlledBlocks.count(BB)) {
                continue;
            }

            std::unordered_set<Value *> Active;
            if (!isStructurallyLoopInvariant(
                    SI->getPointerOperand(), L, Active)) {
                continue;
            }
            Active.clear();
            if (!isStructurallyLoopInvariant(
                    SI->getValueOperand(), L, Active)) {
                continue;
            }

            bool HasCompetingMod = false;
            for (BasicBlock *OtherBB : L->getBlocksIncludingSubLoops()) {
                if (!OtherBB) {
                    HasCompetingMod = true;
                    break;
                }
                for (Instruction *OtherI : *OtherBB) {
                    auto *OtherStore = dynamic_cast<StoreInst *>(OtherI);
                    if (!OtherStore || OtherStore == SI) continue;
                    if (AA.alias(SI->getPointerOperand(),
                                 OtherStore->getPointerOperand(), LI) !=
                        AliasInfo::NoAlias) {
                        HasCompetingMod = true;
                        break;
                    }
                }
                if (HasCompetingMod) break;
            }
            if (!HasCompetingMod) Result.push_back(SI);
        }
    }
    return Result;
}

bool LoopUnrollPass::isDirectUnitIncrement(const InductionDescriptor &IV) {
    auto *Update = dynamic_cast<BinaryInst *>(IV.UpdateValue);
    if (!Update || Update->getOp() != BinaryInst::ADD || !IV.Phi)
        return false;
    Value *A = Update->getOperand(0);
    Value *B = Update->getOperand(1);
    auto *CA = dynamic_cast<ConstInt *>(A);
    auto *CB = dynamic_cast<ConstInt *>(B);
    return (A == IV.Phi && CB && CB->getVal() == 1) ||
           (B == IV.Phi && CA && CA->getVal() == 1);
}

// Return the invariant operand of a strict, signed, increasing header test
//     IV < Bound
// and reject every less precise shape.  In particular, comparing IV.next or
// using <= would need a different exit-value proof and is intentionally not
// inferred here.
Value *LoopUnrollPass::getStrictIncreasingHeaderBound(
    const LoopTripCountInfo &TCI, const InductionDescriptor &IV) {
    if (!TCI.Valid || !TCI.ExitCmp || !TCI.ExitingBlock || !TCI.Header ||
        !TCI.Latch || TCI.ExitingBlock != TCI.Header ||
        TCI.ExitingBlock == TCI.Latch || !IV.Valid || !IV.Phi ||
        !IV.ConstantStep || *IV.ConstantStep != 1) {
        return nullptr;
    }

    BinaryInst::Operation Pred = TCI.ContinueOnTrue
        ? TCI.ExitCmp->getOp()
        : BinaryInst::negatePredicate(TCI.ExitCmp->getOp());
    Value *LHS = TCI.ExitCmp->getOperand(0);
    Value *RHS = TCI.ExitCmp->getOperand(1);
    if (RHS == IV.Phi) {
        std::swap(LHS, RHS);
        Pred = BinaryInst::reversePredicate(Pred);
    }
    if (LHS != IV.Phi || Pred != BinaryInst::L) return nullptr;
    return RHS;
}

bool LoopUnrollPass::proveRuntimeTripCountAtPreheader(
    SCEVResult &SE, const LoopTripCountInfo &TCI, RangeInfo &RI,
    DominatorTree &DT) {
    return SE.proveRuntimeNoWrapAtPreheader(TCI, RI, DT);
}

bool LoopUnrollPass::isExactInnerExitIV(
    Value *V, const LoopTripCountInfo &TCI, PhiInst *IV) {
    if (!V || !IV) return false;
    if (V == IV) return true;

    auto *ExitPhi = dynamic_cast<PhiInst *>(V);
    if (!ExitPhi || ExitPhi->getParent() != TCI.ExitBlock ||
        ExitPhi->getNumIncomingValues() != 1) {
        return false;
    }
    return ExitPhi->getIncomingBlock(0) == TCI.ExitingBlock &&
           ExitPhi->getIncomingValue(0) == IV;
}

bool LoopUnrollPass::isValueAvailableBeforeOuterLoop(
    Value *V, Loop *Outer, BasicBlock *PreHeader, DominatorTree &DT) {
    if (!V || !Outer || !PreHeader) return false;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return true;
    BasicBlock *DefBB = I->getParent();
    if (!DefBB || Outer->contains(DefBB)) return false;
    return DT.dominates(DefBB, PreHeader);
}

bool LoopUnrollPass::containsUndef(
    Value *V, std::unordered_set<Value *> &Active, int Depth) {
    if (!V || Value::isUndef(V)) return true;
    if (Depth > 32) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return false;
    ScopedActiveMark ActiveMark(Active, V);
    if (!ActiveMark.inserted()) return true;
    const bool NoOperandContainsUndef = forEachOperand(
        I, [&Active, Depth](size_t, Use *, Value *Operand) {
            if (dynamic_cast<BasicBlock *>(Operand)) return true;
            return !containsUndef(Operand, Active, Depth + 1);
        });
    return !NoOperandContainsUndef;
}

bool LoopUnrollPass::isTrivialForwardOf(
    Value *Candidate, Value *Source,
    std::unordered_set<Value *> &Active, int Depth) {
    if (Candidate == Source) return true;
    if (!Candidate || !Source || Depth > 16) {
        return false;
    }
    ScopedActiveMark ActiveMark(Active, Candidate);
    if (!ActiveMark.inserted()) return false;

    bool Result = false;
    if (auto *Phi = dynamic_cast<PhiInst *>(Candidate)) {
        Result = Phi->getNumIncomingValues() != 0 &&
                 forEachPhiIncoming(
                     Phi, [Source, &Active, Depth](unsigned, Value *Incoming,
                                                   BasicBlock *) {
                         return isTrivialForwardOf(
                             Incoming, Source, Active, Depth + 1);
                     });
    } else if (auto *Sel = dynamic_cast<SelectInst *>(Candidate)) {
        Result = isTrivialForwardOf(
                     Sel->getTrueValue(), Source, Active, Depth + 1) &&
                 isTrivialForwardOf(
                     Sel->getFalseValue(), Source, Active, Depth + 1);
    }
    return Result;
}

std::pair<IntRange, IntRange> LoopUnrollPass::getFirstOuterEntryRanges(
    Value *Initial, Value *Bound, Loop *Outer, BasicBlock *OuterPreHeader,
    SCEVResult &SE, RangeInfo &RI, DominatorTree &DT) {
    IntRange InitialRange = RI.query(Initial, OuterPreHeader, DT);
    IntRange BoundRange = RI.query(Bound, OuterPreHeader, DT);

    // A dedicated preheader can sit immediately after a conditional guard.
    // Facts on that unique incoming edge are valid for the first peeled
    // iteration even though they need not hold at the loop header globally.
    auto Preds = OuterPreHeader->getPredecessors();
    if (Preds.size() == 1 && Preds.front() &&
        !Outer->contains(Preds.front())) {
        InitialRange = InitialRange.intersectWith(
            RI.queryOnEdge(Initial, Preds.front(), OuterPreHeader, DT));
        BoundRange = BoundRange.intersectWith(
            RI.queryOnEdge(Bound, Preds.front(), OuterPreHeader, DT));
    }

    // For a header-tested loop, only the continue edge reaches the peeled
    // body.  Its condition is therefore another exact first-iteration fact.
    auto OuterTCI = SE.analyzeLoopTripCount(Outer);
    if (OuterTCI && OuterTCI->Valid && OuterTCI->Latch &&
        OuterTCI->ExitingBlock == OuterTCI->Header &&
        OuterTCI->ExitingBlock != OuterTCI->Latch) {
        auto *Br = OuterTCI->Header->getBranchTerminator();
        BasicBlock *Continue = nullptr;
        if (Br && Br->isConditional()) {
            Continue = OuterTCI->ContinueOnTrue
                ? Br->getTrueSuccessor() : Br->getFalseSuccessor();
        }
        if (Continue && Outer->contains(Continue)) {
            InitialRange = InitialRange.intersectWith(
                RI.queryOnEdge(Initial, OuterTCI->Header, Continue, DT));
            BoundRange = BoundRange.intersectWith(
                RI.queryOnEdge(Bound, OuterTCI->Header, Continue, DT));
        }
    }
    return {InitialRange, BoundRange};
}

bool LoopUnrollPass::proveInitialNotAboveBound(
    Value *Initial, Value *Bound, Loop *Outer,
    BasicBlock *OuterPreHeader, SCEVResult &SE, RangeInfo &RI,
    DominatorTree &DT, IntRange &InitialRange, IntRange &BoundRange) {
    auto Ranges = getFirstOuterEntryRanges(
        Initial, Bound, Outer, OuterPreHeader, SE, RI, DT);
    InitialRange = Ranges.first;
    BoundRange = Ranges.second;
    if (!InitialRange.isBottom() && !BoundRange.isBottom() &&
        InitialRange.max <= BoundRange.min) {
        return true;
    }

    // Interval facts intentionally do not retain relations between two SSA
    // values.  Recover one exact first-iteration relation directly from a
    // canonical header guard: if the peeled body is reached through
    //     OuterIV < Bound
    // then Bound >= OuterStart + 1.  Prove Initial <= OuterStart + 1 using
    // independent ranges.  This is integer-order reasoning, not a no-wrap
    // assumption; OuterStart + 1 is also required to stay in i32.
    auto OuterTCI = SE.analyzeLoopTripCount(Outer);
    auto OuterIV = SE.getCanonicalInduction(Outer);
    if (!OuterTCI || !OuterTCI->Valid || !OuterIV || !OuterIV->Valid ||
        getStrictIncreasingHeaderBound(*OuterTCI, *OuterIV) != Bound ||
        !OuterIV->StartValue) {
        return false;
    }

    IntRange StartRange =
        RI.query(OuterIV->StartValue, OuterPreHeader, DT);
    if (InitialRange.isBottom() || StartRange.isBottom() ||
        StartRange.max >= IntRange::I32_MAX) {
        return false;
    }
    return InitialRange.max <= StartRange.min + 1;
}

// Keep the dynamic-bound profitability exception aligned with LICM's reusable
// call policy, and narrow it further to calls which execute on every completed
// outer iteration with loop-invariant arguments.
bool LoopUnrollPass::isKnownReadonlyInvariantCall(
    CallInst *CI, Loop *Outer, Loop *StableInner,
    BasicBlock *OuterLatch, DominatorTree &DT, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM) {
    BasicBlock *CallBB = CI ? CI->getParent() : nullptr;
    if (!CI || !Outer || !StableInner || !OuterLatch || !CallBB ||
        CallBB == Outer->getHeader() || !Outer->contains(CallBB) ||
        !DT.dominates(CallBB, OuterLatch) || CI->isTailCall() ||
        !CI->getIRType() || CI->getIRType()->getEnum() == IR_VOID ||
        !CI->hasUses()) {
        return false;
    }

    Function *Callee = CI->getDirectCallee();
    if (!Callee || !Callee->front()) return false;

    // The outer-loop clone budget sees a call as one instruction.  Bound the
    // hidden body as well, since the following inliner may duplicate this
    // helper into both the peeled and residual paths.
    constexpr std::size_t MaxReadonlyCalleeInstructions = 64;
    std::size_t CalleeInstructions = 0;
    for (BasicBlock *BB : *Callee) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I || ++CalleeInstructions > MaxReadonlyCalleeInstructions)
                return false;
        }
    }

    const SideEffectInfo &SEI =
        FAM.getResult<SideEffectAnalysis>(*Callee);
    if (SEI.hasBuiltin || !SEI.ModSet.empty()) return false;
    for (const auto &Entry : SEI.ptrMap) {
        if (Entry.second.untracked) return false;
    }

    ModRefInfo MRI = getFunctionModRefInfo(SEI);
    if (MRI != ModRefInfo::NoModRef && MRI != ModRefInfo::Ref)
        return false;

    for (Value *Arg : CI->getArgs()) {
        std::unordered_set<Value *> Active;
        if (!isStructurallyLoopInvariant(Arg, Outer, Active))
            return false;
    }

    // Predict LICM's residual-loop Mod/Ref check.  The stable inner loop is
    // deliberately excluded: the proof paired with this query establishes
    // that its original copy is zero-trip after peeling, and executePeeling
    // exposes that fact before LICM runs.  Any write which remains elsewhere
    // in the residual outer loop makes caching unprofitable and keeps the old
    // constant-bound restriction.
    std::vector<Value *> ReadPointers;
    for (Value *ReadValue : SEI.RefSet) {
        auto *Var = dynamic_cast<Variable *>(ReadValue);
        if (!Var) continue;
        if (Var->isGlobal()) {
            ReadPointers.push_back(ReadValue);
            continue;
        }
        if (!Var->isParam()) continue;

        int ParamIndex = Callee->getParamIndex(ReadValue);
        if (ParamIndex < 0) return false;
        Value *Arg = CI->getArg(static_cast<unsigned>(ParamIndex));
        if (!Arg || !Arg->isPointerValue()) return false;
        ReadPointers.push_back(Arg);
    }

    for (Value *Ptr : ReadPointers) {
        for (BasicBlock *BB : Outer->getBlocksIncludingSubLoops()) {
            if (!BB) return false;
            if (StableInner->contains(BB)) continue;
            for (Instruction *I : *BB) {
                if (!I || I == CI) continue;
                if (isModSet(getInstModRefInfo(Ptr, I, AA, LI, FAM)))
                    return false;
            }
        }
    }
    return true;
}

// Prove the one-shot nested-loop state pattern rather than merely matching
// its syntax:
//
//   P = phi [Initial, outer.preheader], [InnerExit, outer.latch]
//   J = phi [P, inner.preheader], [J + 1, inner.latch]
//   while (J < Bound) ...
//   InnerExit = J
//
// Bound must be available before and invariant across the outer loop, the
// inner exit must dominate the outer latch, and the induction update must be
// the exact integer operation J + 1.  The first iteration then leaves
// P == max(Initial, Bound): when Initial < Bound the monotone unit recurrence
// reaches Bound without wrapping; otherwise the header test is false and P
// remains Initial.  Every residual iteration therefore starts at a value
// greater than or equal to Bound and the inner loop is zero-trip.  If ranges
// already prove Initial <= Bound we retain the cheaper value Bound; otherwise
// execution materializes the exact max in the outer preheader.  The proof is
// based only on IR structure and applies independently of source/test names.
std::optional<ProvenStablePeelState>
LoopUnrollPass::findProvenStablePeelState(
    Loop *Outer, LoopInfo &LI, SCEVResult &SE, BasicAAResult &AA,
    RangeInfo &RI, DominatorTree &DT,
    FunctionAnalysisPassManager &FAM) {
    if (!Outer || Outer->isInnermost() || !Outer->getHeader())
        return std::nullopt;

    // If the sibling work is already visible as another direct subloop,
    // peeling would clone it without proving any benefit for that sibling.
    // Keep the enlarged stable-state budget for the single-child shape.  A
    // not-yet-inlined sibling is handled by the call/bound check below.
    if (Outer->getSubLoops().size() != 1)
        return std::nullopt;
    Loop *StableInner = Outer->getSubLoops().front();
    if (!StableInner) return std::nullopt;

    BasicBlock *OuterPreHeader = nullptr;
    BasicBlock *OuterLatch = nullptr;
    if (!Outer->getLoopSimplifyForm(OuterPreHeader, OuterLatch) ||
        !OuterPreHeader || !OuterLatch) {
        return std::nullopt;
    }

    // A sibling's work may still be represented as a call at this pre-inline
    // stage and turn into another full loop later.  Dynamic stable bounds are
    // profitable when every such call is a must-execute, argument-invariant,
    // known readonly call: peeling exposes the one-shot initialization and the
    // following LICM phase can lazily cache the call in the residual loop.
    // Unknown, writing, conditionally executed, or variant-argument calls keep
    // the former constant-bound restriction.  This is a profitability gate;
    // the peeling legality proof below is unchanged.
    bool ContainsDisqualifyingCall = false;
    std::size_t ReadonlyCallCount = 0;
    constexpr std::size_t MaxReadonlyCallsForDynamicStablePeel = 4;
    for (BasicBlock *BB : Outer->getBlocksIncludingSubLoops()) {
        if (!BB) return std::nullopt;
        for (Instruction *I : *BB) {
            auto *CI = dynamic_cast<CallInst *>(I);
            if (!CI) continue;
            ++ReadonlyCallCount;
            if (ReadonlyCallCount > MaxReadonlyCallsForDynamicStablePeel ||
                !isKnownReadonlyInvariantCall(
                    CI, Outer, StableInner, OuterLatch, DT, AA, LI,
                    FAM)) {
                ContainsDisqualifyingCall = true;
            }
        }
    }
    for (PhiInst *StatePhi : Outer->getHeader()->collectPhiNodes()) {
        if (!StatePhi || !StatePhi->getIRType() ||
            StatePhi->getIRType()->getEnum() != IR_INT) {
            continue;
        }
        Value *Initial = StatePhi->getIncomingFromPred(OuterPreHeader);
        Value *Backedge = StatePhi->getIncomingFromPred(OuterLatch);
        if (!Initial || !Backedge || Initial == Backedge) continue;
        for (Loop *Inner : Outer->getSubLoops()) {
            if (Inner != StableInner) continue;
            auto TCI = SE.analyzeLoopTripCount(Inner);
            auto IV = SE.getCanonicalInduction(Inner);
            std::unordered_set<Value *> ForwardActive;
            if (!TCI || !TCI->Valid || !IV || !IV->Valid ||
                !isTrivialForwardOf(
                    IV->StartValue, StatePhi, ForwardActive) ||
                !isDirectUnitIncrement(*IV)) {
                continue;
            }

            Value *Bound = getStrictIncreasingHeaderBound(*TCI, *IV);
            if (!Bound || !Bound->getIRType() ||
                Bound->getIRType()->getEnum() != IR_INT ||
                !isExactInnerExitIV(Backedge, *TCI, IV->Phi) ||
                !TCI->ExitBlock ||
                !DT.dominates(TCI->ExitBlock, OuterLatch) ||
                !isValueAvailableBeforeOuterLoop(
                    Bound, Outer, OuterPreHeader, DT)) {
                continue;
            }
            if (ContainsDisqualifyingCall &&
                !dynamic_cast<ConstInt *>(Bound))
                continue;

            std::unordered_set<Value *> Active;
            if (containsUndef(Initial, Active)) continue;
            Active.clear();
            if (containsUndef(Bound, Active)) continue;

            IntRange InitialRange;
            IntRange BoundRange;
            bool EntryOrderProven = proveInitialNotAboveBound(
                Initial, Bound, Outer, OuterPreHeader, SE, RI, DT,
                InitialRange, BoundRange);

            ProvenStablePeelState Result;
            Result.Phi = StatePhi;
            Result.InnerExitCmp = TCI->ExitCmp;
            Result.InnerContinueOnTrue = TCI->ContinueOnTrue;
            if (EntryOrderProven) {
                Result.StableValue = Bound;
            } else {
                Result.Initial = Initial;
                Result.Bound = Bound;
            }
            return Result;
        }
    }
    return std::nullopt;
}

void LoopUnrollPass::discardUncommittedPeelClones(PeeledBodyCore &Core) {
    std::vector<BasicBlock *> Blocks;
    std::unordered_set<BasicBlock *> Seen;
    for (const auto &Iteration : Core.IterBlocks)
        for (BasicBlock *BB : Iteration)
            if (BB && Seen.insert(BB).second) Blocks.push_back(BB);

    // First sever all SSA cross-links while every defining instruction is
    // alive, then clear all instruction lists (which removes block operands),
    // and only then destroy the block nodes.  The clones are unreachable until
    // the preheader commit, so no original instruction can legitimately use a
    // cloned definition at this point.
    for (BasicBlock *BB : Blocks) {
        for (Instruction *I : *BB) {
            if (!I || !I->getIRType() ||
                I->getIRType()->getEnum() == IR_VOID) {
                continue;
            }
            if (I->hasUses())
                I->replaceAllUsesWith(
                    UndefValue::NewUndefValue(I->getIRType()));
        }
    }
    for (BasicBlock *BB : Blocks) BB->clear();
    for (BasicBlock *BB : Blocks) delete BB;

    Core.IterBlocks.clear();
    Core.Headers.clear();
    Core.Latches.clear();
    Core.Exitings.clear();
    Core.ValueMaps.clear();
}

void LoopUnrollPass::discardUncommittedUnrollClones(
    UnrolledBodyCore &Core) {
    PeeledBodyCore Clones;
    if (Core.IterBlocks.size() > 1) {
        Clones.IterBlocks.assign(Core.IterBlocks.begin() + 1,
                                 Core.IterBlocks.end());
    }
    discardUncommittedPeelClones(Clones);

    Core.IterBlocks.clear();
    Core.Headers.clear();
    Core.Latches.clear();
    Core.Exitings.clear();
    Core.ValueMaps.clear();
}

int LoopUnrollPass::nextUnrollSerial() {
    static int Serial = 0;
    return ++Serial;
}

std::string LoopUnrollPass::makeUnrollBlockIdent(Function *F,
                                 const BasicBlock *BB,
                                 int Iter,
                                 int UnrollSerial) {
    std::string Base = BB ? BB->getIdent() : "bb";

    Base += ".unr";
    Base += std::to_string(UnrollSerial);
    Base += ".";
    Base += std::to_string(Iter);

    return F ? F->makeUniqueBlockIdent(Base) : Base;
}

std::string LoopUnrollPass::makePeelBlockIdent(Function *F,
                               const BasicBlock *BB,
                               int Iter,
                               int PeelSerial) {
    std::string Base = BB ? BB->getIdent() : "bb";

    Base += ".peel";
    Base += std::to_string(PeelSerial);
    Base += ".";
    Base += std::to_string(Iter);

    return F ? F->makeUniqueBlockIdent(Base) : Base;
}

bool LoopUnrollPass::isSupportedCloneInstruction(Instruction *I) {
    if (!I) return false;

    switch (I->instType) {
    case Instruction::RET:
    case Instruction::ALLOCA:
    case Instruction::BinaryUnknown:
        return false;
    default:
        return true;
    }
}

Value *LoopUnrollPass::stripSimpleCast(Value *V) {
    while (auto *I = dynamic_cast<Instruction *>(V)) {
        switch (I->instType) {
        case Instruction::TRUNC:
        case Instruction::ZEXT:
        case Instruction::SEXT:
        case Instruction::BITCAST:
            V = I->getUnaryOperand();
            continue;
        default:
            return V;
        }
    }
    return V;
}

std::optional<PeelPredicate> LoopUnrollPass::matchCompareWithIV(BinaryInst *Cmp, Value *IV) {
    Value *RawLHS = nullptr;
    Value *RawRHS = nullptr;
    if (!Cmp || !IV || !Cmp->getOperands(RawLHS, RawRHS))
        return std::nullopt;

    Value *LHS = stripSimpleCast(RawLHS);
    Value *RHS = stripSimpleCast(RawRHS);
    IV = stripSimpleCast(IV);

    if (LHS == IV) {
        if (auto C = RHS ? RHS->getIntConstant() : std::nullopt) {
            return PeelPredicate{Cmp->getOp(), *C};
        }
    }

    if (RHS == IV) {
        if (auto C = LHS ? LHS->getIntConstant() : std::nullopt) {
            return PeelPredicate{BinaryInst::reversePredicate(Cmp->getOp()), *C};
        }
    }

    return std::nullopt;
}

int LoopUnrollPass::peelCountForPredicate(const PeelPredicate &P) {
    switch (P.Op) {
    case BinaryInst::E:
        if (P.Constant == 0) return 1;
        return 0;
    case BinaryInst::L:
        if (P.Constant == 1) return 1;
        if (P.Constant == 2) return 2;
        return 0;
    case BinaryInst::LE:
        if (P.Constant == 0) return 1;
        if (P.Constant == 1) return 2;
        return 0;
    default:
        return 0;
    }
}

bool LoopUnrollPass::isInternalConditionalBranch(Loop *L, BrInst *Br) {
    if (!L || !Br || !Br->isConditional()) {
        return false;
    }

    auto *TrueBB = Br->getTrueSuccessor();
    auto *FalseBB = Br->getFalseSuccessor();
    return TrueBB && FalseBB && L->contains(TrueBB) && L->contains(FalseBB);
}

UnrollDecision LoopUnrollPass::makeNoUnrollDecision() {
    UnrollDecision Decision;
    Decision.Kind = UnrollKind::None;
    Decision.Count = 0;
    Decision.ShouldUnroll = false;
    Decision.NeedRemainder = false;
    Decision.IsRuntime = false;
    Decision.IsPeeling = false;
    return Decision;
}

bool LoopUnrollPass::withinThreshold(int64_t Cost, int Threshold) {
    return Threshold >= 0 && Cost <= static_cast<int64_t>(Threshold);
}

int LoopUnrollPass::chooseFactorBySize(int BodyCost, int SizeLimit, int MaxCount) {
    if (BodyCost <= 0 || SizeLimit <= 0 || MaxCount < 2)
        return 0;

    int Estimated = SizeLimit / BodyCost;
    int Factor = std::min(Estimated, MaxCount);

    if (Factor >= 4)
        return 4;
    if (Factor >= 2)
        return 2;
    return 0;
}

int LoopUnrollPass::localSCEVNodeCost(const SCEVExpr *E) {
    if (!E) return 1000000;

    switch (E->Kind) {
    case SCEVExprKind::Constant:
        return 0;
    case SCEVExprKind::Unknown:
        return 1;
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Cast:
    case SCEVExprKind::Compare:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max:
        return 1;
    case SCEVExprKind::Mul:
    case SCEVExprKind::AddRec:
        return 2;
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        return 4;
    }

    return 1000000;
}

std::optional<LoopExecutionShape>
LoopUnrollPass::getLoopExecutionShape(Loop *L, SCEVResult &SE) {
    if (!L) return std::nullopt;

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch)) return std::nullopt;

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->Valid) return std::nullopt;
    if (!TCI->Header || !TCI->ExitingBlock || !TCI->ExitBlock) {
        return std::nullopt;
    }

    LoopExecutionShape Shape;
    Shape.PreHeader = PreHeader;
    Shape.Header = TCI->Header;
    Shape.Latch = Latch;
    Shape.Exiting = TCI->ExitingBlock;
    Shape.Exit = TCI->ExitBlock;
    return Shape;
}

bool LoopUnrollPass::canCloneAllInstructions(const std::vector<BasicBlock *> &Blocks) {
    std::unordered_map<Value *, Value *> VMap;
    for (BasicBlock *BB : Blocks) {
        if (BB) VMap[BB] = BB;
    }

    for (BasicBlock *BB : Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!isSupportedCloneInstruction(I)) return false;
            Instruction *Probe = I->clone(VMap);
            if (!Probe) return false;
            delete Probe;
        }
    }

    return true;
}

Value *LoopUnrollPass::mapValueAtIteration(Value *V,
                           int Iter,
                           const UnrolledBodyCore &Core) {
    if (!V || Iter <= 0) return V;
    if (Iter >= static_cast<int>(Core.ValueMaps.size())) return V;

    auto It = Core.ValueMaps[Iter].find(V);
    return It == Core.ValueMaps[Iter].end() ? V : It->second;
}

bool LoopUnrollPass::isBlockValue(Value *V) {
    return dynamic_cast<BasicBlock *>(V) != nullptr;
}

bool LoopUnrollPass::isDataValue(Value *V) {
    return V && !isBlockValue(V) && V->getIRType();
}

bool LoopUnrollPass::isValueCompatibleWithPhi(Value *V, PhiInst *Phi) {
    if (!Phi || !isDataValue(V)) return false;
    auto Ty = Phi->getIRType();
    auto VTy = V->getIRType();
    if (!Ty || !VTy) return false;
    return Ty->getEnum() == VTy->getEnum() &&
           Ty->getSize() == VTy->getSize();
}

bool LoopUnrollPass::isValueCompatibleWithType(Value *V, std::shared_ptr<IRType> Ty) {
    if (!isDataValue(V) || !Ty) return false;
    auto VTy = V->getIRType();
    if (!VTy) return false;
    return Ty->getEnum() == VTy->getEnum() &&
           Ty->getSize() == VTy->getSize();
}

Value *LoopUnrollPass::mapDataValueAtIteration(Value *V,
                               int Iter,
                               const UnrolledBodyCore &Core) {
    if (!V) return nullptr;
    if (Iter > 0 && Iter < static_cast<int>(Core.ValueMaps.size())) {
        auto It = Core.ValueMaps[Iter].find(V);
        if (It != Core.ValueMaps[Iter].end()) {
            return isDataValue(It->second) ? It->second : nullptr;
        }
    }
    return isDataValue(V) ? V : nullptr;
}

Value *LoopUnrollPass::mapDataValueAtPeelIteration(Value *V,
                                   int Iter,
                                   const PeeledBodyCore &Core) {
    if (!V) return nullptr;
    if (Iter >= 0 && Iter < static_cast<int>(Core.ValueMaps.size())) {
        auto It = Core.ValueMaps[Iter].find(V);
        if (It != Core.ValueMaps[Iter].end()) {
            return isDataValue(It->second) ? It->second : nullptr;
        }
    }
    return isDataValue(V) ? V : nullptr;
}

Value *LoopUnrollPass::getCapturedDataValueMappedAtIteration(Value *V,
                                             int Iter,
                                             const UnrolledBodyCore &Core) {
    if (!V || Iter <= 0 ||
        Iter >= static_cast<int>(Core.ValueMaps.size())) {
        return nullptr;
    }

    auto It = Core.ValueMaps[Iter].find(V);
    if (It == Core.ValueMaps[Iter].end()) return nullptr;
    return isDataValue(It->second) ? It->second : nullptr;
}

bool LoopUnrollPass::addIncomingChecked(PhiInst *Phi, Value *V, BasicBlock *Pred) {
    if (!Phi || !Pred || !isValueCompatibleWithPhi(V, Phi)) {
        return false;
    }
    Phi->addIncoming(V, Pred);
    return true;
}

bool LoopUnrollPass::rebuildPhiFromIncomingChecked(
    PhiInst *Phi,
    const std::vector<PhiInst::IncomingEdge> &Incoming,
    Value **Replacement) {
    if (!Phi) return false;
    for (const auto &Edge : Incoming) {
        if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi)) {
            return false;
        }
    }

    Value *Result = nullptr;
    if (Incoming.empty()) {
        Result = UndefValue::NewUndefValue(Phi->getIRType());
        Phi->replaceAllUsesWith(Result);
        delete Phi;
    } else if (Incoming.size() == 1) {
        Result = Incoming.front().V;
        Phi->replaceAllUsesWith(Result);
        delete Phi;
    } else {
        auto *NewPhi = new PhiInst(Phi->getIRType());
        for (const auto &Edge : Incoming) {
            NewPhi->addIncoming(Edge.V, Edge.Pred);
        }
        Phi->replaceAllUsesWith(NewPhi);
        Phi->replaceWith(NewPhi);
        delete Phi;
        Result = NewPhi;
    }

    if (Replacement) *Replacement = Result;
    return true;
}

bool LoopUnrollPass::replaceDataOperandsUsingMap(
    Instruction *I,
    const std::unordered_map<Value *, Value *> &VMap) {
    if (!I) return false;

    forEachOperand(
        I, [&VMap](size_t, Use *U, Value *Old) {
        if (!U) return;
        if (isBlockValue(Old)) return;
        auto It = VMap.find(Old);
        if (It == VMap.end()) return;
        if (!isValueCompatibleWithType(It->second, Old->getIRType())) return;
        U->replaceUseeWith(It->second);
        });
    return true;
}

bool LoopUnrollPass::removeClonedHeaderPhis(UnrolledBodyCore &Core, int Iter) {
    if (Iter <= 0 || Iter >= Core.Count) return false;

    BasicBlock *OrigHeader = Core.Headers.front();
    BasicBlock *OrigLatch = Core.Latches.front();
    BasicBlock *ClonedHeader = Core.Headers[Iter];
    if (!OrigHeader || !OrigLatch || !ClonedHeader) return false;

    auto OrigPhis = OrigHeader->collectPhiNodes();
    auto ClonedPhis = ClonedHeader->collectPhiNodes();
    if (ClonedPhis.size() < OrigPhis.size()) return false;

    for (size_t I = 0; I < OrigPhis.size(); ++I) {
        PhiInst *OrigPhi = OrigPhis[I];
        PhiInst *ClonedPhi = ClonedPhis[I];
        if (!OrigPhi || !ClonedPhi) return false;

        Value *BackedgeValue = OrigPhi->getIncomingFromPred(OrigLatch);
        if (!BackedgeValue) return false;

        Value *Replacement =
            mapDataValueAtIteration(BackedgeValue, Iter - 1, Core);
        if (!Replacement) return false;

        ClonedPhi->replaceAllUsesWith(Replacement);
        Core.ValueMaps[Iter][OrigPhi] = Replacement;
        delete ClonedPhi;
    }

    return true;
}

bool LoopUnrollPass::removePeeledHeaderPhis(PeeledBodyCore &Core, int Iter) {
    if (Iter < 0 || Iter >= Core.PeelCount) return false;

    BasicBlock *OrigHeader = Core.Shape.Header;
    BasicBlock *OrigLatch = Core.Shape.Latch;
    BasicBlock *PreHeader = Core.Shape.PreHeader;
    BasicBlock *ClonedHeader = Core.Headers[Iter];
    if (!OrigHeader || !OrigLatch || !PreHeader || !ClonedHeader) {
        return false;
    }

    auto OrigPhis = OrigHeader->collectPhiNodes();
    auto ClonedPhis = ClonedHeader->collectPhiNodes();
    if (ClonedPhis.size() < OrigPhis.size()) return false;

    for (size_t I = 0; I < OrigPhis.size(); ++I) {
        PhiInst *OrigPhi = OrigPhis[I];
        PhiInst *ClonedPhi = ClonedPhis[I];
        if (!OrigPhi || !ClonedPhi) return false;

        Value *Replacement = nullptr;
        if (Iter == 0) {
            Replacement = OrigPhi->getIncomingFromPred(PreHeader);
        } else {
            Value *BackedgeValue = OrigPhi->getIncomingFromPred(OrigLatch);
            if (!BackedgeValue) return false;
            Replacement =
                mapDataValueAtPeelIteration(BackedgeValue, Iter - 1, Core);
        }

        if (!isValueCompatibleWithPhi(Replacement, ClonedPhi)) {
            return false;
        }

        ClonedPhi->replaceAllUsesWith(Replacement);
        Core.ValueMaps[Iter][OrigPhi] = Replacement;
        delete ClonedPhi;
    }

    return true;
}

std::optional<UnrolledBodyCore>
LoopUnrollPass::cloneUnrolledBodyCore(Loop *L, int Count, SCEVResult &SE) {
    if (!L || Count < 2 || !L->isInnermost()) return std::nullopt;

    auto Shape = getLoopExecutionShape(L, SE);
    if (!Shape || !Shape->Header || !Shape->Header->getParent()) {
        return std::nullopt;
    }

    std::vector<BasicBlock *> LoopBlocks = L->getBlocksInFunctionOrder();
    if (LoopBlocks.empty() || !canCloneAllInstructions(LoopBlocks)) {
        return std::nullopt;
    }

    Function *F = Shape->Header->getParent();
    int UnrollSerial = nextUnrollSerial();
    UnrolledBodyCore Core;
    Core.Shape = *Shape;
    Core.Count = Count;
    Core.IterBlocks.resize(Count);
    Core.IterBlocks[0] = LoopBlocks;
    Core.Headers.assign(Count, nullptr);
    Core.Latches.assign(Count, nullptr);
    Core.Exitings.assign(Count, nullptr);
    Core.ValueMaps.resize(Count);
    Core.Headers[0] = Shape->Header;
    Core.Latches[0] = Shape->Latch;
    Core.Exitings[0] = Shape->Exiting;
    auto Fail = [&]() -> std::optional<UnrolledBodyCore> {
        discardUncommittedUnrollClones(Core);
        return std::nullopt;
    };

    for (int Iter = 1; Iter < Count; ++Iter) {
        CloneBlockRegionOptions Options;
        Options.MakeBlockIdent =
            [F, Iter, UnrollSerial](const BasicBlock *BB, std::size_t) {
                return makeUnrollBlockIdent(F, BB, Iter, UnrollSerial);
            };
        auto Cloned = cloneBasicBlockRegion(*F, LoopBlocks, {}, Options);
        if (!Cloned) return Fail();

        Core.Headers[Iter] = Cloned->getClonedBlock(Shape->Header);
        Core.Latches[Iter] = Cloned->getClonedBlock(Shape->Latch);
        Core.Exitings[Iter] = Cloned->getClonedBlock(Shape->Exiting);
        Core.ValueMaps[Iter] = std::move(Cloned->VMap);
        Core.IterBlocks[Iter] = std::move(Cloned->ClonedBlocks);
        if (!Core.Headers[Iter] || !Core.Latches[Iter] || !Core.Exitings[Iter]) {
            return Fail();
        }

        if (!removeClonedHeaderPhis(Core, Iter)) {
            return Fail();
        }
    }

    return Core;
}

std::optional<PeeledBodyCore>
LoopUnrollPass::clonePeeledIterations(Loop *L, int PeelCount, SCEVResult &SE) {
    if (!L || PeelCount <= 0) return std::nullopt;

    auto Shape = getLoopExecutionShape(L, SE);
    if (!Shape || !Shape->Header || !Shape->PreHeader || !Shape->Latch ||
        !Shape->Exiting || !Shape->Exit || !Shape->Header->getParent()) {
        return std::nullopt;
    }

    std::vector<BasicBlock *> LoopBlocks = L->getBlocksInFunctionOrder();
    if (LoopBlocks.empty() || !canCloneAllInstructions(LoopBlocks)) {
        return std::nullopt;
    }

    Function *F = Shape->Header->getParent();
    int PeelSerial = nextUnrollSerial();
    PeeledBodyCore Core;
    Core.Shape = *Shape;
    Core.PeelCount = PeelCount;
    Core.IterBlocks.resize(PeelCount);
    Core.Headers.resize(PeelCount);
    Core.Latches.resize(PeelCount);
    Core.Exitings.resize(PeelCount);
    Core.ValueMaps.resize(PeelCount);
    auto Fail = [&]() -> std::optional<PeeledBodyCore> {
        discardUncommittedPeelClones(Core);
        return std::nullopt;
    };

    for (int Iter = 0; Iter < PeelCount; ++Iter) {
        CloneBlockRegionOptions Options;
        Options.MakeBlockIdent =
            [F, Iter, PeelSerial](const BasicBlock *BB, std::size_t) {
                return makePeelBlockIdent(F, BB, Iter, PeelSerial);
            };
        auto Cloned = cloneBasicBlockRegion(*F, LoopBlocks, {}, Options);
        if (!Cloned) return Fail();

        Core.Headers[Iter] = Cloned->getClonedBlock(Shape->Header);
        Core.Latches[Iter] = Cloned->getClonedBlock(Shape->Latch);
        Core.Exitings[Iter] = Cloned->getClonedBlock(Shape->Exiting);
        Core.ValueMaps[Iter] = std::move(Cloned->VMap);
        Core.IterBlocks[Iter] = std::move(Cloned->ClonedBlocks);
        if (!Core.Headers[Iter] || !Core.Latches[Iter] ||
            !Core.Exitings[Iter]) {
            return Fail();
        }

        if (!removePeeledHeaderPhis(Core, Iter)) {
            return Fail();
        }
    }

    return Core;
}

bool LoopUnrollPass::rewriteOriginalHeaderPhisForPartial(UnrolledBodyCore &Core,
                                         int LastIter,
                                         std::unordered_map<PhiInst *, Value *>
                                             *Replacements) {
    BasicBlock *Header = Core.Headers.front();
    BasicBlock *OrigLatch = Core.Latches.front();
    BasicBlock *LastLatch =
        LastIter >= 0 && LastIter < Core.Count ? Core.Latches[LastIter]
                                               : nullptr;
    if (!Header || !OrigLatch || !LastLatch || LastIter <= 0) return false;

    auto HeaderPhis = Header->collectPhiNodes();
    std::vector<PhiRewritePlan> Plans;
    Plans.reserve(HeaderPhis.size());

    for (PhiInst *Phi : HeaderPhis) {
        if (!Phi) continue;

        auto Incoming = Phi->collectIncomingEdges();
        PhiRewritePlan Plan;
        Plan.OldPhi = Phi;
        Plan.Incoming.reserve(Incoming.size());

        Value *BackedgeValue = nullptr;
        for (const auto &Edge : Incoming) {
            if (Edge.Pred == OrigLatch) {
                BackedgeValue = Edge.V;
                continue;
            }
            Plan.Incoming.push_back(Edge);
        }

        if (!BackedgeValue) return false;
        Value *MappedBackedge =
            mapDataValueAtIteration(BackedgeValue, LastIter, Core);
        if (!MappedBackedge) return false;

        Plan.Incoming.push_back({MappedBackedge, LastLatch});
        for (const auto &Edge : Plan.Incoming) {
            if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi)) {
                return false;
            }
        }
        Plans.push_back(std::move(Plan));
    }

    for (PhiRewritePlan &Plan : Plans) {
        if (!Plan.OldPhi) return false;
        if (Plan.Incoming.empty()) {
            Plan.Replacement =
                UndefValue::NewUndefValue(Plan.OldPhi->getIRType());
        } else if (Plan.Incoming.size() == 1) {
            Plan.Replacement = Plan.Incoming.front().V;
        } else {
            Plan.NewPhi = new PhiInst(Plan.OldPhi->getIRType());
            for (const auto &Edge : Plan.Incoming) {
                Plan.NewPhi->addIncoming(Edge.V, Edge.Pred);
            }
            Plan.Replacement = Plan.NewPhi;
        }
        if (!isDataValue(Plan.Replacement)) return false;
    }

    if (Replacements) {
        for (const PhiRewritePlan &Plan : Plans) {
            (*Replacements)[Plan.OldPhi] = Plan.Replacement;
        }
    }

    for (const PhiRewritePlan &Plan : Plans) {
        Plan.OldPhi->replaceAllUsesWith(Plan.Replacement);
    }
    for (const PhiRewritePlan &Plan : Plans) {
        if (Plan.NewPhi) {
            Plan.OldPhi->replaceWith(Plan.NewPhi);
        }
    }
    for (const PhiRewritePlan &Plan : Plans) {
        delete Plan.OldPhi;
    }

    return true;
}

bool LoopUnrollPass::rewriteOriginalHeaderPhisAfterPeeling(
    PeeledBodyCore &Core, bool ValidateOnly,
    std::unordered_map<PhiInst *, Value *> *Replacements) {
    BasicBlock *Header = Core.Shape.Header;
    BasicBlock *PreHeader = Core.Shape.PreHeader;
    BasicBlock *OrigLatch = Core.Shape.Latch;
    int LastIter = Core.PeelCount - 1;
    BasicBlock *LastPeeledLatch =
        LastIter >= 0 && LastIter < static_cast<int>(Core.Latches.size())
            ? Core.Latches[LastIter]
            : nullptr;

    if (!Header || !PreHeader || !OrigLatch || !LastPeeledLatch) {
        return false;
    }

    auto HeaderPhis = Header->collectPhiNodes();
    std::vector<PhiRewritePlan> Plans;
    Plans.reserve(HeaderPhis.size());
    for (PhiInst *Phi : HeaderPhis) {
        if (!Phi) continue;

        auto Incoming = Phi->collectIncomingEdges();
        PhiRewritePlan Plan;
        Plan.OldPhi = Phi;
        Plan.Incoming.reserve(Incoming.size());

        Value *BackedgeValue = nullptr;
        for (const auto &Edge : Incoming) {
            if (Edge.Pred == PreHeader) {
                continue;
            }

            if (Edge.Pred == OrigLatch) {
                BackedgeValue = Edge.V;
            }

            Plan.Incoming.push_back(Edge);
        }

        if (!BackedgeValue) return false;

        Value *NewInitial =
            mapDataValueAtPeelIteration(BackedgeValue, LastIter, Core);
        if (!isValueCompatibleWithPhi(NewInitial, Phi)) {
            return false;
        }

        Plan.Incoming.push_back({NewInitial, LastPeeledLatch});

        for (const auto &Edge : Plan.Incoming) {
            if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi))
                return false;
        }
        Plans.push_back(std::move(Plan));
    }

    if (ValidateOnly) return true;

    // Commit only after every PHI has been validated.  This avoids leaving a
    // partially rewritten loop header if a later PHI is malformed.
    for (PhiRewritePlan &Plan : Plans) {
        if (Plan.Incoming.empty()) {
            Plan.Replacement =
                UndefValue::NewUndefValue(Plan.OldPhi->getIRType());
        } else if (Plan.Incoming.size() == 1) {
            Plan.Replacement = Plan.Incoming.front().V;
        } else {
            Plan.NewPhi = new PhiInst(Plan.OldPhi->getIRType());
            for (const auto &Edge : Plan.Incoming)
                Plan.NewPhi->addIncoming(Edge.V, Edge.Pred);
            Plan.Replacement = Plan.NewPhi;
        }
    }
    for (PhiRewritePlan &Plan : Plans)
        Plan.OldPhi->replaceAllUsesWith(Plan.Replacement);
    if (Replacements) {
        for (const PhiRewritePlan &Plan : Plans)
            (*Replacements)[Plan.OldPhi] = Plan.Replacement;
    }
    for (PhiRewritePlan &Plan : Plans)
        if (Plan.NewPhi) Plan.OldPhi->replaceWith(Plan.NewPhi);
    for (PhiRewritePlan &Plan : Plans) delete Plan.OldPhi;

    return true;
}

bool LoopUnrollPass::removeIncomingFromExitPhisForPeeledBlocks(
    PeeledBodyCore &Core, bool ValidateOnly) {
    BasicBlock *Exit = Core.Shape.Exit;
    if (!Exit) return false;

    std::unordered_set<BasicBlock *> RemovedPreds;
    for (BasicBlock *BB : Core.Exitings) {
        if (BB) RemovedPreds.insert(BB);
    }

    std::vector<PhiRewritePlan> Plans;
    for (PhiInst *Phi : Exit->collectPhiNodes()) {
        if (!Phi) continue;

        PhiRewritePlan P;
        P.OldPhi = Phi;
        for (const auto &Edge : Phi->collectIncomingEdges()) {
            if (!RemovedPreds.count(Edge.Pred)) {
                if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi))
                    return false;
                P.Incoming.push_back(Edge);
            }
        }
        Plans.push_back(std::move(P));
    }

    if (ValidateOnly) return true;
    for (PhiRewritePlan &P : Plans)
        if (!rebuildPhiFromIncomingChecked(P.OldPhi, P.Incoming)) return false;

    return true;
}

bool LoopUnrollPass::addIncomingToExitPhisForPeeledBlocks(
    PeeledBodyCore &Core, bool ValidateOnly) {
    BasicBlock *Exit = Core.Shape.Exit;
    BasicBlock *OrigExiting = Core.Shape.Exiting;
    if (!Exit || !OrigExiting || Core.Exitings.size() !=
                                      static_cast<size_t>(Core.PeelCount)) {
        return false;
    }

    // Guarded peeling keeps each cloned exit edge live.  Mirror the original
    // exiting value through that iteration's value map before changing CFG;
    // otherwise a zero/short-trip path would reach Exit with a missing PHI
    // incoming edge.
    std::vector<PhiRewritePlan> Plans;
    for (PhiInst *Phi : Exit->collectPhiNodes()) {
        if (!Phi) return false;
        Value *OrigValue = Phi->getIncomingFromPred(OrigExiting);
        if (!OrigValue) return false;

        PhiRewritePlan P;
        P.OldPhi = Phi;
        P.Incoming = Phi->collectIncomingEdges();
        for (const auto &Edge : P.Incoming)
            if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi))
                return false;
        for (int Iter = 0; Iter < Core.PeelCount; ++Iter) {
            BasicBlock *Pred = Core.Exitings[Iter];
            Value *Mapped =
                mapDataValueAtPeelIteration(OrigValue, Iter, Core);
            if (!Pred || !isValueCompatibleWithPhi(Mapped, Phi))
                return false;
            P.Incoming.push_back({Mapped, Pred});
        }
        Plans.push_back(std::move(P));
    }

    if (ValidateOnly) return true;
    for (PhiRewritePlan &P : Plans)
        if (!rebuildPhiFromIncomingChecked(P.OldPhi, P.Incoming)) return false;
    return true;
}

bool LoopUnrollPass::hasLatchExitShape(const UnrolledBodyCore &Core) {
    return Core.Shape.Exiting && Core.Shape.Latch &&
           Core.Shape.Exiting == Core.Shape.Latch;
}

std::unordered_set<BasicBlock *>
LoopUnrollPass::collectCoreExitings(const UnrolledBodyCore &Core) {
    std::unordered_set<BasicBlock *> Result;
    for (BasicBlock *BB : Core.Exitings) {
        if (BB) Result.insert(BB);
    }
    return Result;
}

bool LoopUnrollPass::rewriteExitPhisForSingleExiting(UnrolledBodyCore &Core,
                                     int ActiveIter) {
    if (ActiveIter < 0 || ActiveIter >= Core.Count) return false;

    BasicBlock *Exit = Core.Shape.Exit;
    BasicBlock *OrigExiting = Core.Shape.Exiting;
    BasicBlock *ActiveExiting = Core.Exitings[ActiveIter];
    if (!Exit || !OrigExiting || !ActiveExiting) return false;

    auto RemovedPreds = collectCoreExitings(Core);
    auto ExitPhis = Exit->collectPhiNodes();
    for (PhiInst *Phi : ExitPhis) {
        if (!Phi) continue;

        Value *OrigValue = Phi->getIncomingFromPred(OrigExiting);
        Value *MappedValue =
            OrigValue ? mapDataValueAtIteration(OrigValue, ActiveIter, Core)
                      : UndefValue::NewUndefValue(Phi->getIRType());
        if (!MappedValue) return false;

        std::vector<PhiInst::IncomingEdge> Incoming;
        for (const auto &Edge : Phi->collectIncomingEdges()) {
            if (!RemovedPreds.count(Edge.Pred)) Incoming.push_back(Edge);
        }
        Incoming.push_back({MappedValue, ActiveExiting});
        if (!rebuildPhiFromIncomingChecked(Phi, Incoming)) return false;
    }
    return true;
}

std::vector<BasicBlock *> LoopUnrollPass::collectExitRegionBlocksFrom(
    BasicBlock *Exit,
    const std::unordered_set<BasicBlock *> &StopBlocks,
    bool StopAtConditionalBranch) {
    std::vector<BasicBlock *> Region;
    if (!Exit) return Region;

    std::vector<BasicBlock *> Roots{Exit};
    forEachReachablePreOrderCustomSucc<BasicBlock>(
        Roots,
        [&StopBlocks](BasicBlock *BB) {
            std::vector<BasicBlock *> Successors;
            if (!BB) return Successors;
            for (BasicBlock *Succ : BB->getSuccessors()) {
                if (!Succ || StopBlocks.count(Succ)) continue;
                Successors.push_back(Succ);
            }
            // The old explicit stack pushed successors in their natural order,
            // so LIFO visited the last successor first.
            std::reverse(Successors.begin(), Successors.end());
            return Successors;
        },
        [&Region, Exit, &StopBlocks,
         StopAtConditionalBranch](BasicBlock *BB) {
            if (BB != Exit && StopBlocks.count(BB))
                return TraversalControl::SkipSuccessors;

            Region.push_back(BB);
            if (StopAtConditionalBranch &&
                BasicBlock::isCondBr(BB->getBranchTerminator())) {
                return TraversalControl::SkipSuccessors;
            }
            return TraversalControl::Continue;
        });

    return Region;
}

std::vector<ExitRegionDataUse> LoopUnrollPass::collectExitRegionDataUsesFrom(
    BasicBlock *Exit,
    const std::unordered_set<BasicBlock *> &StopBlocks,
    BasicBlock *Exiting,
    bool StopAtConditionalBranch) {
    std::vector<ExitRegionDataUse> Uses;

    for (BasicBlock *BB : collectExitRegionBlocksFrom(
             Exit, StopBlocks, StopAtConditionalBranch)) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<PhiInst *>(I)) continue;
            forEachOperand(
                I, [Exit, Exiting, &Uses](size_t, Use *U, Value *Old) {
                if (!U) return;
                if (!isDataValue(Old)) return;
                Value *Lookup = Old;
                if (auto *Phi = dynamic_cast<PhiInst *>(Old)) {
                    if (Phi->getParent() == Exit && Exiting) {
                        if (Value *Incoming = Phi->getIncomingFromPred(Exiting)) {
                            Lookup = Incoming;
                        }
                    }
                }
                Uses.push_back({U, Old, Lookup, Old->getIRType()});
                });
        }
    }

    return Uses;
}

bool LoopUnrollPass::rewriteExitRegionDataOperandsForIteration(
    UnrolledBodyCore &Core,
    int ActiveIter,
    const std::vector<ExitRegionDataUse> &CapturedUses,
    const std::unordered_map<Value *, Value *> *HeaderExitValues) {
    if (ActiveIter <= 0 ||
        ActiveIter >= static_cast<int>(Core.ValueMaps.size())) {
        return false;
    }

    for (const ExitRegionDataUse &Entry : CapturedUses) {
        if (!Entry.OperandUse || !Entry.OriginalValue) continue;
        Value *Lookup = Entry.LookupValue ? Entry.LookupValue
                                          : Entry.OriginalValue;
        if (HeaderExitValues) {
            auto It = HeaderExitValues->find(Lookup);
            if (It != HeaderExitValues->end()) {
                if (isValueCompatibleWithType(It->second,
                                              Entry.ExpectedType)) {
                    Entry.OperandUse->replaceUseeWith(It->second);
                    continue;
                }
            }
        }
        Value *Mapped = getCapturedDataValueMappedAtIteration(
            Lookup, ActiveIter, Core);
        if (!Mapped) continue;
        if (!isValueCompatibleWithType(Mapped, Entry.ExpectedType)) continue;
        Entry.OperandUse->replaceUseeWith(Mapped);
    }

    return true;
}

bool LoopUnrollPass::replaceDataOperandsInReachableRegionFrom(
    BasicBlock *Start,
    const std::unordered_set<BasicBlock *> &StopBlocks,
    const std::unordered_map<Value *, Value *> &Replacements) {
    for (BasicBlock *BB :
         collectExitRegionBlocksFrom(Start, StopBlocks,
                                     /*StopAtConditionalBranch=*/false)) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (!replaceDataOperandsUsingMap(I, Replacements)) {
                return false;
            }
        }
    }
    return true;
}

void LoopUnrollPass::refreshCapturedExitRegionUses(
    std::vector<ExitRegionDataUse> &Uses,
    const std::unordered_map<Value *, Value *> &Replacements) {
    for (ExitRegionDataUse &Entry : Uses) {
        auto OrigIt = Replacements.find(Entry.OriginalValue);
        if (OrigIt != Replacements.end()) {
            Entry.OriginalValue = OrigIt->second;
        }

        auto LookupIt = Replacements.find(Entry.LookupValue);
        if (LookupIt != Replacements.end()) {
            Entry.LookupValue = LookupIt->second;
        }
    }
}

bool LoopUnrollPass::removeOriginalHeaderPhisForFull(UnrolledBodyCore &Core) {
    BasicBlock *Header = Core.Headers.front();
    BasicBlock *PreHeader = Core.Shape.PreHeader;
    if (!Header || !PreHeader) return false;

    auto HeaderPhis = Header->collectPhiNodes();
    for (PhiInst *Phi : HeaderPhis) {
        if (!Phi) continue;
        Value *InitialValue = Phi->getIncomingFromPred(PreHeader);
        if (!InitialValue) return false;
        Phi->replaceAllUsesWith(InitialValue);
        delete Phi;
    }

    return true;
}

bool LoopUnrollPass::makeStraightLineMain(UnrolledBodyCore &Core, int MainCount,
                          BasicBlock *FinalTarget, Value *ContinueCondition) {
    if (MainCount < 2 || MainCount > Core.Count || !FinalTarget) return false;
    if (!hasLatchExitShape(Core)) return false;

    for (int Iter = 0; Iter + 1 < MainCount; ++Iter) {
        if (!BasicBlock::replaceCondBrWithUncond(Core.Exitings[Iter],
                                                Core.Headers[Iter + 1])) {
            return false;
        }
    }

    BasicBlock *LastExiting = Core.Exitings[MainCount - 1];
    if (!LastExiting) return false;
    if (!ContinueCondition) {
        return BasicBlock::replaceCondBrWithUncond(LastExiting, FinalTarget);
    }

    return BasicBlock::replaceTerminator(
        LastExiting,
        new BrInst(ContinueCondition, Core.Headers.front(), FinalTarget));
}

Value *LoopUnrollPass::makeTypedIntConstant(int64_t V, std::shared_ptr<IRType> Ty) {
    if (V < std::numeric_limits<int>::min() ||
        V > std::numeric_limits<int>::max()) {
        return nullptr;
    }
    size_t Size = 4;
    if (Ty && Ty->getEnum() == IR_INT) Size = Ty->getSize();
    return ConstInt::newConstInt(static_cast<int>(V), Size);
}

bool LoopUnrollPass::isValueAvailableAtPreheader(Value *V, Loop *L, BasicBlock *PreHeader,
                                 const DominatorTree &DT) {
    if (!V || !L || !PreHeader) return false;
    if (V->isConst()) return true;
    if (auto *Var = dynamic_cast<Variable *>(V)) {
        return Var->isParam() || Var->isGlobal();
    }
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || L->contains(I->getParent())) return false;
    return I->getParent() == PreHeader ||
           DT.dominates(I->getParent(), PreHeader);
}

bool LoopUnrollPass::canMaterializeSCEV(const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
                        const DominatorTree &DT) {
    if (!E) return false;
    switch (E->Kind) {
    case SCEVExprKind::Constant:
        return E->ConstValue >= std::numeric_limits<int>::min() &&
               E->ConstValue <= std::numeric_limits<int>::max();
    case SCEVExprKind::Unknown:
        return isValueAvailableAtPreheader(E->RawValue, L, PreHeader, DT);
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Mul:
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max:
    case SCEVExprKind::Compare:
        return E->Operands.size() == 2 &&
               canMaterializeSCEV(E->Operands[0], L, PreHeader, DT) &&
               canMaterializeSCEV(E->Operands[1], L, PreHeader, DT);
    case SCEVExprKind::Cast:
        return E->Ty && E->Operands.size() == 1 &&
               canMaterializeSCEV(E->Operands[0], L, PreHeader, DT);
    case SCEVExprKind::AddRec:
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        return false;
    }
    return false;
}

Value *LoopUnrollPass::materializeSCEV(const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
                       const DominatorTree &DT,
                       std::unordered_map<const SCEVExpr *, Value *> &Cache) {
    if (!canMaterializeSCEV(E, L, PreHeader, DT)) return nullptr;
    auto Cached = Cache.find(E);
    if (Cached != Cache.end()) return Cached->second;

    Value *Result = nullptr;
    if (E->Kind == SCEVExprKind::Constant) {
        Result = makeTypedIntConstant(E->ConstValue, E->Ty);
    } else if (E->Kind == SCEVExprKind::Unknown) {
        Result = E->RawValue;
    } else if (E->Kind == SCEVExprKind::Cast) {
        Value *Op =
            materializeSCEV(E->Operands[0], L, PreHeader, DT, Cache);
        if (!Op) return nullptr;

        Instruction *Cast = nullptr;
        switch (E->CastKind) {
        case SCEVCastKind::Trunc:
            Cast = new TruncInst(E->Ty);
            Cast->addUse(Op);
            break;
        case SCEVCastKind::ZExt:
            Cast = new ZextInst(E->Ty);
            Cast->addUse(Op);
            break;
        case SCEVCastKind::SExt:
            Cast = new SextInst(E->Ty);
            Cast->addUse(Op);
            break;
        case SCEVCastKind::BitCast:
            Cast = new BitCastInst(Op, E->Ty);
            break;
        }
        if (!Cast) return nullptr;
        BasicBlock::insertBeforeTerminator(PreHeader, Cast);
        Result = Cast;
    } else {
        Value *LHS =
            materializeSCEV(E->Operands[0], L, PreHeader, DT, Cache);
        Value *RHS =
            materializeSCEV(E->Operands[1], L, PreHeader, DT, Cache);
        if (!LHS || !RHS) return nullptr;

        if (E->Kind == SCEVExprKind::Min ||
            E->Kind == SCEVExprKind::Max) {
            BinaryInst::Operation Pred =
                E->Kind == SCEVExprKind::Min ? BinaryInst::L
                                             : BinaryInst::G;
            auto *Cmp = new BinaryInst(LHS, Pred, RHS);
            BasicBlock::insertBeforeTerminator(PreHeader, Cmp);
            auto *Select = new SelectInst(Cmp, LHS, RHS);
            BasicBlock::insertBeforeTerminator(PreHeader, Select);
            Result = Select;
        } else {
            BinaryInst::Operation Op = BinaryInst::ADD;
            switch (E->Kind) {
            case SCEVExprKind::Add: Op = BinaryInst::ADD; break;
            case SCEVExprKind::Sub: Op = BinaryInst::SUB; break;
            case SCEVExprKind::Mul: Op = BinaryInst::MUL; break;
            case SCEVExprKind::Div: Op = BinaryInst::DIV; break;
            case SCEVExprKind::Mod: Op = BinaryInst::MOD; break;
            case SCEVExprKind::Compare: Op = E->Predicate; break;
            default: return nullptr;
            }
            auto *I = new BinaryInst(LHS, Op, RHS);
            BasicBlock::insertBeforeTerminator(PreHeader, I);
            Result = I;
        }
    }

    if (Result) Cache[E] = Result;
    return Result;
}

bool LoopUnrollPass::replaceValuesInIterationBlocks(
    UnrolledBodyCore &Core, int FirstIter,
    const std::unordered_map<Value *, Value *> &Replacements) {
    for (int Iter = FirstIter; Iter < Core.Count; ++Iter) {
        if (Iter < 0 || Iter >= static_cast<int>(Core.IterBlocks.size())) {
            return false;
        }
        for (BasicBlock *NewBB : Core.IterBlocks[Iter]) {
            if (!NewBB) continue;
            for (Instruction *I : *NewBB) {
                if (!replaceDataOperandsUsingMap(I, Replacements)) {
                    return false;
                }
            }
        }
    }
    return true;
}

Value *LoopUnrollPass::getValueBeforeLoop(Value *V, const UnrolledBodyCore &Core,
                          std::shared_ptr<IRType> FallbackTy) {
    if (!V) return UndefValue::NewUndefValue(std::move(FallbackTy));
    if (!isDataValue(V)) return nullptr;
    if (auto *Phi = dynamic_cast<PhiInst *>(V)) {
        if (Phi->getParent() == Core.Shape.Header) {
            Value *Initial = Phi->getIncomingFromPred(Core.Shape.PreHeader);
            if (isDataValue(Initial)) return Initial;
            return nullptr;
        }
    }
    if (auto *I = dynamic_cast<Instruction *>(V)) {
        if (I->getParent() && I->getParent() != Core.Shape.PreHeader) {
            for (const auto &MapEntry : Core.ValueMaps) {
                if (MapEntry.count(I)) {
                    return UndefValue::NewUndefValue(std::move(FallbackTy));
                }
            }
        }
    }
    return isDataValue(V) ? V : nullptr;
}

BasicBlock *LoopUnrollPass::getNonExitSuccessor(BasicBlock *Exiting, BasicBlock *Exit) {
    if (!Exiting || !Exit) return nullptr;
    for (BasicBlock *Succ : Exiting->getSuccessors()) {
        if (Succ && Succ != Exit) return Succ;
    }
    return nullptr;
}

Value *LoopUnrollPass::mapValueAfterIteration(Value *V, int Iter,
                              const std::vector<HeaderState> &States,
                              const UnrolledBodyCore &Core) {
    if (!isDataValue(V)) return nullptr;
    for (const HeaderState &State : States) {
        if (V == State.CurrentValue) {
            Value *Mapped = State.BackedgeIsOriginalPhi
                                ? State.CurrentValue
                                : mapDataValueAtIteration(
                                      State.BackedgeValue, Iter, Core);
            if (Iter >= State.TailStartIter && State.TailStateValue &&
                Mapped == State.CurrentValue) {
                return State.TailStateValue;
            }
            return Mapped;
        }
    }
    return mapDataValueAtIteration(V, Iter, Core);
}

bool LoopUnrollPass::buildHeaderExitValueMap(
    const std::vector<HeaderState> &HeaderStates,
    int ExitIter,
    const UnrolledBodyCore &Core,
    BasicBlock *PreHeader,
    std::unordered_map<Value *, Value *> &HeaderExitValues) {
    HeaderExitValues.clear();

    for (const HeaderState &State : HeaderStates) {
        Value *ExitValue = mapValueAfterIteration(State.CurrentValue,
                                                  ExitIter,
                                                  HeaderStates,
                                                  Core);
        if (!isDataValue(ExitValue)) return false;

        HeaderExitValues[State.CurrentValue] = ExitValue;
        HeaderExitValues[State.MainValue] = ExitValue;
        HeaderExitValues[State.BackedgeValue] = ExitValue;
        if (State.CurrentPhi) {
            for (const auto &Edge : State.CurrentPhi->collectIncomingEdges()) {
                if (Edge.Pred != PreHeader && isDataValue(Edge.V)) {
                    HeaderExitValues[Edge.V] = ExitValue;
                }
            }
        }
    }

    return true;
}

bool LoopUnrollPass::buildHeaderTailInputValueMap(
    const std::vector<HeaderState> &HeaderStates,
    BasicBlock *PreHeader,
    std::unordered_map<Value *, Value *> &TailInputValues) {
    TailInputValues.clear();

    for (const HeaderState &State : HeaderStates) {
        Value *InputValue = State.TailStateValue ? State.TailStateValue
                                                 : State.CurrentValue;
        if (!isDataValue(InputValue)) return false;

        TailInputValues[State.CurrentValue] = InputValue;
        TailInputValues[State.MainValue] = InputValue;
        TailInputValues[State.BackedgeValue] = InputValue;
        if (State.CurrentPhi) {
            for (const auto &Edge : State.CurrentPhi->collectIncomingEdges()) {
                if (Edge.Pred != PreHeader && isDataValue(Edge.V)) {
                    TailInputValues[Edge.V] = InputValue;
                }
            }
        }
    }

    return true;
}

LoopUnrollResult LoopUnrollPass::executeUnrollWithTail(
    Loop *L, int MainCount, int TailCopies, Value *NumGroups,
    Value *RemainderValue, bool IsRuntime, SCEVResult &SE) {
    if (!L || MainCount < 2 || TailCopies < 1 || !NumGroups) {
        return LoopUnrollResult::Unmodified;
    }

    auto PreCloneShape = getLoopExecutionShape(L, SE);
    if (!PreCloneShape || !PreCloneShape->Exit) {
        return LoopUnrollResult::Unmodified;
    }
    std::unordered_set<BasicBlock *> PreCloneLoopBlocks;
    for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
        if (BB) PreCloneLoopBlocks.insert(BB);
    }
    const bool PreCloneLatchExiting =
        PreCloneShape->Exiting && PreCloneShape->Latch &&
        PreCloneShape->Exiting == PreCloneShape->Latch;
    // 单块循环的 header、latch 与 exiting 是同一块，但它先执行循环体再
    // 沿回边继续，语义上必须归入 latch-tested。两类若同时置真，后续
    // CFG 会按 latch 连接，而 tail PHI 却会错误地按 header 选择前驱。
    const bool PreCloneHeaderExiting =
        !PreCloneLatchExiting && PreCloneShape->Exiting &&
        PreCloneShape->Header &&
        PreCloneShape->Exiting == PreCloneShape->Header;
    std::vector<ExitRegionDataUse> ExitRegionUses =
        collectExitRegionDataUsesFrom(PreCloneShape->Exit,
                                      PreCloneLoopBlocks,
                                      PreCloneShape->Exiting,
                                      PreCloneHeaderExiting);

    auto Core = cloneUnrolledBodyCore(L, MainCount + TailCopies, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }
    const bool LatchExiting = hasLatchExitShape(*Core);
    const bool HeaderExiting =
        !LatchExiting && Core->Shape.Exiting && Core->Shape.Header &&
        Core->Shape.Exiting == Core->Shape.Header;
    if (!LatchExiting && !HeaderExiting) {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    BasicBlock *Header = Core->Shape.Header;
    BasicBlock *PreHeader = Core->Shape.PreHeader;
    BasicBlock *MainLast = Core->Latches[MainCount - 1];
    BasicBlock *MainTailPred =
        HeaderExiting ? Header : Core->Exitings[MainCount - 1];
    BasicBlock *Exit = Core->Shape.Exit;
    Function *F = Header ? Header->getParent() : nullptr;
    if (!Header || !PreHeader || !MainLast || !MainTailPred || !Exit || !F) {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    std::vector<HeaderState> HeaderStates;
    for (PhiInst *Phi : Header->collectPhiNodes()) {
        Value *Initial = Phi->getIncomingFromPred(PreHeader);
        Value *Backedge = Phi->getIncomingFromPred(Core->Shape.Latch);
        Value *MainValue =
            mapDataValueAtIteration(Backedge, MainCount - 1, *Core);
        if (!isDataValue(Initial) || !isDataValue(Backedge) ||
            !isDataValue(MainValue)) {
            discardUncommittedUnrollClones(*Core);
            return LoopUnrollResult::Unmodified;
        }
        HeaderStates.push_back(
            {Phi, nullptr, nullptr, Initial, Backedge, MainValue, nullptr,
             std::numeric_limits<int>::max(), MainValue == Phi,
             Backedge == Phi});
    }

    std::unordered_map<PhiInst *, Value *> HeaderPhiReplacements;
    if (!rewriteOriginalHeaderPhisForPartial(*Core, MainCount - 1,
                                             &HeaderPhiReplacements)) {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    for (HeaderState &State : HeaderStates) {
        auto It = HeaderPhiReplacements.find(State.OriginalPhi);
        if (It == HeaderPhiReplacements.end() ||
            !isDataValue(It->second)) {
            return LoopUnrollResult::Unmodified;
        }
        State.CurrentValue = It->second;
        State.CurrentPhi = dynamic_cast<PhiInst *>(State.CurrentValue);
        if (State.MainValueIsOriginalPhi) {
            State.MainValue = State.CurrentValue;
        }
    }
    std::unordered_map<Value *, Value *> HeaderValueReplacements;
    for (const auto &Entry : HeaderPhiReplacements) {
        HeaderValueReplacements[Entry.first] = Entry.second;
    }
    if (HeaderExiting) {
        refreshCapturedExitRegionUses(ExitRegionUses,
                                      HeaderValueReplacements);
    }

    auto *TailEntry = new BasicBlock();
    std::string TailBase =
        Header->getIdent() +
        (IsRuntime ? ".unr.runtime.tail" : ".unr.constant.tail");
    TailEntry->setIdent(F->makeUniqueBlockIdent(TailBase));
    F->pushBlock(TailEntry);

    if (IsRuntime || HeaderExiting) {
        std::unordered_map<Value *, Value *> TailStateMap;
        for (HeaderState &State : HeaderStates) {
            Value *TailState = State.CurrentValue;
            if (IsRuntime) {
                auto *Merge = new PhiInst(State.InitialValue->getIRType());
                Value *MainIncoming =
                    HeaderExiting ? State.CurrentValue : State.MainValue;
                if (!addIncomingChecked(Merge, State.InitialValue,
                                        PreHeader) ||
                    !addIncomingChecked(Merge, MainIncoming,
                                        MainTailPred)) {
                    delete Merge;
                    return LoopUnrollResult::Unmodified;
                }
                TailEntry->pushBack(Merge);
                TailState = Merge;
            }
            if (!isDataValue(State.MainValue) || !isDataValue(TailState)) {
                return LoopUnrollResult::Unmodified;
            }
            State.TailStateValue = TailState;
            State.TailStartIter = MainCount;
            TailStateMap[State.MainValue] = TailState;
            if (HeaderExiting) {
                TailStateMap[State.CurrentValue] = TailState;
                TailStateMap[State.BackedgeValue] = TailState;
                if (State.CurrentPhi) {
                    for (const auto &Edge :
                         State.CurrentPhi->collectIncomingEdges()) {
                        if (Edge.Pred != PreHeader && isDataValue(Edge.V)) {
                            TailStateMap[Edge.V] = TailState;
                        }
                    }
                }
            }
        }
        if (!replaceValuesInIterationBlocks(*Core, MainCount, TailStateMap)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    auto *GroupPhi = new PhiInst(NumGroups->getIRType());
    Value *Zero = makeTypedIntConstant(0, NumGroups->getIRType());
    Value *One = makeTypedIntConstant(1, NumGroups->getIRType());
    if (!Zero || !One) {
        return LoopUnrollResult::Unmodified;
    }
    GroupPhi->addIncoming(Zero, PreHeader);
    Header->pushFront(GroupPhi);

    auto *NextGroup = new BinaryInst(GroupPhi, BinaryInst::ADD, One);
    BasicBlock::insertBeforeTerminator(MainLast, NextGroup);
    GroupPhi->addIncoming(NextGroup, MainLast);

    if (LatchExiting) {
        auto *MoreGroups =
            new BinaryInst(NextGroup, BinaryInst::L, NumGroups);
        BasicBlock::insertBeforeTerminator(MainLast, MoreGroups);
        if (!makeStraightLineMain(*Core, MainCount, TailEntry,
                                  MoreGroups)) {
            return LoopUnrollResult::Unmodified;
        }
    } else {
        BasicBlock *FirstBody = getNonExitSuccessor(Header, Exit);
        if (!FirstBody) {
            return LoopUnrollResult::Unmodified;
        }

        auto *HasGroup =
            new BinaryInst(GroupPhi, BinaryInst::L, NumGroups);
        BasicBlock::insertBeforeTerminator(Header, HasGroup);
        if (!BasicBlock::replaceTerminator(
                Header, new BrInst(HasGroup, FirstBody, TailEntry))) {
            return LoopUnrollResult::Unmodified;
        }

        for (int Iter = 1; Iter < MainCount; ++Iter) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Exitings[Iter], Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Body)) {
                return LoopUnrollResult::Unmodified;
            }
        }
        for (int Iter = 0; Iter + 1 < MainCount; ++Iter) {
            if (!BasicBlock::redirectBranchTarget(
                    Core->Latches[Iter], Core->Headers[Iter],
                    Core->Headers[Iter + 1])) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!BasicBlock::redirectBranchTarget(
                Core->Latches[MainCount - 1],
                Core->Headers[MainCount - 1], Header)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    for (int Tail = 0; Tail < TailCopies; ++Tail) {
        int Iter = MainCount + Tail;
        BasicBlock *TailExiting =
            LatchExiting ? Core->Exitings[Iter] : Core->Latches[Iter];
        if (!TailExiting) {
            return LoopUnrollResult::Unmodified;
        }

        if (HeaderExiting) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Exitings[Iter], Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Body)) {
                return LoopUnrollResult::Unmodified;
            }
        }

        if (!IsRuntime) {
            BasicBlock *Target =
                Tail + 1 < TailCopies ? Core->Headers[Iter + 1] : Exit;
            if (!BasicBlock::replaceCondBrWithUncond(TailExiting, Target)) {
                return LoopUnrollResult::Unmodified;
            }
            continue;
        }

        if (Tail + 1 == TailCopies) {
            if (!BasicBlock::replaceCondBrWithUncond(TailExiting, Exit)) {
                return LoopUnrollResult::Unmodified;
            }
            continue;
        }

        Value *Threshold =
            makeTypedIntConstant(Tail + 1, RemainderValue->getIRType());
        if (!Threshold) {
            return LoopUnrollResult::Unmodified;
        }
        auto *HasMore =
            new BinaryInst(RemainderValue, BinaryInst::G, Threshold);
        BasicBlock::insertBeforeTerminator(TailExiting, HasMore);
        if (!BasicBlock::replaceTerminator(TailExiting,
                           new BrInst(HasMore, Core->Headers[Iter + 1],
                                      Exit))) {
            return LoopUnrollResult::Unmodified;
        }
    }

    auto RemovedPreds = collectCoreExitings(*Core);
    auto ExitPhis = Exit->collectPhiNodes();
    for (PhiInst *Phi : ExitPhis) {
        Value *OrigValue = Phi->getIncomingFromPred(Core->Shape.Exiting);
        std::vector<PhiInst::IncomingEdge> Incoming;
        for (const auto &Edge : Phi->collectIncomingEdges()) {
            if (!RemovedPreds.count(Edge.Pred)) Incoming.push_back(Edge);
        }

        if (IsRuntime) {
            Value *InitialExit =
                getValueBeforeLoop(OrigValue, *Core, Phi->getIRType());
            Value *MainExit =
                OrigValue ? (HeaderExiting
                                 ? OrigValue
                                 : mapValueAfterIteration(
                                       OrigValue, MainCount - 1,
                                       HeaderStates, *Core))
                          : UndefValue::NewUndefValue(Phi->getIRType());
            if (!InitialExit || !MainExit) {
                return LoopUnrollResult::Unmodified;
            }
            auto *NoTailValue = new PhiInst(Phi->getIRType());
            if (!addIncomingChecked(NoTailValue, InitialExit, PreHeader) ||
                !addIncomingChecked(NoTailValue, MainExit, MainTailPred)) {
                delete NoTailValue;
                return LoopUnrollResult::Unmodified;
            }
            TailEntry->pushBack(NoTailValue);
            Incoming.push_back({NoTailValue, TailEntry});

            for (int Tail = 0; Tail < TailCopies; ++Tail) {
                int Iter = MainCount + Tail;
                Value *TailValue =
                    OrigValue ? mapValueAfterIteration(
                                    OrigValue, Iter, HeaderStates, *Core)
                              : UndefValue::NewUndefValue(Phi->getIRType());
                if (!TailValue) {
                    return LoopUnrollResult::Unmodified;
                }
                Incoming.push_back(
                    {TailValue, LatchExiting ? Core->Exitings[Iter]
                                             : Core->Latches[Iter]});
            }
        } else {
            int LastTailIter = MainCount + TailCopies - 1;
            Value *TailValue =
                OrigValue ? mapValueAfterIteration(
                                OrigValue, LastTailIter, HeaderStates, *Core)
                    : UndefValue::NewUndefValue(Phi->getIRType());
            if (!TailValue) {
                return LoopUnrollResult::Unmodified;
            }
            Incoming.push_back(
                {TailValue, LatchExiting ? Core->Exitings[LastTailIter]
                                         : Core->Latches[LastTailIter]});
        }

        if (!rebuildPhiFromIncomingChecked(Phi, Incoming)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    if (!IsRuntime) {
        int LastTailIter = MainCount + TailCopies - 1;
        std::unordered_map<Value *, Value *> HeaderExitValues;
        if (HeaderExiting) {
            if (!buildHeaderExitValueMap(HeaderStates, LastTailIter,
                                         *Core, PreHeader,
                                         HeaderExitValues)) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!rewriteExitRegionDataOperandsForIteration(
                *Core, LastTailIter, ExitRegionUses,
                HeaderExiting ? &HeaderExitValues : nullptr)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    if (IsRuntime) {
        if (!RemainderValue) {
            return LoopUnrollResult::Unmodified;
        }
        Value *ZeroRemainder =
            makeTypedIntConstant(0, RemainderValue->getIRType());
        if (!ZeroRemainder) {
            return LoopUnrollResult::Unmodified;
        }
        auto *HasRemainder =
            new BinaryInst(RemainderValue, BinaryInst::NE, ZeroRemainder);
        TailEntry->pushBack(HasRemainder);
        TailEntry->pushBack(
            new BrInst(HasRemainder, Core->Headers[MainCount], Exit));

        Value *ZeroGroups =
            makeTypedIntConstant(0, NumGroups->getIRType());
        if (!ZeroGroups) {
            return LoopUnrollResult::Unmodified;
        }
        auto *HasMain =
            new BinaryInst(NumGroups, BinaryInst::G, ZeroGroups);
        BasicBlock::insertBeforeTerminator(PreHeader, HasMain);
        if (!BasicBlock::replaceTerminator(
                PreHeader,
                new BrInst(HasMain, Core->Headers.front(), TailEntry))) {
            return LoopUnrollResult::Unmodified;
        }
    } else {
        TailEntry->pushBack(new BrInst(Core->Headers[MainCount]));
        if (HeaderExiting) {
            std::unordered_map<Value *, Value *> TailInputValues;
            if (!buildHeaderTailInputValueMap(HeaderStates, PreHeader,
                                              TailInputValues)) {
                return LoopUnrollResult::Unmodified;
            }
            auto TailScanStopBlocks = PreCloneLoopBlocks;
            TailScanStopBlocks.insert(Exit);
            if (!replaceDataOperandsInReachableRegionFrom(
                    TailEntry, TailScanStopBlocks, TailInputValues)) {
                return LoopUnrollResult::Unmodified;
            }
        }
    }

    SE.clear();
    return LoopUnrollResult::PartiallyUnrolled;
}

LoopUnrollPass::LoopUnrollPass(UnrollConfig ConfigValue)
    : Config(std::move(ConfigValue)) {}

const UnrollConfig &LoopUnrollPass::getConfig() const {
    return Config;
}

UnrollConfig LoopUnrollPass::makePeelingOnlyConfig(UnrollConfig Config) {
    Config.AllowFull = false;
    Config.AllowPartial = false;
    Config.AllowRuntime = false;
    Config.AllowRemainder = false;
    Config.AllowPeeling = true;
    return Config;
}

LoopPeelingPass::LoopPeelingPass(UnrollConfig Config)
    : Impl(LoopUnrollPass::makePeelingOnlyConfig(std::move(Config))) {}

PreservationStatus LoopPeelingPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    return Impl.run(F, FAM);
}

PreservationStatus LoopUnrollPass::run(Function &F,
                                       FunctionAnalysisPassManager &FAM) {
    auto &AlreadyTransformed = TransformedLoopHeaders[&F];
    // Capture only the loops present at pass entry.  A successful original
    // header is remembered across invocations of this pass object, preventing
    // an enclosing fixed point from multiplying one partial unroll again
    // (4x -> 16x -> 32x).  Newly-created residual/remainder headers remain
    // eligible in a later bounded round.
    std::vector<BasicBlock *> CandidateHeaders;
    {
        auto &InitialLI = FAM.getResult<LoopAnalysis>(F);
        for (Loop *L : InitialLI.getLoopsInPostOrder(true)) {
            if (L && L->getHeader() &&
                AlreadyTransformed.count(L->getHeader()->getIdent()) == 0)
                CandidateHeaders.push_back(L->getHeader());
        }
    }

    if (CandidateHeaders.empty()) return PreservationStatus::all();

    // Keep whole-function code growth and analysis rebuild cost bounded.  A
    // A function may contain dozens of individually profitable small loops;
    // unrolling all of them in one pass can multiply compile time and IR
    // size.  Four successful transforms per
    // invocation still give broader coverage than the old one-loop limit,
    // while the enclosing three-round pipeline retains a hard global budget.
    static constexpr size_t MaxTransformsPerRun = 4;
    size_t TransformCount = 0;
    bool Changed = false;
    for (BasicBlock *Header : CandidateHeaders) {
        // An earlier full unroll of a containing loop may have erased this
        // candidate.  Check membership using pointer identity only; never
        // dereference a header until it is found in the live function list.
        bool HeaderIsLive = false;
        for (BasicBlock *BB : F) {
            if (BB == Header) {
                HeaderIsLive = true;
                break;
            }
        }
        if (!HeaderIsLive) continue;

        auto &LI = FAM.getResult<LoopAnalysis>(F);
        Loop *L = LI.getLoopFor(Header);
        if (!L || L->getHeader() != Header) continue;

        auto &DT = FAM.getResult<DominantAnalysis>(F);
        auto &SE = FAM.getResult<SCEVAnalysis>(F);
        auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
        auto &RI = FAM.getResult<RangeAnalysis>(F);
        const std::string HeaderIdent = Header->getIdent();
        LoopUnrollResult Result =
            tryToUnrollLoop(F, L, LI, DT, SE, AA, RI, FAM);
        if (Result == LoopUnrollResult::Unmodified) continue;

        AlreadyTransformed.insert(HeaderIdent);
        Changed = true;
        ++TransformCount;
        SE.clear();

        // The transform invalidates every Loop*/DT/SCEV reference above.
        // Rebuild them before considering the next entry-snapshot loop.
        // Cloning and rewiring CFG/memory invalidates every cached
        // function analysis, including Range, BasicAA and MemorySSA.  Keeping
        // any of them here would let the next candidate consume pre-clone
        // facts.
        FAM.invalidate(F, PreservationStatus::none());

        if (TransformCount >= MaxTransformsPerRun) break;
    }

    return Changed ? PreservationStatus::none()
                   : PreservationStatus::all();
}

LoopUnrollResult LoopUnrollPass::tryToUnrollLoop(Function &F,
                                                 Loop *L,
                                                 LoopInfo &LI,
                                                 DominatorTree &DT,
                                                 SCEVResult &SE,
                                                 BasicAAResult &AA,
                                                 RangeInfo &RI,
                                                 FunctionAnalysisPassManager &FAM) {
    (void)F;

    if (!L) {
        return LoopUnrollResult::Unmodified;
    }

    if (!isCanonicalLoop(L)) {
        return LoopUnrollResult::Unmodified;
    }

    auto TripInfo = SE.analyzeLoopTripCount(L);
    if (!TripInfo || !TripInfo->Valid) {
        return LoopUnrollResult::Unmodified;
    }

    if (!canCloneLoopBody(L, LI)) {
        return LoopUnrollResult::Unmodified;
    }

    int BodyCost = estimateLoopBodyCost(L, LI);
    UnrollDecision Decision =
        computeUnrollDecision(
            L, *TripInfo, BodyCost, Config, LI, SE, AA, RI, DT, FAM);

    if (!Decision.ShouldUnroll || Decision.Kind == UnrollKind::None) {
        return LoopUnrollResult::Unmodified;
    }


    switch (Decision.Kind) {
    case UnrollKind::Full:
        return executeFullUnroll(L, Decision, SE, LI, DT);
    case UnrollKind::Partial:
        return executePartialUnroll(L, Decision, SE, LI, DT);
    case UnrollKind::PartialWithRemainder:
        return executePartialWithRemainder(L, Decision, SE, LI, DT);
    case UnrollKind::Runtime:
        return executeRuntimeUnroll(L, Decision, SE, LI, DT);
    case UnrollKind::Peeling:
        return executePeeling(L, Decision, SE, LI, DT, AA);
    case UnrollKind::None:
        break;
    }

    return LoopUnrollResult::Unmodified;
}

bool LoopUnrollPass::isCanonicalLoop(Loop *L) const {
    if (!L || !L->getHeader()) {
        return false;
    }

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) || !PreHeader || !Latch) {
        return false;
    }

    if (!LCSSAPass::isLCSSAForm(L)) {
        return false;
    }

    std::vector<BasicBlock *> ExitBlocks = L->getExitBlocks();
    if (ExitBlocks.size() != 1 || !ExitBlocks.front()) {
        return false;
    }

    std::unordered_set<BasicBlock *> ExitingBlocks;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (L->isExitingBlock(BB)) {
            ExitingBlocks.insert(BB);
        }
    }

    return ExitingBlocks.size() == 1;
}

bool LoopUnrollPass::canCloneLoopBody(Loop *L, LoopInfo &LI) const {
    (void)LI;
    if (!L) return false;

    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (auto It = BB->begin(); It != BB->end(); ++It) {
            if (!isSupportedCloneInstruction(*It)) {
                return false;
            }
        }
    }

    return true;
}

int LoopUnrollPass::estimateLoopBodyCost(Loop *L, LoopInfo &LI) const {
    if (!L) return 0;

    int Cost = 0;
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB || LI.getLoopFor(BB) != L) continue;

        for (auto It = BB->begin(); It != BB->end(); ++It) {
            Instruction *I = *It;
            if (!I) continue;

            switch (I->instType) {
            case Instruction::PHI:
                break;
            case Instruction::ADD:
            case Instruction::SUB:
            case Instruction::FADD:
            case Instruction::FSUB:
            case Instruction::ICMP:
            case Instruction::FCMP:
            case Instruction::TRUNC:
            case Instruction::ZEXT:
            case Instruction::SEXT:
            case Instruction::FPTOSI:
            case Instruction::SITOFP:
            case Instruction::BITCAST:
            case Instruction::AND:
            case Instruction::OR:
            case Instruction::XOR:
            case Instruction::SHL:
            case Instruction::LSHR:
            case Instruction::ASHR:
            case Instruction::SELECT:
                Cost += 1;
                break;
            case Instruction::MUL:
            case Instruction::FMUL:
                Cost += 3;
                break;
            case Instruction::SDIV:
            case Instruction::UDIV:
            case Instruction::SREM:
            case Instruction::UREM:
                Cost += 10;
                break;
            case Instruction::FDIV:
            case Instruction::FREM:
                Cost += 10;
                break;
            case Instruction::LOAD:
            case Instruction::STORE:
            case Instruction::GEP:
                Cost += 2;
                break;
            case Instruction::CALL:
                Cost += 20;
                break;
            case Instruction::BR:
            case Instruction::RET:
                Cost += 1;
                break;
            default:
                Cost += 1;
                break;
            }
        }
    }

    return Cost;
}

bool LoopUnrollPass::containsCall(Loop *L, LoopInfo &LI) const {
    (void)LI;
    if (!L) return false;

    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) continue;
        for (auto It = BB->begin(); It != BB->end(); ++It) {
            Instruction *I = *It;
            if (I && I->instType == Instruction::CALL) {
                return true;
            }
        }
    }

    return false;
}

int LoopUnrollPass::estimateSCEVCost(const SCEVExpr *E) const {
    if (!E) return 1000000;

    int Cost = localSCEVNodeCost(E);
    for (const SCEVExpr *Op : E->Operands) {
        Cost += estimateSCEVCost(Op);
    }
    return Cost;
}

bool LoopUnrollPass::isCheapSCEV(const SCEVExpr *E,
                                 const UnrollConfig &Config) const {
    if (!E) return false;
    if (Config.AllowExpensiveTripCount) return true;

    int Nodes = 0;
    int Cost = 0;
    std::vector<const SCEVExpr *> Stack;
    Stack.push_back(E);

    while (!Stack.empty()) {
        const SCEVExpr *Cur = Stack.back();
        Stack.pop_back();
        if (!Cur) return false;

        ++Nodes;
        Cost += localSCEVNodeCost(Cur);
        if (Nodes > Config.SCEVExpansionBudget ||
            Cost > Config.SCEVExpansionBudget) {
            return false;
        }

        for (const SCEVExpr *Op : Cur->Operands) {
            Stack.push_back(Op);
        }
    }

    return true;
}

int LoopUnrollPass::detectPeelCount(Loop *L,
                                    const InductionDescriptor &IV,
                                    const UnrollConfig &Config) const {
    if (!L || !IV.Valid || !IV.Phi) {
        return 0;
    }

    if (!IV.ConstantStart || !IV.ConstantStep ||
        *IV.ConstantStart != 0 || *IV.ConstantStep != 1) {
        return 0;
    }

    int BestPeelCount = 0;
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB) continue;
        if (BB->size() == 0) continue;
        auto *Br = dynamic_cast<BrInst *>(BB->back());
        if (!isInternalConditionalBranch(L, Br)) {
            continue;
        }

        auto *Cmp = dynamic_cast<BinaryInst *>(Br->getOperand(0));
        auto Match = matchCompareWithIV(Cmp, IV.Phi);
        if (!Match) {
            continue;
        }

        BestPeelCount = std::max(BestPeelCount, peelCountForPredicate(*Match));
        if (BestPeelCount >= Config.MaxPeelCount) {
            return Config.MaxPeelCount;
        }
    }

    return std::min(BestPeelCount, Config.MaxPeelCount);
}

UnrollDecision LoopUnrollPass::computeUnrollDecision(
    Loop *L,
    const LoopTripCountInfo &TCI,
    int BodyCost,
    const UnrollConfig &Config,
    LoopInfo &LI,
    SCEVResult &SE,
    BasicAAResult &AA,
    RangeInfo &RI,
    DominatorTree &DT,
    FunctionAnalysisPassManager &FAM) const {
    if (!L || !TCI.Valid || BodyCost <= 0) {
        return makeNoUnrollDecision();
    }

    bool HasCall = containsCall(L, LI);
    bool Nested = !L->isInnermost();

    if (Config.AllowFull && TCI.ProvenNoWrap && TCI.ConstantTripCount) {
        int64_t TripCount = *TCI.ConstantTripCount;
        int64_t MaxCount = (BodyCost <= 4)
                               ? Config.TinyLoopFullUnrollMaxCount
                               : Config.FullUnrollMaxCount;
        int64_t TotalCost = static_cast<int64_t>(BodyCost) * TripCount;
        bool Restricted = HasCall || Nested;
        int FullLimit = Restricted ? Config.FullThreshold / 2
                                   : Config.FullThreshold;

        if (TripCount > 1 && TripCount <= MaxCount &&
            (!Restricted || TripCount <= 4) &&
            withinThreshold(TotalCost, FullLimit)) {
            UnrollDecision Decision;
            Decision.Kind = UnrollKind::Full;
            Decision.Count = static_cast<int>(TripCount);
            Decision.ShouldUnroll = true;
            Decision.NeedRemainder = false;
            Decision.IsRuntime = false;
            Decision.IsPeeling = false;
            return Decision;
        }
    }

    if (Config.AllowPeeling) {
        auto IV = SE.getCanonicalInduction(L);
        int PeelCount =
            (IV && IV->Valid) ? detectPeelCount(L, *IV, Config) : 0;
        auto StableState = findProvenStablePeelState(
            L, LI, SE, AA, RI, DT, FAM);
        // The fixed-point proof establishes everything after exactly the
        // first iteration.  Never let its enlarged clone budget justify a
        // second peeled copy.
        if (StableState) PeelCount = 1;
        const int InvariantStoreCount = static_cast<int>(
            collectPeelRedundantStoreCandidates(L, LI, AA, DT).size());
        // Repeated loop-invariant stores provide a structural dynamic-benefit
        // signal: peeling establishes their first-iteration memory state and
        // exposes the residual repetitions to memory dependence cleanup.  A
        // high count justifies a larger, still hard-bounded clone budget.
        // This is based solely on generic IR properties, never on identifiers
        // or expected inputs.
        const bool HighStoreBenefit = InvariantStoreCount >= 8;
        if (PeelCount == 0 && HighStoreBenefit) PeelCount = 1;
        int EffectiveThreshold = Config.PeelThreshold;
        int EffectiveBlockLimit = Config.MaxPeelBlocks;
        if (StableState) {
            // The residual nested loop is proven zero-trip after the peeled
            // iteration, so permit enough room to clone the one-time work.
            // The limits remain absolute and source-independent.
            EffectiveThreshold = std::max(EffectiveThreshold, 192);
            EffectiveBlockLimit = std::max(EffectiveBlockLimit, 96);
        }
        if (HighStoreBenefit) {
            EffectiveThreshold = std::max(
                EffectiveThreshold,
                std::min(1024, 128 + InvariantStoreCount * 8));
            EffectiveBlockLimit = std::max(EffectiveBlockLimit, 320);
        }

        int PeelCloneCost = 0;
        int PeelBlockCount = 0;
        for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
            if (!BB) {
                PeelCloneCost = EffectiveThreshold + 1;
                break;
            }
            ++PeelBlockCount;
            for (Instruction *I : *BB) {
                if (!I || dynamic_cast<PhiInst *>(I)) continue;
                int LocalCost = 1;
                if (dynamic_cast<CallInst *>(I)) LocalCost = 8;
                else if (I->instType == Instruction::SDIV ||
                         I->instType == Instruction::UDIV ||
                         I->instType == Instruction::FDIV ||
                         I->instType == Instruction::SREM ||
                         I->instType == Instruction::UREM ||
                         I->instType == Instruction::FREM) {
                    LocalCost = 4;
                }
                if (PeelCloneCost > EffectiveThreshold - LocalCost) {
                    PeelCloneCost = EffectiveThreshold + 1;
                    break;
                }
                PeelCloneCost += LocalCost;
            }
        }

        if (IV && IV->Valid && PeelBlockCount <= EffectiveBlockLimit &&
            PeelCloneCost > 0 && PeelCloneCost <= EffectiveThreshold) {
            bool ConstantLongEnough =
                TCI.ProvenNoWrap && TCI.ConstantTripCount &&
                *TCI.ConstantTripCount > PeelCount;
            bool CanGuardRuntime =
                (TCI.IsRuntime || !TCI.ProvenNoWrap) && PeelCount > 0 &&
                PeelCount <= Config.MaxGuardedPeelCount;
            if (PeelCount > 0 &&
                PeelCount <= Config.MaxPeelCount &&
                (ConstantLongEnough || CanGuardRuntime) &&
                static_cast<int64_t>(PeelCloneCost) * PeelCount <=
                    EffectiveThreshold) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Peeling;
                Decision.Count = PeelCount;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = false;
                Decision.IsRuntime = false;
                Decision.IsPeeling = true;
                if (StableState) {
                    Decision.StabilizedPhi = StableState->Phi;
                    Decision.StabilizedValue = StableState->StableValue;
                    Decision.StabilizedInitial = StableState->Initial;
                    Decision.StabilizedBound = StableState->Bound;
                    Decision.StabilizedInnerExitCmp =
                        StableState->InnerExitCmp;
                    Decision.StabilizedInnerContinueOnTrue =
                        StableState->InnerContinueOnTrue;
                }
                return Decision;
            }
        }
    }

    if (HasCall || Nested) {
        return makeNoUnrollDecision();
    }

    if (Config.AllowPartial && TCI.ProvenNoWrap && TCI.ConstantTripCount &&
        BodyCost <= Config.MaxLoopBodyCostForPartial) {
        int64_t TripCount = *TCI.ConstantTripCount;
        int Factor = chooseFactorBySize(BodyCost,
                                        Config.PartialThreshold,
                                        Config.PartialMaxCount);
        std::vector<int> Factors;
        if (Factor >= 4) {
            Factors.push_back(4);
            Factors.push_back(2);
        } else if (Factor >= 2) {
            Factors.push_back(2);
        }

        for (int Candidate : Factors) {
            if (TripCount >= Candidate && TripCount % Candidate == 0) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Partial;
                Decision.Count = Candidate;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = false;
                Decision.IsRuntime = false;
                Decision.IsPeeling = false;
                return Decision;
            }
        }

        if (Config.AllowRemainder) {
            for (int Candidate : Factors) {
                if (TripCount % Candidate == 0 ||
                    TripCount <= Candidate) {
                    continue;
                }

                auto RI = SE.getRemainderInfo(L, Candidate);
                if (!RI || !RI->Valid || !RI->NeedRemainderLoop) {
                    continue;
                }

                UnrollDecision Decision;
                Decision.Kind = UnrollKind::PartialWithRemainder;
                Decision.Count = Candidate;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = true;
                Decision.IsRuntime = false;
                Decision.IsPeeling = false;
                return Decision;
            }
        }
    }

    if (Config.AllowRuntime && Config.AllowRemainder &&
        TCI.IsRuntime &&
        BodyCost <= Config.MaxLoopBodyCostForRuntime &&
        isCheapSCEV(TCI.TripCount, Config)) {
        const bool RuntimeTripCountIsProven =
            TCI.ProvenNoWrap ||
            proveRuntimeTripCountAtPreheader(SE, TCI, RI, DT);
        if (!RuntimeTripCountIsProven)
            return makeNoUnrollDecision();
        int RuntimeCountLimit = std::min(Config.RuntimeDefaultCount,
                                         Config.RuntimeMaxCount);
        int Factor = chooseFactorBySize(BodyCost,
                                        Config.RuntimeThreshold,
                                        RuntimeCountLimit);
        if (Factor >= 2) {
            auto RI = SE.getRemainderInfo(L, Factor);
            if (RI && RI->Valid && RI->NeedRemainderLoop) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Runtime;
                Decision.Count = Factor;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = true;
                Decision.IsRuntime = true;
                Decision.IsPeeling = false;
                Decision.TripCountIsProven = true;
                return Decision;
            }
        }
    }

    return makeNoUnrollDecision();
}

LoopUnrollResult LoopUnrollPass::executeFullUnroll(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }
    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->ProvenNoWrap || !TCI->ConstantTripCount ||
        *TCI->ConstantTripCount != Decision.Count ||
        TCI->ExitingBlock != TCI->Latch) {
        return LoopUnrollResult::Unmodified;
    }

    auto Core = cloneUnrolledBodyCore(L, Decision.Count, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }
    if (!hasLatchExitShape(*Core)) {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    if (!makeStraightLineMain(*Core, Decision.Count, Core->Shape.Exit,
                              nullptr)) {
        return LoopUnrollResult::Unmodified;
    }
    if (!rewriteExitPhisForSingleExiting(*Core, Decision.Count - 1)) {
        return LoopUnrollResult::Unmodified;
    }
    if (!removeOriginalHeaderPhisForFull(*Core)) {
        return LoopUnrollResult::Unmodified;
    }

    SE.clear();
    return LoopUnrollResult::FullyUnrolled;
}

LoopUnrollResult LoopUnrollPass::executePartialUnroll(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->ProvenNoWrap || !TCI->ConstantTripCount ||
        *TCI->ConstantTripCount % Decision.Count != 0 ||
        (TCI->ExitingBlock != TCI->Latch &&
         TCI->ExitingBlock != TCI->Header)) {
        return LoopUnrollResult::Unmodified;
    }

    auto Core = cloneUnrolledBodyCore(L, Decision.Count, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }

    if (hasLatchExitShape(*Core)) {
        BasicBlock *LastExiting = Core->Exitings.back();
        BasicBlock *ClonedHeader = Core->Headers.back();
        if (!LastExiting || !ClonedHeader ||
            !BasicBlock::redirectBranchTarget(LastExiting, ClonedHeader,
                                              Core->Headers.front())) {
            discardUncommittedUnrollClones(*Core);
            return LoopUnrollResult::Unmodified;
        }

        for (int Iter = 0; Iter + 1 < Decision.Count; ++Iter) {
            if (!BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Core->Headers[Iter + 1])) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!rewriteExitPhisForSingleExiting(*Core,
                                             Decision.Count - 1)) {
            return LoopUnrollResult::Unmodified;
        }
    } else if (Core->Shape.Exiting == Core->Shape.Header) {
        for (int Iter = 1; Iter < Decision.Count; ++Iter) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Exitings[Iter], Core->Shape.Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Body)) {
                discardUncommittedUnrollClones(*Core);
                return LoopUnrollResult::Unmodified;
            }
        }
        for (int Iter = 0; Iter + 1 < Decision.Count; ++Iter) {
            if (!BasicBlock::redirectBranchTarget(
                    Core->Latches[Iter], Core->Headers[Iter],
                    Core->Headers[Iter + 1])) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!BasicBlock::redirectBranchTarget(
                Core->Latches.back(), Core->Headers.back(),
                Core->Headers.front())) {
            return LoopUnrollResult::Unmodified;
        }
    } else {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    if (!rewriteOriginalHeaderPhisForPartial(*Core,
                                             Decision.Count - 1)) {
        return LoopUnrollResult::Unmodified;
    }

    SE.clear();
    return LoopUnrollResult::PartiallyUnrolled;
}

LoopUnrollResult LoopUnrollPass::executePartialWithRemainder(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }
    auto RI = SE.getRemainderInfo(L, Decision.Count);
    if (!RI || !RI->Valid || !RI->NeedRemainderLoop) {
        return LoopUnrollResult::Unmodified;
    }

    if (!RI->ConstantMainTripCount || !RI->ConstantRemainderCount ||
        *RI->ConstantMainTripCount < Decision.Count ||
        *RI->ConstantRemainderCount <= 0 ||
        *RI->ConstantRemainderCount >= Decision.Count) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || (TCI->ExitingBlock != TCI->Latch &&
                 TCI->ExitingBlock != TCI->Header)) {
        return LoopUnrollResult::Unmodified;
    }

    int64_t NumGroups =
        *RI->ConstantMainTripCount / Decision.Count;
    Value *GroupCount =
        makeTypedIntConstant(NumGroups, RI->TripCount->Ty);
    if (!GroupCount) return LoopUnrollResult::Unmodified;

    return executeUnrollWithTail(
        L, Decision.Count,
        static_cast<int>(*RI->ConstantRemainderCount), GroupCount, nullptr,
        false, SE);
}

LoopUnrollResult LoopUnrollPass::executeRuntimeUnroll(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }
    if (!Decision.TripCountIsProven)
        return LoopUnrollResult::Unmodified;

    auto RI = SE.getRemainderInfo(L, Decision.Count);
    if (!RI || !RI->Valid || !RI->IsRuntimeRemainder) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || (TCI->ExitingBlock != TCI->Latch &&
                 TCI->ExitingBlock != TCI->Header) ||
        !TCI->PreHeader || !RI->TripCount ||
        !canMaterializeSCEV(RI->TripCount, L, TCI->PreHeader, DT)) {
        return LoopUnrollResult::Unmodified;
    }

    // Runtime-unroll bookkeeping belongs at the outermost loop preheader at
    // which the trip-count expression is still available.  Materializing it
    // unconditionally in the inner preheader makes an outer-invariant bound
    // execute once per outer iteration.  LICM can normally repair that, but a
    // max(0, N) SCEV contains a SelectInst and used to strand the whole chain
    // in the parent loop.  Choosing the placement here is both cheaper and
    // independent of a later cleanup pass.
    Loop *ExpansionLoop = L;
    BasicBlock *ExpansionBlock = TCI->PreHeader;
    for (Loop *Ancestor = TCI->ProvenNoWrap ? L->getParentLoop() : nullptr;
         Ancestor; Ancestor = Ancestor->getParentLoop()) {
        // A rotated parent may have a guard predecessor whose other edge is
        // the zero-trip bypass.  Such a block is not a canonical preheader
        // (it has two successors), but it is still the unique entry edge and
        // is a valid placement for this non-trapping integer expression.
        BasicBlock *AncestorEntry = nullptr;
        bool AmbiguousEntry = false;
        for (BasicBlock *Pred : Ancestor->getHeader()->getPredecessors()) {
            if (!Pred || Ancestor->contains(Pred)) continue;
            if (!AncestorEntry) {
                AncestorEntry = Pred;
            } else if (AncestorEntry != Pred) {
                AmbiguousEntry = true;
                break;
            }
        }
        if (AmbiguousEntry || !AncestorEntry ||
            !canMaterializeSCEV(RI->TripCount, Ancestor,
                                AncestorEntry, DT)) {
            break;
        }
        ExpansionLoop = Ancestor;
        ExpansionBlock = AncestorEntry;
    }

    std::unordered_map<const SCEVExpr *, Value *> Cache;
    Value *TripCount =
        materializeSCEV(RI->TripCount, ExpansionLoop, ExpansionBlock,
                        DT, Cache);
    if (!TripCount || !TripCount->getIRType() ||
        !TripCount->getIRType()->isInt()) {
        return LoopUnrollResult::Unmodified;
    }

    Value *Remainder = nullptr;
    Value *NumGroups = nullptr;

    // runtime 决策只有在 trip count 的固定位宽计算已被证明无回绕后才会
    // 到达这里。header-test 的 max(0, distance) 与 update-test 的
    // max(0, distance)+1 都非负，因此二次幂因子可统一使用移位和掩码，
    // 不必再依赖 exiting 块恰好同时也是 header 这一结构巧合。
    const bool CanUsePowerOfTwoForm =
        Decision.TripCountIsProven &&
        IRMath::isPowerOf2(Decision.Count);
    if (CanUsePowerOfTwoForm) {
        Value *Mask = makeTypedIntConstant(
            Decision.Count - 1, TripCount->getIRType());
        Value *Shift = makeTypedIntConstant(
            IRMath::log2i(Decision.Count), TripCount->getIRType());
        if (!Mask || !Shift) return LoopUnrollResult::Unmodified;

        auto *RemainderInst =
            new BinaryInst(TripCount, BinaryInst::AND, Mask);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, RemainderInst);
        auto *NumGroupsInst =
            new BinaryInst(TripCount, BinaryInst::LSHR, Shift);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, NumGroupsInst);
        Remainder = RemainderInst;
        NumGroups = NumGroupsInst;
    } else {
        // 非二次幂因子保留通用的有符号除法与取模路径。
        Value *Factor = makeTypedIntConstant(
            Decision.Count, TripCount->getIRType());
        if (!Factor) return LoopUnrollResult::Unmodified;

        auto *RemainderInst =
            new BinaryInst(TripCount, BinaryInst::MOD, Factor);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, RemainderInst);
        auto *MainTripCount =
            new BinaryInst(TripCount, BinaryInst::SUB, RemainderInst);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, MainTripCount);
        auto *NumGroupsInst =
            new BinaryInst(MainTripCount, BinaryInst::DIV, Factor);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, NumGroupsInst);
        Remainder = RemainderInst;
        NumGroups = NumGroupsInst;
    }

    return executeUnrollWithTail(L, Decision.Count, Decision.Count - 1,
                                 NumGroups, Remainder, true, SE);
}

LoopUnrollResult LoopUnrollPass::executePeeling(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT,
    BasicAAResult &AA) {
    if (!L || Decision.Count <= 0) {
        return LoopUnrollResult::Unmodified;
    }

    int PeelCount = Decision.Count;

    std::vector<BasicBlock *> ExitBlocks = L->getExitBlocks();
    if (ExitBlocks.size() != 1 || !ExitBlocks.front()) {
        return LoopUnrollResult::Unmodified;
    }

    std::unordered_set<BasicBlock *> ExitingBlocks;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (L->isExitingBlock(BB)) {
            ExitingBlocks.insert(BB);
        }
    }
    if (ExitingBlocks.size() != 1) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->Valid) {
        return LoopUnrollResult::Unmodified;
    }
    // A mathematical constant count without a no-wrap proof cannot justify
    // deleting the cloned exit checks.  Peeling is still legal when every
    // cloned iteration retains its original guard.
    const bool GuardedPeeling =
        !TCI->ConstantTripCount.has_value() || !TCI->ProvenNoWrap;
    if (GuardedPeeling) {
        if ((!TCI->IsRuntime && TCI->ProvenNoWrap) ||
            PeelCount > Config.MaxGuardedPeelCount)
            return LoopUnrollResult::Unmodified;
    } else if (*TCI->ConstantTripCount <= PeelCount) {
        return LoopUnrollResult::Unmodified;
    }

    auto ShapeOpt = getLoopExecutionShape(L, SE);
    if (!ShapeOpt || !ShapeOpt->PreHeader || !ShapeOpt->Header ||
        !ShapeOpt->Latch || !ShapeOpt->Exiting || !ShapeOpt->Exit) {
        return LoopUnrollResult::Unmodified;
    }

    // 与 remainder unroll 使用相同的互斥分类。单块循环若同时进入两种
    // 处理路径，克隆退出边与原循环 PHI 的前驱会来自不同复制体。
    const bool LatchExiting = ShapeOpt->Exiting == ShapeOpt->Latch;
    const bool HeaderExiting =
        !LatchExiting && ShapeOpt->Exiting == ShapeOpt->Header;
    if (!LatchExiting && !HeaderExiting) {
        return LoopUnrollResult::Unmodified;
    }

    std::vector<BasicBlock *> LoopBlocks = L->getBlocksInFunctionOrder();
    if (LoopBlocks.empty() || !canCloneAllInstructions(LoopBlocks)) {
        return LoopUnrollResult::Unmodified;
    }

    std::vector<StoreInst *> RedundantResidualStores =
        collectPeelRedundantStoreCandidates(L, LI, AA, DT);

    auto Core = clonePeeledIterations(L, PeelCount, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }
    auto FailUncommitted = [&]() {
        discardUncommittedPeelClones(*Core);
        return LoopUnrollResult::Unmodified;
    };
    if (!rewriteOriginalHeaderPhisAfterPeeling(
            *Core, /*ValidateOnly=*/true)) {
        return FailUncommitted();
    }
    if (GuardedPeeling) {
        if (!addIncomingToExitPhisForPeeledBlocks(
                *Core, /*ValidateOnly=*/true)) {
            return FailUncommitted();
        }
    } else if (!removeIncomingFromExitPhisForPeeledBlocks(
                   *Core, /*ValidateOnly=*/true)) {
        return FailUncommitted();
    }

    auto *PreBr = Core->Shape.PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(PreBr) ||
        PreBr->getSuccessor(0) != Core->Shape.Header) {
        return FailUncommitted();
    }
    for (int Iter = 0; Iter < PeelCount; ++Iter) {
        if (LatchExiting) {
            auto *Br = Core->Exitings[Iter]
                ? Core->Exitings[Iter]->getBranchTerminator() : nullptr;
            if (!Br || !Br->isConditional())
                return FailUncommitted();
            bool HasBackedge = false;
            for (BasicBlock *Succ : Core->Exitings[Iter]->getSuccessors())
                HasBackedge |= Succ == Core->Headers[Iter];
            if (!HasBackedge) return FailUncommitted();
        } else {
            auto *HeaderBr = Core->Headers[Iter]
                ? Core->Headers[Iter]->getBranchTerminator() : nullptr;
            auto *LatchBr = Core->Latches[Iter]
                ? Core->Latches[Iter]->getBranchTerminator() : nullptr;
            if (!HeaderBr || !HeaderBr->isConditional() || !LatchBr)
                return FailUncommitted();
            bool LatchTargetsHeader = false;
            for (BasicBlock *Succ : Core->Latches[Iter]->getSuccessors())
                LatchTargetsHeader |= Succ == Core->Headers[Iter];
            if (!LatchTargetsHeader) return FailUncommitted();
        }
    }

    if (LatchExiting) {
        for (int Iter = 0; Iter < PeelCount; ++Iter) {
            BasicBlock *Target =
                Iter + 1 < PeelCount ? Core->Headers[Iter + 1]
                                     : Core->Shape.Header;

            if (!Core->Exitings[Iter] || !Target) {
                return FailUncommitted();
            }
            if (GuardedPeeling) {
                // Preserve the short-trip/exit edge and retarget only the
                // cloned backedge to the next peeled/original iteration.
                if (!BasicBlock::redirectBranchTarget(
                        Core->Exitings[Iter], Core->Headers[Iter], Target)) {
                    return FailUncommitted();
                }
            } else if (!BasicBlock::replaceCondBrWithUncond(
                           Core->Exitings[Iter], Target)) {
                return FailUncommitted();
            }
        }
    } else if (HeaderExiting) {
        for (int Iter = 0; Iter < PeelCount; ++Iter) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Headers[Iter], Core->Shape.Exit);
            if (!Body) {
                return FailUncommitted();
            }
            if (!GuardedPeeling &&
                !BasicBlock::replaceCondBrWithUncond(Core->Headers[Iter],
                                                     Body)) {
                return FailUncommitted();
            }

            BasicBlock *Target =
                Iter + 1 < PeelCount ? Core->Headers[Iter + 1]
                                     : Core->Shape.Header;

            if (!Core->Latches[Iter] || !Target ||
                !BasicBlock::redirectBranchTarget(Core->Latches[Iter],
                                                  Core->Headers[Iter],
                                                  Target)) {
                return FailUncommitted();
            }
        }
    }

    // Exit PHIs must be made consistent before the new guarded exit edges
    // become reachable.  For a proven-long constant trip those edges were
    // removed and their cloned incoming entries must instead disappear.
    if (GuardedPeeling) {
        if (!addIncomingToExitPhisForPeeledBlocks(*Core))
            return LoopUnrollResult::Unmodified;
    } else if (!removeIncomingFromExitPhisForPeeledBlocks(*Core)) {
        return LoopUnrollResult::Unmodified;
    }

    if (Core->Headers.empty() || !Core->Headers[0] ||
        !BasicBlock::redirectBranchTarget(Core->Shape.PreHeader,
                                          Core->Shape.Header,
                                          Core->Headers[0])) {
        return LoopUnrollResult::Unmodified;
    }

    std::unordered_map<PhiInst *, Value *> HeaderPhiReplacements;
    if (!rewriteOriginalHeaderPhisAfterPeeling(
            *Core, /*ValidateOnly=*/false, &HeaderPhiReplacements)) {
        return LoopUnrollResult::Unmodified;
    }

    Value *StableResidualValue = Decision.StabilizedValue;
    if (!StableResidualValue && Decision.StabilizedInitial &&
        Decision.StabilizedBound) {
        // Exact signed maximum for the stabilized one-shot state.  Both
        // operands were proved available before the outer loop while making
        // the decision, so placing it in the preheader preserves dominance
        // and computes it only once.
        auto *ChooseInitial = new BinaryInst(
            Decision.StabilizedInitial, BinaryInst::G,
            Decision.StabilizedBound);
        BasicBlock::insertBeforeTerminator(Core->Shape.PreHeader,
                                           ChooseInitial);
        auto *StableMax = new SelectInst(ChooseInitial,
                                         Decision.StabilizedInitial,
                                         Decision.StabilizedBound);
        BasicBlock::insertBeforeTerminator(Core->Shape.PreHeader, StableMax);
        StableResidualValue = StableMax;
    }

    if (Decision.StabilizedPhi && StableResidualValue) {
        auto It = HeaderPhiReplacements.find(Decision.StabilizedPhi);
        Value *ResidualState =
            It == HeaderPhiReplacements.end() ? nullptr : It->second;
        if (ResidualState && ResidualState != StableResidualValue) {
            auto *ResidualPhi = dynamic_cast<PhiInst *>(ResidualState);
            if (ResidualPhi &&
                ResidualPhi->getParent() == Core->Shape.Header) {
                ResidualPhi->replaceAllUsesWith(StableResidualValue);
                delete ResidualPhi;
            }
        }
    }

    // The stable-state proof established that this exact nested-loop header
    // test takes the exit edge on every residual outer iteration.  Materialize
    // the proven result now.  The peeled clone has its own cloned comparison,
    // so the first iteration still executes the one-shot nested loop normally.
    // CFG cleanup can then remove the unreachable residual body before LICM
    // decides whether the readonly sibling call is reusable.
    if (Decision.StabilizedInnerExitCmp &&
        Decision.StabilizedInnerExitCmp->getParent()) {
        const bool ExitCondition =
            !Decision.StabilizedInnerContinueOnTrue;
        Decision.StabilizedInnerExitCmp->replaceAllUsesWith(
            ConstBool::newConstBool(ExitCondition));
    }

    // Any path entering the residual loop has completed a peeled iteration.
    // For a store selected above, direct control is iteration-invariant, its
    // address/value are invariant, and no competing write can change the
    // location.  Therefore the peeled iteration either established the exact
    // value (and every residual iteration would repeat it), or the invariant
    // control skipped it and will keep skipping it.  Loads remain untouched.
    for (StoreInst *SI : RedundantResidualStores) {
        if (!SI || !SI->getParent() || !L->contains(SI->getParent()))
            continue;
        SI->eraseFromParent();
        delete SI;
    }

    SE.clear();
    return LoopUnrollResult::PartiallyUnrolled;
}
