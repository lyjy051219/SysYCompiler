#include "ir/opt/LoopUnswitch.hpp"

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/CFG.hpp"
#include "lib/TraverseTools.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

bool LoopUnswitchPass::isBooleanNonConstant(Value *V) {
    if (!V || Value::isUndef(V) || V->isConst() || !V->getIRType())
        return false;
    return V->getIRType() == INTType::NewBool();
}

bool LoopUnswitchPass::isAvailableAt(Value *V, Instruction *UsePoint, const Loop &L,
                   const DominatorTree &DT) {
    if (!V || !UsePoint || Value::isUndef(V)) return false;
    if (V->isConst() || dynamic_cast<Function *>(V) ||
        dynamic_cast<BuildInFunction *>(V)) {
        return true;
    }
    if (auto *Var = dynamic_cast<Variable *>(V)) {
        return Var->isParam() || Var->isGlobal();
    }

    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || L.contains(I->getParent())) return false;
    return DT.dominatesValue(I, UsePoint);
}

bool LoopUnswitchPass::getCanonicalLoopShape(Loop *L, CanonicalLoopShape &Shape) {
    if (!L || !L->getHeader() || !LCSSAPass::isLCSSAForm(L)) return false;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch) {
        return false;
    }
    auto *PreBr = PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(PreBr) ||
        PreBr->getTrueSuccessor() != L->getHeader()) {
        return false;
    }
    Shape = {L->getHeader(), PreHeader, Latch};
    return true;
}

bool LoopUnswitchPass::isDirectLoopBlock(const LoopInfo &LI, Loop *L, BasicBlock *BB) {
    return L && BB && LI.getLoopFor(BB) == L;
}

std::vector<BasicBlock *> LoopUnswitchPass::collectExitingBlocks(Loop &L) {
    std::vector<BasicBlock *> Result;
    for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
        if (BB && L.isExitingBlock(BB)) Result.push_back(BB);
    }
    return Result;
}

bool LoopUnswitchPass::isMustExecuteBeforeLoopProgress(BasicBlock *Parent, Loop &L,
                                     const DominatorTree &DT) {
    if (!Parent) return false;
    for (BasicBlock *Backedge : L.getBackedgeSrcs()) {
        if (!Backedge || !DT.dominates(Parent, Backedge)) return false;
    }
    for (BasicBlock *Exiting : collectExitingBlocks(L)) {
        if (!DT.dominates(Parent, Exiting)) return false;
    }
    return true;
}

bool LoopUnswitchPass::isSafeTrivialPrefix(const TrivialCandidate &C, const LoopInfo &LI,
                         const DominatorTree &DT,
                         const MemorySSAResult &MSSA) {
    if (!C.L || !C.Parent || !C.Branch || !C.Shape.Header) return false;
    if (!isMustExecuteBeforeLoopProgress(C.Parent, *C.L, DT)) return false;

    // Reach the candidate during the first iteration only.  Crossing an edge
    // back to Header would make an instruction in the region execute before
    // the candidate in an earlier iteration and invalidate prefix motion.
    BlockSet Forward;
    std::vector<BasicBlock *> ForwardRoots{C.Shape.Header};
    bool ForwardComplete =
        forEachReachablePreOrderCustomSucc<BasicBlock>(
            ForwardRoots,
            [](BasicBlock *BB) {
                auto Successors = BB ? BB->getSuccessors()
                                     : std::vector<BasicBlock *>{};
                std::reverse(Successors.begin(), Successors.end());
                return Successors;
            },
            [&C, &Forward](BasicBlock *BB) {
                if (!BB || !C.L->contains(BB))
                    return TraversalControl::SkipSuccessors;
                Forward.insert(BB);
                if (BB == C.Parent)
                    return TraversalControl::SkipSuccessors;

                for (BasicBlock *Succ : BB->getSuccessors()) {
                    if (!Succ || !C.L->contains(Succ) ||
                        Succ == C.Shape.Header) {
                        return TraversalControl::Stop;
                    }
                }
                return TraversalControl::Continue;
            });
    if (!ForwardComplete || !Forward.count(C.Parent)) return false;

    // Every block reachable before Parent must still be able to reach Parent
    // without taking the loop backedge.  This excludes a side path on which
    // the original branch was never executed.
    BlockSet CanReachParent;
    std::vector<BasicBlock *> ReverseRoots{C.Parent};
    forEachReachablePreOrderCustomSucc<BasicBlock>(
        ReverseRoots,
        [&C](BasicBlock *BB) {
            std::vector<BasicBlock *> Predecessors;
            if (!BB || BB == C.Shape.Header) return Predecessors;
            for (BasicBlock *Pred : BB->getPredecessors()) {
                if (Pred && C.L->contains(Pred))
                    Predecessors.push_back(Pred);
            }
            std::reverse(Predecessors.begin(), Predecessors.end());
            return Predecessors;
        },
        [&C, &CanReachParent](BasicBlock *BB) {
            if (!BB || !C.L->contains(BB))
                return TraversalControl::SkipSuccessors;
            CanReachParent.insert(BB);
            if (BB == C.Shape.Header)
                return TraversalControl::SkipSuccessors;
            return TraversalControl::Continue;
        });

    for (BasicBlock *BB : Forward) {
        if (!CanReachParent.count(BB) || !isDirectLoopBlock(LI, C.L, BB))
            return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (I == C.Branch) break;
            MemoryAccess *MA = MSSA.getMemoryAccess(I);
            if ((MA && MA->isDef()) ||
                instructionMayFaultOrProducePoison(I) ||
                dceInstructionMustBePreserved(I)) {
                return false;
            }
        }
    }
    return true;
}

bool LoopUnswitchPass::validateTrivialExitPhis(const TrivialCandidate &C,
                             const DominatorTree &DT) {
    if (!C.Exit || !C.Shape.PreHeader || !C.Shape.PreHeader->getTerminator())
        return false;

    const bool SplitExit = C.Exit->getPredecessors().size() != 1 ||
                           C.Exit->getPredecessors().front() != C.Parent;
    for (PhiInst *Phi : C.Exit->collectPhiNodes()) {
        if (!Phi) return false;
        Value *DirectValue = nullptr;
        unsigned DirectCount = 0;
        unsigned RemainingCount = 0;
        forEachPhiIncoming(
            Phi, [&C, &DirectValue, &DirectCount, &RemainingCount](
                     unsigned, Value *Incoming, BasicBlock *Pred) {
            if (Pred == C.Parent) {
                DirectValue = Incoming;
                ++DirectCount;
            } else {
                ++RemainingCount;
            }
            });
        if (DirectCount != 1 ||
            !isAvailableAt(DirectValue, C.Shape.PreHeader->getTerminator(),
                           *C.L, DT)) {
            return false;
        }
        if (SplitExit && RemainingCount == 0) return false;

        // Exit PHIs produced by LCSSA do not depend on each other.  Reject an
        // unusual cyclic/cross-PHI form instead of creating a definition in
        // the new merge block that is used by its predecessor.
        if (!forEachPhiIncoming(
                Phi, [&C](unsigned, Value *Incoming, BasicBlock *) {
                    auto *InI = dynamic_cast<Instruction *>(Incoming);
                    return !InI || InI->getParent() != C.Exit;
                })) {
            return false;
        }
    }
    return true;
}

std::optional<LoopUnswitchPass::TrivialCandidate>
LoopUnswitchPass::findTrivialCandidate(
    Function &F, LoopInfo &LI, const DominatorTree &DT,
    const MemorySSAResult &MSSA) {
    std::vector<BasicBlock *> DomOrder = DT.getBlocksInPreOrder();
    if (DomOrder.empty()) {
        for (BasicBlock *BB : F) DomOrder.push_back(BB);
    }

    for (Loop *L : LI.getLoopsInPostOrder()) {
        CanonicalLoopShape Shape;
        if (!getCanonicalLoopShape(L, Shape)) continue;

        for (BasicBlock *BB : DomOrder) {
            if (!isDirectLoopBlock(LI, L, BB)) continue;
            BrInst *Br = BB->getBranchTerminator();
            if (!BasicBlock::isCondBr(Br)) continue;
            Value *Cond = Br->getCondition();
            if (!isBooleanNonConstant(Cond) ||
                !isAvailableAt(Cond, Shape.PreHeader->getTerminator(),
                               *L, DT)) {
                continue;
            }

            BasicBlock *TrueSucc = Br->getTrueSuccessor();
            BasicBlock *FalseSucc = Br->getFalseSuccessor();
            if (!TrueSucc || !FalseSucc || TrueSucc == FalseSucc) continue;
            const bool TrueInside = L->contains(TrueSucc);
            const bool FalseInside = L->contains(FalseSucc);
            if (TrueInside == FalseInside) continue;

            TrivialCandidate C;
            C.L = L;
            C.Shape = Shape;
            C.Parent = BB;
            C.Branch = Br;
            C.Condition = Cond;
            C.ExitOnTrue = !TrueInside;
            C.Exit = C.ExitOnTrue ? TrueSucc : FalseSucc;
            C.Continue = C.ExitOnTrue ? FalseSucc : TrueSucc;

            if (!isSafeTrivialPrefix(C, LI, DT, MSSA) ||
                !validateTrivialExitPhis(C, DT)) {
                continue;
            }
            return C;
        }
    }
    return std::nullopt;
}

bool LoopUnswitchPass::replaceConditionUsesInBlocks(Value *Condition, const BlockSet &Blocks,
                                  bool KnownValue) {
    if (!Condition || Blocks.empty()) return false;
    auto Uses = Condition->getUses();
    Value *Replacement = ConstBool::newConstBool(KnownValue);
    bool Changed = false;
    for (Use *U : Uses) {
        auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
        BasicBlock *UserBB = UserI ? UserI->getParent() : nullptr;
        if (UserBB && Blocks.count(UserBB))
            Changed |= U->replaceUseeWith(Replacement);
    }
    return Changed;
}

void LoopUnswitchPass::setHeaderPredecessor(BasicBlock *Header, BasicBlock *OldPreHeader,
                          BasicBlock *NewPreHeader) {
    if (!Header || !OldPreHeader || !NewPreHeader) return;
    for (PhiInst *Phi : Header->collectPhiNodes()) {
        forEachPhiIncoming(
            Phi, [Phi, OldPreHeader, NewPreHeader](
                     unsigned Index, Value *, BasicBlock *Pred) {
                if (Pred == OldPreHeader)
                    Phi->setIncomingBlock(Index, NewPreHeader);
            });
    }
}

bool LoopUnswitchPass::transformTrivial(Function &F, const TrivialCandidate &C) {
    if (!C.L || !C.Shape.Header || !C.Shape.PreHeader || !C.Parent ||
        !C.Branch || !C.Exit || !C.Continue) {
        return false;
    }

    BasicBlock *OldPreHeader = C.Shape.PreHeader;
    BasicBlock *Header = C.Shape.Header;
    auto *OldPreBr = OldPreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(OldPreBr) ||
        OldPreBr->getTrueSuccessor() != Header ||
        C.Parent->getBranchTerminator() != C.Branch) {
        return false;
    }

    auto *NewPreHeader = new BasicBlock();
    F.pushBlock(NewPreHeader);
    NewPreHeader->setIdent(
        F.makeUniqueBlockIdent(Header->getIdent() + ".unswitch.cont"));

    BasicBlock *ExitDestination = C.Exit;
    const auto ExitPreds = C.Exit->getPredecessors();
    const bool ReuseExit = ExitPreds.size() == 1 &&
                           ExitPreds.front() == C.Parent;

    if (ReuseExit) {
        for (PhiInst *Phi : C.Exit->collectPhiNodes()) {
            forEachPhiIncoming(
                Phi, [Phi, &C, OldPreHeader](
                         unsigned Index, Value *, BasicBlock *Pred) {
                    if (Pred == C.Parent)
                        Phi->setIncomingBlock(Index, OldPreHeader);
                });
        }
    } else {
        auto *ExitMerge = new BasicBlock();
        F.pushBlock(ExitMerge);
        ExitMerge->setIdent(
            F.makeUniqueBlockIdent(C.Exit->getIdent() + ".unswitch.merge"));
        ExitDestination = ExitMerge;

        // Moving the terminator changes every outgoing CFG edge from
        // C.Exit -> Succ into ExitMerge -> Succ.  Preserve the old successor
        // list so their PHIs can be updated after the move.
        const std::vector<BasicBlock *> OldExitSuccessors =
            C.Exit->getSuccessors();
        std::vector<Instruction *> NonPhis;
        for (Instruction *I : *C.Exit) {
            if (!dynamic_cast<PhiInst *>(I)) NonPhis.push_back(I);
        }
        for (Instruction *I : NonPhis) {
            I->eraseFromParent();
            ExitMerge->pushBack(I);
        }

        std::vector<PhiInst *> OldPhis = C.Exit->collectPhiNodes();
        std::vector<PhiInst *> NormalPhis;
        std::vector<PhiInst *> MergePhis;
        for (PhiInst *OldPhi : OldPhis) {
            Value *DirectValue = nullptr;
            std::vector<PhiInst::IncomingEdge> Remaining;
            for (const auto &In : OldPhi->collectIncomingEdges()) {
                if (In.Pred == C.Parent) DirectValue = In.V;
                else Remaining.push_back(In);
            }

            // Keep an exit PHI even for a single remaining edge.  Remaining
            // values may be defined inside the loop, and forwarding them
            // directly to ExitMerge would break LCSSA at the new immediate
            // exit block C.Exit.
            auto *NormalPhi = new PhiInst(OldPhi->getIRType());
            NormalPhi->setIdent(OldPhi->getIdent() + ".unswitch.normal");
            for (const auto &In : Remaining)
                NormalPhi->addIncoming(In.V, In.Pred);
            NormalPhis.push_back(NormalPhi);
            Value *NormalValue = NormalPhi;

            auto *MergePhi = new PhiInst(OldPhi->getIRType());
            MergePhi->setIdent(OldPhi->getIdent() + ".unswitch.merge");
            MergePhi->addIncoming(DirectValue, OldPreHeader);
            MergePhi->addIncoming(NormalValue, C.Exit);
            MergePhis.push_back(MergePhi);

            OldPhi->replaceAllUsesWith(MergePhi);
            delete OldPhi;
        }
        for (auto It = NormalPhis.rbegin(); It != NormalPhis.rend(); ++It)
            C.Exit->pushFront(*It);
        for (auto It = MergePhis.rbegin(); It != MergePhis.rend(); ++It)
            ExitMerge->pushFront(*It);
        C.Exit->genUnCondInst(ExitMerge);

        BlockSet SeenSuccessors;
        for (BasicBlock *Succ : OldExitSuccessors) {
            if (!Succ || !SeenSuccessors.insert(Succ).second) continue;
            BasicBlock::replaceIncomingPredInSuccPhis(
                Succ, C.Exit, ExitMerge);
        }
    }

    setHeaderPredecessor(Header, OldPreHeader, NewPreHeader);
    NewPreHeader->genUnCondInst(Header);

    // Reuse the original conditional branch object in the old preheader.
    // Besides matching the intended transform, this guarantees that no
    // branch flags or condition spelling are accidentally lost.
    const unsigned ExitOperand = C.ExitOnTrue ? 1U : 2U;
    const unsigned ContinueOperand = C.ExitOnTrue ? 2U : 1U;
    C.Branch->replaceOperand(ExitOperand, ExitDestination);
    C.Branch->replaceOperand(ContinueOperand, NewPreHeader);
    C.Branch->eraseFromParent();
    delete OldPreBr;
    OldPreHeader->pushBack(C.Branch);
    C.Parent->genUnCondInst(C.Continue);

    BlockSet OriginalLoopBlocks;
    for (BasicBlock *BB : C.L->getBlocksIncludingSubLoops())
        if (BB) OriginalLoopBlocks.insert(BB);
    replaceConditionUsesInBlocks(C.Condition, OriginalLoopBlocks,
                                 /*KnownValue=*/!C.ExitOnTrue);
    return true;
}

LoopUnswitchPass::BlockSet
LoopUnswitchPass::collectSpecializedReachable(Loop &L, BasicBlock *Parent,
                                     BasicBlock *ChosenSuccessor) {
    BlockSet Reachable;
    BasicBlock *Header = L.getHeader();
    if (!Header || !Parent || !ChosenSuccessor) return Reachable;
    if (!L.contains(Header)) return Reachable;

    std::vector<BasicBlock *> Roots{Header};
    return collectReachableFromRootsCustomSucc<BasicBlock>(
        Roots, [&L, Parent, ChosenSuccessor](BasicBlock *BB) {
            std::vector<BasicBlock *> Successors;
            if (!BB || !L.contains(BB)) return Successors;
            if (BB == Parent) {
                if (L.contains(ChosenSuccessor))
                    Successors.push_back(ChosenSuccessor);
                return Successors;
            }
            for (BasicBlock *Succ : BB->getSuccessors()) {
                if (Succ && L.contains(Succ)) Successors.push_back(Succ);
            }
            std::reverse(Successors.begin(), Successors.end());
            return Successors;
        });
}

bool LoopUnswitchPass::isActiveSpecializedEdge(BasicBlock *Pred, BasicBlock *Succ,
                             BasicBlock *Parent,
                             BasicBlock *ChosenSuccessor) {
    if (!Pred || !Succ) return false;
    if (Pred == Parent) return Succ == ChosenSuccessor;
    return Pred->countEdgesToSucc(Succ) > 0;
}

std::size_t LoopUnswitchPass::countInstructions(const std::vector<BasicBlock *> &Blocks,
                                                const BlockSet *Filter) {
    std::size_t Count = 0;
    for (BasicBlock *BB : Blocks) {
        if (!BB || (Filter && !Filter->count(BB))) continue;
        Count += static_cast<std::size_t>(BB->size());
    }
    return Count;
}

unsigned LoopUnswitchPass::countControlUses(Value *Condition, Loop &L) {
    if (!Condition) return 0;
    std::unordered_set<Instruction *> Seen;
    for (Use *U : Condition->getUses()) {
        auto *I = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
        if (!I || !I->getParent() || !L.contains(I->getParent())) continue;
        if (auto *Br = dynamic_cast<BrInst *>(I)) {
            if (Br->getCondition() == Condition) Seen.insert(I);
        } else if (auto *Sel = dynamic_cast<SelectInst *>(I)) {
            if (Sel->getCondition() == Condition) Seen.insert(I);
        }
    }
    return static_cast<unsigned>(Seen.size());
}

Value *LoopUnswitchPass::stripBooleanNot(Value *V, bool &Inverted) {
    Inverted = false;
    for (unsigned Depth = 0; V && Depth != 8; ++Depth) {
        auto *BI = dynamic_cast<BinaryInst *>(V);
        if (!BI || BI->getOp() != BinaryInst::XOR ||
            BI->getIRType() != INTType::NewBool()) {
            break;
        }
        Value *L = BI->getOperand(0);
        Value *R = BI->getOperand(1);
        auto *LC = dynamic_cast<ConstBool *>(L);
        auto *RC = dynamic_cast<ConstBool *>(R);
        if (LC && LC->getVal()) {
            V = R;
            Inverted = !Inverted;
        } else if (RC && RC->getVal()) {
            V = L;
            Inverted = !Inverted;
        } else {
            break;
        }
    }
    return V;
}

std::optional<bool> LoopUnswitchPass::evaluateConstantBoolean(Value *Condition) {
    if (!Condition || Value::isUndef(Condition)) return std::nullopt;
    if (auto *CB = dynamic_cast<ConstBool *>(Condition))
        return CB->getVal();

    bool Inverted = false;
    Value *Base = stripBooleanNot(Condition, Inverted);
    if (auto *CB = dynamic_cast<ConstBool *>(Base))
        return CB->getVal() != Inverted;

    auto *BI = dynamic_cast<BinaryInst *>(Base);
    if (!BI || BI->getNumOperands() < 2) return std::nullopt;
    Value *Folded = BinaryInst::foldConstants(
        BI->getOp(), BI->getOperand(0), BI->getOperand(1), true);
    auto *FoldedBool = dynamic_cast<ConstBool *>(Folded);
    if (!FoldedBool) return std::nullopt;
    return FoldedBool->getVal() != Inverted;
}

unsigned LoopUnswitchPass::predicateRelationMask(BinaryInst::Operation Op) {
    switch (Op) {
    case BinaryInst::L:  return RelationLessMask;
    case BinaryInst::LE: return RelationLessMask | RelationEqualMask;
    case BinaryInst::E:  return RelationEqualMask;
    case BinaryInst::NE: return RelationLessMask | RelationGreaterMask;
    case BinaryInst::GE: return RelationEqualMask | RelationGreaterMask;
    case BinaryInst::G:  return RelationGreaterMask;
    default:             return 0;
    }
}

std::vector<LoopUnswitchPass::ClosedInterval>
LoopUnswitchPass::predicateIntervals(BinaryInst::Operation Op,
                                               std::int64_t Bound) {
    std::vector<ClosedInterval> Result;
    auto Add = [&](std::int64_t L, std::int64_t R) {
        L = std::max(L, PredicateMinimum);
        R = std::min(R, PredicateMaximum);
        if (L <= R) Result.emplace_back(L, R);
    };
    switch (Op) {
    case BinaryInst::L:  Add(PredicateMinimum, Bound - 1); break;
    case BinaryInst::LE: Add(PredicateMinimum, Bound); break;
    case BinaryInst::E:  Add(Bound, Bound); break;
    case BinaryInst::NE:
        Add(PredicateMinimum, Bound - 1);
        Add(Bound + 1, PredicateMaximum);
        break;
    case BinaryInst::GE: Add(Bound, PredicateMaximum); break;
    case BinaryInst::G:  Add(Bound + 1, PredicateMaximum); break;
    default: break;
    }
    return Result;
}

bool LoopUnswitchPass::intervalsAreSubset(const std::vector<ClosedInterval> &A,
                        const std::vector<ClosedInterval> &B) {
    for (const ClosedInterval &Need : A) {
        std::int64_t CoveredThrough = Need.first - 1;
        for (const ClosedInterval &Have : B) {
            if (Have.second < Need.first || Have.first > Need.second)
                continue;
            if (Have.first > CoveredThrough + 1) break;
            CoveredThrough = std::max(CoveredThrough, Have.second);
            if (CoveredThrough >= Need.second) break;
        }
        if (CoveredThrough < Need.second) return false;
    }
    return true;
}

std::optional<LoopUnswitchPass::IntegerBoundPredicate>
LoopUnswitchPass::normalizeIntegerBound(Value *V) {
    auto *BI = dynamic_cast<BinaryInst *>(V);
    if (!BI || !BinaryInst::isCompareOp(BI->getOp())) return std::nullopt;
    Value *L = BI->getOperand(0);
    Value *R = BI->getOperand(1);
    if (!L || !R) return std::nullopt;

    Value *Variable = L;
    auto *Bound = dynamic_cast<ConstInt *>(R);
    BinaryInst::Operation Predicate = BI->getOp();
    if (!Bound) {
        Bound = dynamic_cast<ConstInt *>(L);
        Variable = R;
        Predicate = BinaryInst::reversePredicate(Predicate);
    }
    auto *Ty = Variable
                   ? dynamic_cast<INTType *>(Variable->getIRType().get())
                   : nullptr;
    if (!Bound || !Ty || Ty->getIsBool() || Ty->getSize() != 4)
        return std::nullopt;
    return IntegerBoundPredicate{
        Variable, Predicate, static_cast<std::int32_t>(Bound->getVal())};
}

std::optional<bool> LoopUnswitchPass::proveBaseConditionFromGuard(Value *Target,
                                                Value *Guard,
                                                bool GuardKnown) {
    if (!Target || !Guard) return std::nullopt;
    if (Target == Guard) return GuardKnown;

    auto *TargetCmp = dynamic_cast<BinaryInst *>(Target);
    auto *GuardCmp = dynamic_cast<BinaryInst *>(Guard);
    if (!TargetCmp || !GuardCmp ||
        !BinaryInst::isCompareOp(TargetCmp->getOp()) ||
        !BinaryInst::isCompareOp(GuardCmp->getOp())) {
        return std::nullopt;
    }

    Value *TL = TargetCmp->getOperand(0);
    Value *TR = TargetCmp->getOperand(1);
    Value *GL = GuardCmp->getOperand(0);
    Value *GR = GuardCmp->getOperand(1);
    BinaryInst::Operation GuardOp = GuardCmp->getOp();
    if (TL == GR && TR == GL) {
        std::swap(GL, GR);
        GuardOp = BinaryInst::reversePredicate(GuardOp);
    }

    const bool IntegerOperands = TL && TR && TL->getIRType() &&
                                 TR->getIRType() &&
                                 TL->getIRType()->getEnum() == IR_INT &&
                                 TR->getIRType()->getEnum() == IR_INT;
    if (TL == GL && TR == GR) {
        if (!IntegerOperands) {
            if (TargetCmp->getOp() == GuardOp) return GuardKnown;
            return std::nullopt;
        }
        if (!GuardKnown) GuardOp = BinaryInst::negatePredicate(GuardOp);
        const unsigned Known = predicateRelationMask(GuardOp);
        const unsigned Target = predicateRelationMask(TargetCmp->getOp());
        if (Known && (Known & ~Target) == 0) return true;
        if (Known && (Known & Target) == 0) return false;
    }

    if (!IntegerOperands) return std::nullopt;
    auto TargetBound = normalizeIntegerBound(Target);
    auto GuardBound = normalizeIntegerBound(Guard);
    if (!TargetBound || !GuardBound ||
        TargetBound->Variable != GuardBound->Variable) {
        return std::nullopt;
    }
    BinaryInst::Operation KnownPredicate = GuardBound->Predicate;
    if (!GuardKnown)
        KnownPredicate = BinaryInst::negatePredicate(KnownPredicate);
    const auto Known = predicateIntervals(KnownPredicate, GuardBound->Bound);
    const auto TargetTrue = predicateIntervals(TargetBound->Predicate,
                                               TargetBound->Bound);
    if (intervalsAreSubset(Known, TargetTrue)) return true;
    const auto TargetFalse = predicateIntervals(
        BinaryInst::negatePredicate(TargetBound->Predicate),
        TargetBound->Bound);
    if (intervalsAreSubset(Known, TargetFalse)) return false;
    return std::nullopt;
}

std::optional<bool> LoopUnswitchPass::proveConditionFromGuard(Value *Target, Value *Guard,
                                            bool GuardKnown) {
    bool TargetInverted = false;
    bool GuardInverted = false;
    Target = stripBooleanNot(Target, TargetInverted);
    Guard = stripBooleanNot(Guard, GuardInverted);
    GuardKnown = GuardKnown != GuardInverted;
    auto Result = proveBaseConditionFromGuard(Target, Guard, GuardKnown);
    if (!Result) return std::nullopt;
    return *Result != TargetInverted;
}

bool LoopUnswitchPass::conditionIsStableAcrossGuardLoop(Value *Condition, BasicBlock *GuardBB,
                                      BasicBlock *PreHeader,
                                      const LoopInfo &LI,
                                      const DominatorTree &DT) {
    Loop *GuardLoop = LI.getLoopFor(GuardBB);
    if (!GuardLoop || !GuardLoop->contains(PreHeader)) return true;
    CanonicalLoopShape GuardShape;
    return getCanonicalLoopShape(GuardLoop, GuardShape) &&
           GuardShape.PreHeader && GuardShape.PreHeader->getTerminator() &&
           isAvailableAt(Condition, GuardShape.PreHeader->getTerminator(),
                         *GuardLoop, DT);
}

std::optional<bool> LoopUnswitchPass::getEntryKnownValue(
    Value *Condition, BasicBlock *PreHeader, const LoopInfo &LI,
    const DominatorTree &DT) {
    if (!Condition || !PreHeader) return std::nullopt;
    if (auto Constant = evaluateConstantBoolean(Condition))
        return Constant;

    // First follow the actual dynamic entry path.  Unlike a bare block-
    // dominance test, a unique-predecessor chain cannot reuse a condition
    // fact from an earlier iteration of an enclosing loop.
    BasicBlock *Current = PreHeader;
    BlockSet Visited;
    for (unsigned Depth = 0; Current && Depth != 32; ++Depth) {
        if (!Visited.insert(Current).second) break;
        auto Preds = Current->getPredecessors();
        if (Preds.size() != 1) break;
        BasicBlock *Pred = Preds.front();
        if (!Pred || Pred->countEdgesToSucc(Current) != 1 ||
            !DT.dominates(Pred, PreHeader)) {
            break;
        }
        BrInst *Guard = Pred->getBranchTerminator();
        if (BasicBlock::isCondBr(Guard)) {
            BasicBlock *T = Guard->getTrueSuccessor();
            BasicBlock *F = Guard->getFalseSuccessor();
            if (!T || !F || T == F || (T != Current && F != Current))
                break;
            const bool GuardKnown = T == Current;
            if (auto Proven = proveConditionFromGuard(
                    Condition, Guard->getCondition(), GuardKnown)) {
                return Proven;
            }
            Current = Pred;
            continue;
        }
        if (!BasicBlock::isUncondBr(Guard) ||
            Guard->getTrueSuccessor() != Current) {
            break;
        }
        Current = Pred;
    }

    // A successor with a unique incoming edge turns block dominance into
    // edge dominance.  For a guard in an enclosing loop, additionally prove
    // that the SSA condition is invariant across that loop so a fact from an
    // earlier outer iteration cannot leak into the current one.
    Current = PreHeader;
    Visited.clear();
    for (unsigned Depth = 0; Current && Depth != 32; ++Depth) {
        Current = DT.getIDom(Current);
        if (!Current || !Visited.insert(Current).second) break;
        BrInst *Guard = Current->getBranchTerminator();
        if (!BasicBlock::isCondBr(Guard) ||
            !conditionIsStableAcrossGuardLoop(Condition, Current, PreHeader,
                                              LI, DT)) {
            continue;
        }
        BasicBlock *Successors[2] = {Guard->getTrueSuccessor(),
                                     Guard->getFalseSuccessor()};
        if (!Successors[0] || !Successors[1] ||
            Successors[0] == Successors[1]) {
            continue;
        }
        for (unsigned Arm = 0; Arm != 2; ++Arm) {
            BasicBlock *S = Successors[Arm];
            auto Preds = S->getPredecessors();
            if (Preds.size() != 1 || Preds.front() != Current ||
                Current->countEdgesToSucc(S) != 1 ||
                !DT.dominates(S, PreHeader)) {
                continue;
            }
            if (auto Proven = proveConditionFromGuard(
                    Condition, Guard->getCondition(), Arm == 0)) {
                return Proven;
            }
        }
    }
    return std::nullopt;
}

bool LoopUnswitchPass::cloneClosureIsLegal(const NonTrivialCandidate &C) {
    if (!C.L || !C.Parent || !C.CloneSucc || !C.OriginalSucc)
        return false;
    for (BasicBlock *BB : C.AllBlocks) {
        if (!BB || !C.CloneReachable.count(BB)) continue;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
                const bool Legal = forEachPhiIncoming(
                    Phi, [&C, BB](unsigned, Value *Incoming,
                                  BasicBlock *Pred) {
                    bool Active = false;
                    if (BB == C.Shape.Header && Pred == C.Shape.PreHeader) {
                        Active = true;
                    } else if (C.CloneReachable.count(Pred)) {
                        Active = isActiveSpecializedEdge(
                            Pred, BB, C.Parent, C.CloneSucc);
                    }
                    if (!Active) return true;
                    auto *Def = dynamic_cast<Instruction *>(Incoming);
                    if (Def && Def->getParent() &&
                        C.L->contains(Def->getParent()) &&
                        !C.CloneReachable.count(Def->getParent())) {
                        return false;
                    }
                    return true;
                    });
                if (!Legal) return false;
                continue;
            }

            const bool Legal = forEachOperand(
                I, [&C, BB, I](size_t, Use *, Value *Operand) {
                if (auto *Target = dynamic_cast<BasicBlock *>(Operand)) {
                    if (C.L->contains(Target) &&
                        !C.CloneReachable.count(Target)) {
                        auto *Br = dynamic_cast<BrInst *>(I);
                        if (!(BB == C.Parent && Br &&
                              Target == C.OriginalSucc)) {
                            return false;
                        }
                    }
                    return true;
                }
                auto *Def = dynamic_cast<Instruction *>(Operand);
                if (Def && Def->getParent() &&
                    C.L->contains(Def->getParent()) &&
                    !C.CloneReachable.count(Def->getParent())) {
                    return false;
                }
                return true;
                });
            if (!Legal) return false;
        }
    }
    return true;
}

bool LoopUnswitchPass::hasAnyCall(const std::vector<BasicBlock *> &Blocks) {
    for (BasicBlock *BB : Blocks) {
        if (!BB) return true;
        for (Instruction *I : *BB)
            if (dynamic_cast<CallInst *>(I)) return true;
    }
    return false;
}

std::size_t LoopUnswitchPass::estimateExitPhiCost(Loop &L) {
    std::size_t Count = 0;
    for (BasicBlock *Exit : L.getExitBlocks()) {
        if (Exit) Count += Exit->collectPhiNodes().size();
    }
    return Count;
}

bool LoopUnswitchPass::validateVersionedExitPhis(const NonTrivialCandidate &C) {
    if (!C.L) return false;
    for (BasicBlock *Exit : C.L->getExitBlocks()) {
        if (!Exit) return false;
        BlockSet LivePreds = C.TrueReachable;
        LivePreds.insert(C.FalseReachable.begin(), C.FalseReachable.end());

        for (BasicBlock *Pred : LivePreds) {
            if (!Pred || Pred->countEdgesToSucc(Exit) == 0) continue;
            // Parallel exit edges require duplicate PHI entries, a shape not
            // produced by the SysY frontend and deliberately outside this
            // first bounded implementation.
            if (Pred->countEdgesToSucc(Exit) != 1) return false;
        }

        for (PhiInst *Phi : Exit->collectPhiNodes()) {
            if (!Phi) return false;
            if (!forEachPhiIncoming(
                    Phi, [Exit](unsigned, Value *Incoming, BasicBlock *) {
                        auto *InI = dynamic_cast<Instruction *>(Incoming);
                        return !InI || InI->getParent() != Exit;
                    })) {
                return false;
            }
            for (BasicBlock *Pred : C.TrueReachable) {
                if (!Pred || Pred->countEdgesToSucc(Exit) == 0) continue;
                unsigned Hits = 0;
                forEachPhiIncoming(
                    Phi, [Pred, &Hits](unsigned, Value *,
                                       BasicBlock *IncomingPred) {
                        Hits += IncomingPred == Pred;
                    });
                if (Hits != 1) return false;
            }
            for (BasicBlock *Pred : C.FalseReachable) {
                if (!Pred || Pred->countEdgesToSucc(Exit) == 0) continue;
                unsigned Hits = 0;
                forEachPhiIncoming(
                    Phi, [Pred, &Hits](unsigned, Value *,
                                       BasicBlock *IncomingPred) {
                        Hits += IncomingPred == Pred;
                    });
                if (Hits != 1) return false;
            }
        }
    }
    return true;
}

std::vector<LoopUnswitchPass::EntryKnownCandidate>
LoopUnswitchPass::findEntryKnownCandidates(
    LoopInfo &LI, const DominatorTree &DT) {
    std::vector<EntryKnownCandidate> Result;
    for (Loop *L : LI.getLoopsInPostOrder()) {
        CanonicalLoopShape Shape;
        if (!getCanonicalLoopShape(L, Shape) ||
            !Shape.PreHeader->getTerminator()) {
            continue;
        }
        std::vector<BasicBlock *> AllBlocks = L->getBlocksInFunctionOrder();
        for (BasicBlock *BB : AllBlocks) {
            if (!isDirectLoopBlock(LI, L, BB)) continue;
            BrInst *Br = BB->getBranchTerminator();
            if (!BasicBlock::isCondBr(Br)) continue;
            Value *Condition = Br->getCondition();
            if (!isBooleanNonConstant(Condition) ||
                !isAvailableAt(Condition, Shape.PreHeader->getTerminator(),
                               *L, DT)) {
                continue;
            }
            auto Proof = getEntryKnownValue(Condition, Shape.PreHeader,
                                            LI, DT);
            if (!Proof) continue;

            bool Duplicate = false;
            for (const EntryKnownCandidate &Existing : Result) {
                if (Existing.L == L && Existing.Condition == Condition) {
                    Duplicate = true;
                    break;
                }
            }
            if (Duplicate) continue;
            Result.push_back(EntryKnownCandidate{
                L, Condition, AllBlocks, *Proof});
        }
    }
    return Result;
}

std::vector<BasicBlock *> LoopUnswitchPass::collectFunctionBlocks(Function &F) {
    std::vector<BasicBlock *> Blocks;
    for (BasicBlock *BB : F)
        if (BB) Blocks.push_back(BB);
    return Blocks;
}

std::size_t LoopUnswitchPass::weightedRegionSize(const std::vector<BasicBlock *> &Blocks,
                                                 const BlockSet *Filter) {
    std::size_t BlockCount = 0;
    for (BasicBlock *BB : Blocks)
        if (BB && (!Filter || Filter->count(BB))) ++BlockCount;
    return countInstructions(Blocks, Filter) + 3 * BlockCount;
}

bool LoopUnswitchPass::hasLoopTransformProvenance(
    const std::vector<BasicBlock *> &Blocks) {
    for (BasicBlock *BB : Blocks) {
        if (!BB) continue;
        const std::string &Name = BB->getIdent();
        if (Name.find(".unr") != std::string::npos ||
            Name.find(".peel") != std::string::npos) {
            return true;
        }
    }
    return false;
}


unsigned LoopUnswitchPass::invariantScope(Value *Condition, Loop *L,
                        const DominatorTree &DT) {
    unsigned Scope = 1;
    for (Loop *Parent = L ? L->getParentLoop() : nullptr;
         Parent; Parent = Parent->getParentLoop()) {
        CanonicalLoopShape ParentShape;
        if (!getCanonicalLoopShape(Parent, ParentShape) ||
            !ParentShape.PreHeader->getTerminator() ||
            !isAvailableAt(Condition,
                           ParentShape.PreHeader->getTerminator(),
                           *Parent, DT)) {
            break;
        }
        ++Scope;
    }
    return Scope;
}

bool LoopUnswitchPass::chooseDirectionalClonePlan(NonTrivialCandidate &C) {
    const std::size_t TrueWeight =
        weightedRegionSize(C.AllBlocks, &C.TrueReachable);
    const std::size_t FalseWeight =
        weightedRegionSize(C.AllBlocks, &C.FalseReachable);

    // Preserve the larger version in the original layout.  In the corpus this
    // is the loop-body arm of zero-trip guards and is overwhelmingly the hot
    // path.  It also makes the actually cloned side as small as possible.
    C.OriginalValue = TrueWeight >= FalseWeight;
    C.OriginalSucc = C.OriginalValue ? C.TrueSucc : C.FalseSucc;
    C.CloneSucc = C.OriginalValue ? C.FalseSucc : C.TrueSucc;
    C.OriginalReachable = C.OriginalValue ? C.TrueReachable
                                          : C.FalseReachable;
    C.CloneReachable = C.OriginalValue ? C.FalseReachable
                                       : C.TrueReachable;
    return C.OriginalSucc && C.CloneSucc;
}

std::optional<LoopUnswitchPass::NonTrivialCandidate>
LoopUnswitchPass::findBestNonTrivialCandidate(
    Function &F, LoopInfo &LI, const DominatorTree &DT, SCEVResult &SE) {
    std::optional<NonTrivialCandidate> Best;
    auto Consider = [&](NonTrivialCandidate C) {
        const std::int64_t Net = C.Benefit - C.Cost;
        const std::int64_t BestNet =
            Best ? Best->Benefit - Best->Cost
                 : std::numeric_limits<std::int64_t>::min();
        if (!Best || Net > BestNet ||
            (Net == BestNet && C.ControlUses > Best->ControlUses) ||
            (Net == BestNet && C.ControlUses == Best->ControlUses &&
             C.CloneWeight < Best->CloneWeight)) {
            Best = std::move(C);
        }
    };

    const std::vector<BasicBlock *> FunctionBlocks =
        collectFunctionBlocks(F);
    const std::size_t FunctionWeight = weightedRegionSize(FunctionBlocks);
    if (FunctionWeight == 0) return std::nullopt;

    for (Loop *L : LI.getLoopsInPostOrder()) {
        CanonicalLoopShape Shape;
        if (!getCanonicalLoopShape(L, Shape)) continue;

        std::vector<BasicBlock *> AllBlocks = L->getBlocksInFunctionOrder();
        if (AllBlocks.empty()) continue;
        for (BasicBlock *BB : AllBlocks) {
            if (!isDirectLoopBlock(LI, L, BB)) continue;
            BrInst *Br = BB->getBranchTerminator();
            if (!BasicBlock::isCondBr(Br)) continue;
            BasicBlock *T = Br->getTrueSuccessor();
            BasicBlock *Fals = Br->getFalseSuccessor();
            if (!T || !Fals || T == Fals ||
                !L->contains(T) || !L->contains(Fals)) {
                continue;
            }
            Value *Cond = Br->getCondition();
            if (!isBooleanNonConstant(Cond) ||
                !isAvailableAt(Cond, Shape.PreHeader->getTerminator(),
                               *L, DT)) {
                continue;
            }

            NonTrivialCandidate C;
            C.L = L;
            C.Shape = Shape;
            C.Parent = BB;
            C.Branch = Br;
            C.Condition = Cond;
            C.TrueSucc = T;
            C.FalseSucc = Fals;
            C.AllBlocks = AllBlocks;
            C.ControlUses = countControlUses(Cond, *L);
            if (C.ControlUses == 0) continue;
            if (!isMustExecuteBeforeLoopProgress(BB, *L, DT)) {
                continue;
            }

            C.TrueReachable = collectSpecializedReachable(*L, BB, T);
            C.FalseReachable = collectSpecializedReachable(*L, BB, Fals);
            if (!C.TrueReachable.count(Shape.Header) ||
                !C.TrueReachable.count(BB) ||
                !C.FalseReachable.count(Shape.Header) ||
                !C.FalseReachable.count(BB) ||
                !chooseDirectionalClonePlan(C)) {
                continue;
            }
            if (C.CloneReachable.size() > MaxNonTrivialCloneBlocks) {
                continue;
            }
            if (!cloneClosureIsLegal(C)) {
                continue;
            }
            if (!validateVersionedExitPhis(C)) {
                continue;
            }

            C.CloneInstructions =
                countInstructions(AllBlocks, &C.CloneReachable);
            if (C.CloneInstructions > MaxNonTrivialCloneInstructions) {
                continue;
            }
            std::vector<BasicBlock *> CloneBlocks;
            for (BasicBlock *CloneBB : AllBlocks)
                if (CloneBB && C.CloneReachable.count(CloneBB))
                    CloneBlocks.push_back(CloneBB);
            if (hasAnyCall(CloneBlocks)) {
                continue;
            }

            const std::size_t ExitPhiCost = estimateExitPhiCost(*L);
            C.CloneWeight = C.CloneInstructions +
                            3 * C.CloneReachable.size() +
                            4 * ExitPhiCost;
            C.GeneratedByLoopTransform =
                hasLoopTransformProvenance(AllBlocks);
            const std::size_t RelativeLimit =
                C.GeneratedByLoopTransform
                    ? MaxGeneratedClonePercentOfFunction
                    : MaxClonePercentOfFunction;
            if (C.CloneWeight * 100 > FunctionWeight * RelativeLimit) {
                continue;
            }

            std::int64_t TripEstimate = 1;
            if (auto TC = SE.getSmallConstantTripCount(L)) {
                TripEstimate = std::clamp<std::int64_t>(*TC, 1, 64);
            } else if (L->getDepth() >= 2) {
                // Unknown-trip nested loops dominate the profitable corpus
                // cases; keep the estimate bounded so size still wins for a
                // one-use or control-heavy candidate.
                TripEstimate = 8;
            } else {
                TripEstimate = 4;
            }
            const unsigned Scope = invariantScope(Cond, L, DT);
            const std::int64_t InvocationFactor =
                std::int64_t{1} << std::min<unsigned>(Scope - 1, 3);
            C.Benefit = 3 * std::max<std::int64_t>(
                0, TripEstimate * C.ControlUses - 1) * InvocationFactor;
            const std::size_t OriginalWeight =
                weightedRegionSize(AllBlocks, &C.OriginalReachable);
            // A single invariant branch in a very small loop is quickly
            // learned by the predictor; selector/layout overhead can equal
            // the saved instruction.  Require extra headroom unless either
            // multiple control uses disappear or a larger hot body is being
            // specialized.  This separates the tiny transpose shape from the
            // larger game-of-life body without naming either benchmark.
            const std::int64_t SmallBodyLayoutPenalty =
                Scope == 1 && C.ControlUses == 1 && OriginalWeight < 64
                    ? 8
                    : 0;
            C.Cost = static_cast<std::int64_t>(C.CloneWeight) +
                     SmallBodyLayoutPenalty;

            if (C.Benefit < C.Cost + 4) {
                continue;
            }
            Consider(std::move(C));
        }
    }
    return Best;
}

Value *LoopUnswitchPass::mapValue(Value *V, const ValueToValueMap &VMap) {
    auto It = VMap.find(V);
    return It == VMap.end() ? V : It->second;
}

bool LoopUnswitchPass::pruneClonedPhis(ClonedBlockRegion &Region,
                     const NonTrivialCandidate &C,
                     BasicBlock *ClonedPreHeader) {
    std::vector<PhiPrunePlan> Plans;
    for (const auto &Pair : Region.InstPairs) {
        auto *OldPhi = dynamic_cast<PhiInst *>(Pair.first);
        if (!OldPhi) continue;
        auto *NewPhi = dynamic_cast<PhiInst *>(Pair.second);
        if (!NewPhi) return false;
        BasicBlock *OldBB = OldPhi->getParent();

        PhiPrunePlan P;
        P.Original = OldPhi;
        P.Cloned = NewPhi;
        const bool Planned = forEachPhiIncoming(
            OldPhi, [&Region, &C, ClonedPreHeader, OldBB, &P](
                        unsigned, Value *OldValue, BasicBlock *OldPred) {
            BasicBlock *NewPred = nullptr;
            bool Active = false;
            if (OldBB == C.Shape.Header &&
                OldPred == C.Shape.PreHeader) {
                Active = true;
                NewPred = ClonedPreHeader;
            } else if (C.CloneReachable.count(OldPred) &&
                       isActiveSpecializedEdge(
                           OldPred, OldBB, C.Parent, C.CloneSucc)) {
                Active = true;
                NewPred = Region.getClonedBlock(OldPred);
            }
            if (!Active) return true;
            Value *NewValue = mapValue(OldValue, Region.VMap);
            if (!NewPred || !NewValue) return false;
            P.Incoming.push_back({NewValue, NewPred});
            return true;
            });
        if (!Planned) return false;
        Plans.push_back(std::move(P));
    }

    // Collapsing mutually-referential singleton PHIs would make deletion
    // order observable (A -> B, B -> A can leave a dangling replacement).
    // Retained multi-incoming PHIs break such chains, so conservatively reject
    // only cycles made entirely from singleton replacements.
    std::unordered_map<Value *, Value *> SingletonReplacements;
    for (PhiPrunePlan &P : Plans) {
        if (P.Incoming.size() != 1) continue;
        Value *Replacement = P.Incoming.front().V;
        if (Replacement == P.Cloned) {
            Replacement = UndefValue::NewUndefValue(P.Cloned->getIRType());
        }
        SingletonReplacements[P.Cloned] = Replacement;
    }
    for (const auto &Entry : SingletonReplacements) {
        Value *Current = Entry.first;
        std::unordered_set<Value *> Path;
        while (SingletonReplacements.count(Current)) {
            if (!Path.insert(Current).second) return false;
            Current = SingletonReplacements[Current];
        }
    }

    std::unordered_map<Value *, Value *> PhiReplacements;
    for (PhiPrunePlan &P : Plans) {
        if (P.Incoming.empty()) {
            P.Replacement = UndefValue::NewUndefValue(
                P.Cloned->getIRType());
        } else if (P.Incoming.size() == 1) {
            P.Replacement = SingletonReplacements[P.Cloned];
        } else {
            P.NewPhi = new PhiInst(P.Cloned->getIRType());
            P.NewPhi->setIdent(P.Cloned->getIdent());
            P.Replacement = P.NewPhi;
        }
        PhiReplacements[P.Cloned] = P.Replacement;
    }

    auto Resolve = [&](Value *V) {
        std::unordered_set<Value *> Seen;
        while (V && PhiReplacements.count(V) && Seen.insert(V).second) {
            Value *Next = PhiReplacements[V];
            if (Next == V) break;
            V = Next;
        }
        return V;
    };

    for (PhiPrunePlan &P : Plans) {
        if (P.NewPhi) {
            for (const auto &In : P.Incoming) {
                Value *Resolved = Resolve(In.V);
                if (!Resolved) return false;
                P.NewPhi->addIncoming(Resolved, In.Pred);
            }
        } else {
            P.Replacement = Resolve(P.Replacement);
            if (!P.Replacement) return false;
            PhiReplacements[P.Cloned] = P.Replacement;
        }
        Region.VMap[P.Original] = P.Replacement;
    }

    for (PhiPrunePlan &P : Plans) {
        Value *Replacement = P.NewPhi
                                 ? static_cast<Value *>(P.NewPhi)
                                 : P.Replacement;
        P.Cloned->replaceAllUsesWith(Replacement);
        if (P.NewPhi) P.Cloned->replaceWith(P.NewPhi);
        delete P.Cloned;
    }
    return true;
}

void LoopUnswitchPass::destroyBlocks(std::vector<BasicBlock *> Blocks) {
    for (BasicBlock *BB : Blocks) {
        if (!BB) continue;
        for (Instruction *I : *BB)
            if (I) I->clearOperands();
    }
    for (BasicBlock *BB : Blocks)
        if (BB) BB->clear();
    for (BasicBlock *BB : Blocks) delete BB;
}

PhiInst *LoopUnswitchPass::buildExitProxyValue(
    const std::shared_ptr<IRType> &Ty,
    const std::vector<PhiInst::IncomingEdge> &Incoming,
    BasicBlock *Proxy, const std::string &Name) {
    if (Incoming.empty() || !Proxy) return nullptr;
    auto *Phi = new PhiInst(Ty);
    Phi->setIdent(Name);
    for (const auto &In : Incoming) Phi->addIncoming(In.V, In.Pred);
    Proxy->pushFront(Phi);
    return Phi;
}

std::optional<LoopUnswitchPass::VersionedExitPlan>
LoopUnswitchPass::planVersionedExit(
    BasicBlock *Exit, const NonTrivialCandidate &C,
    ClonedBlockRegion &Region) {
    if (!Exit) return std::nullopt;

    VersionedExitPlan Plan;
    Plan.Exit = Exit;

    std::vector<BasicBlock *> CloneSourcePreds;
    for (BasicBlock *BB : C.AllBlocks) {
        if (!BB || BB->countEdgesToSucc(Exit) == 0) continue;
        if (C.OriginalReachable.count(BB)) {
            if (BB->countEdgesToSucc(Exit) != 1) return std::nullopt;
            Plan.OriginalPreds.push_back(BB);
        }
        if (C.CloneReachable.count(BB)) CloneSourcePreds.push_back(BB);
    }

    for (BasicBlock *SourcePred : CloneSourcePreds) {
        BasicBlock *Pred = Region.getClonedBlock(SourcePred);
        if (!Pred || Pred->countEdgesToSucc(Exit) != 1)
            return std::nullopt;
        Plan.ClonePreds.push_back(Pred);
    }

    for (PhiInst *OldPhi : Exit->collectPhiNodes()) {
        if (!OldPhi) return std::nullopt;
        VersionedExitPhiPlan PhiPlan;
        PhiPlan.OldPhi = OldPhi;
        for (const auto &In : OldPhi->collectIncomingEdges()) {
            if (!In.V || !In.Pred) return std::nullopt;
            if (C.OriginalReachable.count(In.Pred) &&
                In.Pred->countEdgesToSucc(Exit) > 0) {
                PhiPlan.OriginalIncoming.push_back(In);
            }
            if (C.CloneReachable.count(In.Pred)) {
                BasicBlock *MappedPred = Region.getClonedBlock(In.Pred);
                Value *MappedValue = mapValue(In.V, Region.VMap);
                if (MappedPred && MappedPred->countEdgesToSucc(Exit) > 0 &&
                    MappedValue) {
                    PhiPlan.CloneIncoming.push_back(
                        {MappedValue, MappedPred});
                }
            }
        }

        if ((!Plan.OriginalPreds.empty() &&
             PhiPlan.OriginalIncoming.size() !=
                 Plan.OriginalPreds.size()) ||
            (!Plan.ClonePreds.empty() &&
             PhiPlan.CloneIncoming.size() != Plan.ClonePreds.size())) {
            return std::nullopt;
        }
        Plan.PhiPlans.push_back(std::move(PhiPlan));
    }

    return Plan;
}

void LoopUnswitchPass::applyVersionedExitPlan(Function &F, VersionedExitPlan &Plan) {
    BasicBlock *Exit = Plan.Exit;
    if (!Exit || (Plan.OriginalPreds.empty() && Plan.ClonePreds.empty()))
        return;

    BasicBlock *OriginalProxy = nullptr;
    BasicBlock *CloneProxy = nullptr;
    if (!Plan.OriginalPreds.empty()) {
        OriginalProxy = new BasicBlock();
        F.pushBlock(OriginalProxy);
        OriginalProxy->setIdent(F.makeUniqueBlockIdent(
            Exit->getIdent() + ".unswitch.original.exit"));
    }
    if (!Plan.ClonePreds.empty()) {
        CloneProxy = new BasicBlock();
        F.pushBlock(CloneProxy);
        CloneProxy->setIdent(F.makeUniqueBlockIdent(
            Exit->getIdent() + ".unswitch.clone.exit"));
    }

    for (BasicBlock *Pred : Plan.OriginalPreds) {
        const bool Redirected =
            BasicBlock::redirectBranchTarget(Pred, Exit, OriginalProxy);
        assert(Redirected && "preflighted original exit edge disappeared");
        (void)Redirected;
    }
    for (BasicBlock *Pred : Plan.ClonePreds) {
        const bool Redirected =
            BasicBlock::redirectBranchTarget(Pred, Exit, CloneProxy);
        assert(Redirected && "preflighted cloned exit edge disappeared");
        (void)Redirected;
    }

    for (VersionedExitPhiPlan &PhiPlan : Plan.PhiPlans) {
        PhiInst *OldPhi = PhiPlan.OldPhi;
        std::vector<PhiInst::IncomingEdge> MergeIncoming;
        if (OriginalProxy) {
            PhiInst *V = buildExitProxyValue(
                OldPhi->getIRType(), PhiPlan.OriginalIncoming, OriginalProxy,
                OldPhi->getIdent() + ".unswitch.original");
            assert(V && "preflighted original exit PHI has no incoming");
            MergeIncoming.push_back({V, OriginalProxy});
        }
        if (CloneProxy) {
            PhiInst *V = buildExitProxyValue(
                OldPhi->getIRType(), PhiPlan.CloneIncoming, CloneProxy,
                OldPhi->getIdent() + ".unswitch.clone");
            assert(V && "preflighted cloned exit PHI has no incoming");
            MergeIncoming.push_back({V, CloneProxy});
        }

        assert(!MergeIncoming.empty() && "versioned exit lost every version");
        if (MergeIncoming.size() == 1) {
            OldPhi->replaceAllUsesWith(MergeIncoming.front().V);
            delete OldPhi;
        } else {
            auto *MergePhi = new PhiInst(OldPhi->getIRType());
            MergePhi->setIdent(OldPhi->getIdent() + ".unswitch.merge");
            for (const auto &In : MergeIncoming)
                MergePhi->addIncoming(In.V, In.Pred);
            OldPhi->replaceAllUsesWith(MergePhi);
            OldPhi->replaceWith(MergePhi);
            delete OldPhi;
        }
    }

    if (OriginalProxy) OriginalProxy->genUnCondInst(Exit);
    if (CloneProxy) CloneProxy->genUnCondInst(Exit);
}

void LoopUnswitchPass::eraseDeadOriginalBlocks(const std::vector<BasicBlock *> &AllBlocks,
                             const BlockSet &LiveBlocks) {
    BlockSet Dead;
    for (BasicBlock *BB : AllBlocks)
        if (BB && !LiveBlocks.count(BB)) Dead.insert(BB);
    if (Dead.empty()) return;

    for (BasicBlock *BB : Dead) {
        for (BasicBlock *Succ : BB->getSuccessors()) {
            if (!Succ || Dead.count(Succ)) continue;
            BasicBlock::removeEdgeFromSuccPhis(
                Succ, BB, BB->countEdgesToSucc(Succ));
        }
    }
    for (BasicBlock *BB : AllBlocks) {
        if (BB && Dead.count(BB)) BB->eraseBlockSafely();
    }
}

bool LoopUnswitchPass::transformEntryKnown(const EntryKnownCandidate &C) {
    if (!C.Condition || !C.L) return false;
    BlockSet Blocks;
    for (BasicBlock *BB : C.AllBlocks)
        if (BB) Blocks.insert(BB);
    return replaceConditionUsesInBlocks(C.Condition, Blocks, C.KnownValue);
}

bool LoopUnswitchPass::transformNonTrivial(Function &F, NonTrivialCandidate &C) {
    if (!C.L || !C.Shape.Header || !C.Shape.PreHeader || !C.Parent ||
        !C.Branch || !C.TrueSucc || !C.FalseSucc || !C.OriginalSucc ||
        !C.CloneSucc) {
        return false;
    }
    BrInst *OldPreBr = C.Shape.PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(OldPreBr) ||
        OldPreBr->getTrueSuccessor() != C.Shape.Header ||
        C.Parent->getBranchTerminator() != C.Branch ||
        !BasicBlock::isCondBr(C.Branch) ||
        C.Branch->getCondition() != C.Condition ||
        C.Branch->getTrueSuccessor() != C.TrueSucc ||
        C.Branch->getFalseSuccessor() != C.FalseSucc) {
        return false;
    }

    auto *OriginalPreHeader = new BasicBlock();
    F.pushBlock(OriginalPreHeader);
    OriginalPreHeader->setIdent(F.makeUniqueBlockIdent(
        C.Shape.Header->getIdent() + ".unswitch.original.preheader"));
    auto *ClonedPreHeader = new BasicBlock();
    F.pushBlock(ClonedPreHeader);
    ClonedPreHeader->setIdent(F.makeUniqueBlockIdent(
        C.Shape.Header->getIdent() +
        (C.OriginalValue ? ".unswitch.false.preheader"
                         : ".unswitch.true.preheader")));

    std::vector<BasicBlock *> CloneBlocks;
    for (BasicBlock *BB : C.AllBlocks)
        if (BB && C.CloneReachable.count(BB)) CloneBlocks.push_back(BB);

    ValueToValueMap InitialMap;
    InitialMap[C.Shape.PreHeader] = ClonedPreHeader;
    CloneBlockRegionOptions Options;
    Options.MakeBlockIdent = [&F, &C](const BasicBlock *BB, std::size_t) {
        const std::string Base = BB ? BB->getIdent() : "bb";
        return F.makeUniqueBlockIdent(
            Base + (C.OriginalValue ? ".unswitch.false"
                                    : ".unswitch.true"));
    };
    auto Region = cloneBasicBlockRegion(F, CloneBlocks, InitialMap, Options);
    if (!Region) {
        delete OriginalPreHeader;
        delete ClonedPreHeader;
        return false;
    }

    auto RollbackClones = [&]() {
        destroyBlocks(Region->ClonedBlocks);
        delete OriginalPreHeader;
        delete ClonedPreHeader;
    };

    BasicBlock *ClonedHeader = Region->getClonedBlock(C.Shape.Header);
    BasicBlock *ClonedParent = Region->getClonedBlock(C.Parent);
    BasicBlock *ClonedChosenSucc = Region->getClonedBlock(C.CloneSucc);
    if (!ClonedHeader || !ClonedParent || !ClonedChosenSucc) {
        RollbackClones();
        return false;
    }
    BrInst *ClonedBranch = ClonedParent->getBranchTerminator();
    if (!BasicBlock::isCondBr(ClonedBranch) ||
        !BasicBlock::replaceCondBrWithUncond(
            ClonedParent, ClonedChosenSucc) ||
        !pruneClonedPhis(*Region, C, ClonedPreHeader)) {
        RollbackClones();
        return false;
    }

    std::vector<VersionedExitPlan> ExitPlans;
    for (BasicBlock *Exit : C.L->getExitBlocks()) {
        auto Plan = planVersionedExit(Exit, C, *Region);
        if (!Plan) {
            RollbackClones();
            return false;
        }
        ExitPlans.push_back(std::move(*Plan));
    }

    // All remaining operations were preflighted before the first mutation of
    // the original CFG.  From here onward the transform commits atomically at
    // the pass level.
    setHeaderPredecessor(C.Shape.Header, C.Shape.PreHeader,
                         OriginalPreHeader);
    OriginalPreHeader->genUnCondInst(C.Shape.Header);
    ClonedPreHeader->genUnCondInst(ClonedHeader);

    BasicBlock::removeEdgeFromSuccPhis(C.CloneSucc, C.Parent, 1);
    const bool SpecializedOriginal =
        BasicBlock::replaceCondBrWithUncond(C.Parent, C.OriginalSucc);
    assert(SpecializedOriginal &&
           "preflighted original unswitch branch disappeared");
    (void)SpecializedOriginal;

    BlockSet OriginalVersionBlocks = C.OriginalReachable;
    BlockSet ClonedVersionBlocks;
    for (BasicBlock *BB : C.CloneReachable) {
        if (BasicBlock *Mapped = Region->getClonedBlock(BB))
            ClonedVersionBlocks.insert(Mapped);
    }
    replaceConditionUsesInBlocks(C.Condition, OriginalVersionBlocks,
                                 C.OriginalValue);
    replaceConditionUsesInBlocks(C.Condition, ClonedVersionBlocks,
                                 !C.OriginalValue);

    for (VersionedExitPlan &Plan : ExitPlans)
        applyVersionedExitPlan(F, Plan);

    BasicBlock *TruePreHeader =
        C.OriginalValue ? OriginalPreHeader : ClonedPreHeader;
    BasicBlock *FalsePreHeader =
        C.OriginalValue ? ClonedPreHeader : OriginalPreHeader;
    const bool InstalledSelector = C.Shape.PreHeader->replaceTerminator(
        new BrInst(C.Condition, TruePreHeader, FalsePreHeader));
    assert(InstalledSelector && "preflighted loop preheader lost terminator");
    (void)InstalledSelector;

    eraseDeadOriginalBlocks(C.AllBlocks, C.OriginalReachable);
    return true;
}

PassResult LoopUnswitchPass::run(Function &F,
                                 FunctionAnalysisPassManager &FAM) {
    FunctionState &State = States[&F];

    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);

    if (State.Kind != SelectedKind::NonTrivial) {
        auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
        if (auto Candidate = findTrivialCandidate(F, LI, DT, MSSA)) {
            if (transformTrivial(F, *Candidate)) {
                State.Kind = SelectedKind::Trivial;
                return PassResult::changed(PreservationStatus::none());
            }
        }
    }

    // Once this function selected trivial unswitching, later fixed-point
    // rounds may find another trivial branch but must never switch modes.
    if (State.Kind == SelectedKind::Trivial)
        return PassResult::unchanged();

    // Entry facts are proof-driven and never clone code.  Apply every fact in
    // one batch so the three outer fixed-point rounds are not consumed one at
    // a time, and keep their quota independent from Full Unswitch.
    bool EntryChanged = false;
    for (const EntryKnownCandidate &Candidate :
         findEntryKnownCandidates(LI, DT)) {
        if (!transformEntryKnown(Candidate)) continue;
        EntryChanged = true;
    }
    if (EntryChanged) {
        State.Kind = SelectedKind::NonTrivial;
        return PassResult::changed(PreservationStatus::none());
    }

    // Keep code-growing versioning bounded.  Entry-known remains available in
    // later rounds even after this quota is exhausted.
    if (State.FullCloneCount != 0)
        return PassResult::unchanged();

    auto Candidate = findBestNonTrivialCandidate(F, LI, DT, SE);
    if (!Candidate) return PassResult::unchanged();

    if (!transformNonTrivial(F, *Candidate))
        return PassResult::unchanged();

    State.Kind = SelectedKind::NonTrivial;
    ++State.FullCloneCount;
    return PassResult::changed(PreservationStatus::none());
}
