#include "ir/opt/LoopDeletion.hpp"

#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using PhiReplacementMap = std::unordered_map<PhiInst *, Value *>;

std::vector<BasicBlock *> collectLoopBlocks(Loop *L, Function *F) {
    std::vector<BasicBlock *> Result;
    if (!L || !F) return Result;
    std::unordered_set<BasicBlock *> InLoop;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops())
        if (BB && BB->getParent() == F) InLoop.insert(BB);

    // Function order makes deletion deterministic and avoids dereferencing
    // loop-analysis nodes after their blocks have been destroyed.
    for (BasicBlock *BB : *F)
        if (InLoop.count(BB)) Result.push_back(BB);
    return Result;
}

bool isSafeToSkipOnZeroTrip(Instruction *I) {
    if (!I) return false;
    if (dynamic_cast<PhiInst *>(I) || dynamic_cast<BrInst *>(I)) return true;
    // Use the same observable-execution policy as DCE/ADCE.  Ordinary pure
    // operations may disappear with a dead loop; future volatile/atomic or
    // constrained instructions must opt out through this shared predicate.
    if (dceInstructionMustBePreserved(I)) return false;
    if (dynamic_cast<BinaryInst *>(I)) return true;
    if (dynamic_cast<SelectInst *>(I) ||
        dynamic_cast<GetElementPtrInst *>(I)) {
        return true;
    }
    switch (I->instType) {
    case Instruction::TRUNC:
    case Instruction::ZEXT:
    case Instruction::SEXT:
    case Instruction::FPTOSI:
    case Instruction::SITOFP:
    case Instruction::BITCAST:
        return true;
    default:
        // Calls, memory operations, allocation and returns may be observable,
        // trap, or fail to return.  The IR has no speculatable/willreturn
        // attributes, so skipping them would be a guess.
        return false;
    }
}

bool hasNoObservableLoopInstruction(Loop *L) {
    if (!L) return false;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (!isSafeToSkipOnZeroTrip(I)) return false;
        }
    }
    return true;
}

Value *resolveZeroTripValue(Value *V, Loop *L, BasicBlock *PreHeader,
                            std::unordered_set<Value *> &Active) {
    if (!V || !L || !PreHeader) return nullptr;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || !L->contains(I->getParent())) return V;

    // On the zero-trip edge only a header PHI has an unambiguous value: its
    // preheader incoming.  Do not try to symbolically execute arbitrary
    // header instructions here.
    auto *Phi = dynamic_cast<PhiInst *>(I);
    if (!Phi || Phi->getParent() != L->getHeader()) return nullptr;
    if (!Active.insert(V).second) return nullptr;
    Value *Incoming = Phi->getIncomingFromPred(PreHeader);
    Value *Resolved = resolveZeroTripValue(Incoming, L, PreHeader, Active);
    Active.erase(V);
    return Resolved;
}

bool planZeroTripPhiReplacements(Loop *L,
                                 const LoopTripCountInfo &TCI,
                                 PhiReplacementMap &Plan) {
    if (!L || !TCI.PreHeader || !TCI.ExitingBlock || !TCI.ExitBlock)
        return false;

    for (PhiInst *Phi : TCI.ExitBlock->collectPhiNodes()) {
        if (!Phi) return false;
        Value *OnExit = Phi->getIncomingFromPred(TCI.ExitingBlock);
        if (!OnExit) return false;
        std::unordered_set<Value *> Active;
        Value *Replacement =
            resolveZeroTripValue(OnExit, L, TCI.PreHeader, Active);
        if (!Replacement || !Replacement->getIRType() ||
            !Phi->getIRType() ||
            Replacement->getIRType()->getEnum() !=
                Phi->getIRType()->getEnum() ||
            Replacement->getIRType()->getSize() !=
                Phi->getIRType()->getSize()) {
            return false;
        }
        Plan[Phi] = Replacement;
    }
    return true;
}

bool planDeadLoopPhiReplacements(Loop *L, BasicBlock *Exit,
                                 PhiReplacementMap &Plan) {
    if (!L || !Exit) return false;
    for (PhiInst *Phi : Exit->collectPhiNodes()) {
        if (!Phi) return false;
        unsigned N = Phi->getNumIncomingValues();
        if (N == 0) {
            Plan[Phi] = UndefValue::NewUndefValue(Phi->getIRType());
            continue;
        }

        Value *Keep = Phi->getIncomingValue(0);
        if (!Keep) return false;
        for (unsigned I = 1; I < N; ++I)
            if (Phi->getIncomingValue(I) != Keep) return false;

        auto *Def = dynamic_cast<Instruction *>(Keep);
        if (Def && Def->getParent() && L->contains(Def->getParent()))
            return false;
        Plan[Phi] = Keep;
    }
    return true;
}

bool outsideUsesAreCovered(
    Loop *L, const std::vector<BasicBlock *> &Blocks,
    const PhiReplacementMap &Plan) {
    if (!L) return false;
    for (BasicBlock *BB : Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            for (auto It = I->getUseList().begin();
                 It != I->getUseList().end(); ++It) {
                Use *U = *It;
                if (!U || U->getValue() != I) continue;
                auto *UserI = dynamic_cast<Instruction *>(U->getUser());
                if (!UserI || !UserI->getParent()) return false;
                if (L->contains(UserI->getParent())) continue;
                auto *Phi = dynamic_cast<PhiInst *>(UserI);
                if (!Phi || !Plan.count(Phi)) return false;
            }
        }
    }
    return true;
}

void applyPhiReplacements(const PhiReplacementMap &Plan) {
    for (const auto &KV : Plan) {
        PhiInst *Phi = KV.first;
        Value *Replacement = KV.second;
        if (Phi && Replacement) Phi->replaceAllUsesWith(Replacement);
    }
    for (const auto &KV : Plan)
        if (KV.first) delete KV.first;
}

bool deleteLoopCFG(Loop *L, Function &F, BasicBlock *PreHeader,
                   BasicBlock *Exit,
                   const std::vector<BasicBlock *> &LoopBlocks,
                   const PhiReplacementMap &PhiPlan) {
    if (!L || !PreHeader || !Exit || LoopBlocks.empty()) return false;
    auto *OldTerm = PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(OldTerm) ||
        OldTerm->getSuccessor(0) != L->getHeader()) {
        return false;
    }

    if (!PreHeader->replaceTerminator(new BrInst(Exit))) return false;
    applyPhiReplacements(PhiPlan);

    // Tear down in three phases.  Deleting one BasicBlock while another loop
    // terminator/PHI still names it would make Value::~Value recursively
    // destroy that user and leave the remaining block list half-mutated.
    for (BasicBlock *BB : LoopBlocks) {
        if (!BB || BB->getParent() != &F) continue;
        for (Instruction *I : *BB) {
            if (!I || !I->getIRType() ||
                I->getIRType()->getEnum() == IR_VOID) {
                continue;
            }
            if (!I->getUseList().is_empty())
                I->replaceAllUsesWith(
                    UndefValue::NewUndefValue(I->getIRType()));
        }
    }
    for (BasicBlock *BB : LoopBlocks) {
        if (BB && BB->getParent() == &F) BB->clear();
    }
    for (BasicBlock *BB : LoopBlocks)
        if (BB && BB->getParent() == &F) delete BB;
    return true;
}

} // namespace

PreservationStatus LoopDeletionPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);

    bool Changed = false;
    // One CFG deletion invalidates every Loop* in the snapshot.  The enclosing
    // bounded fixed point will rebuild analyses and find another candidate.
    for (Loop *L : LI.getLoopsInPostOrder(true)) {
        if (!L || !L->getHeader() || !L->getSubLoops().empty()) continue;
        if (L->getHeader()->getParent() != &F) continue;
        if (!LCSSAPass::isLCSSAForm(L)) continue;

        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
            !PreHeader || !Latch) {
            continue;
        }

        auto TCI = SE.analyzeLoopTripCount(L);
        // Deleting a side-effect-free loop still changes semantics if it may
        // be infinite.  A proven finite constant count is the deliberately
        // conservative progress proof used here.
        if (!TCI || !TCI->Valid || !TCI->ConstantTripCount ||
            *TCI->ConstantTripCount < 0 || !TCI->ProvenNoWrap ||
            !TCI->ExitBlock) {
            continue;
        }

        std::vector<BasicBlock *> Blocks = collectLoopBlocks(L, &F);
        if (Blocks.empty()) continue;

        PhiReplacementMap PhiPlan;
        bool ZeroTrip = *TCI->ConstantTripCount == 0;
        if (ZeroTrip) {
            // A header-tested zero-trip loop still executes its header once.
            // Only bypass it when that header has no observable/trapping work.
            bool HeaderSafe = true;
            for (Instruction *I : *L->getHeader())
                if (!isSafeToSkipOnZeroTrip(I)) {
                    HeaderSafe = false;
                    break;
                }
            if (!HeaderSafe ||
                !planZeroTripPhiReplacements(L, *TCI, PhiPlan)) {
                continue;
            }
        } else {
            if (!hasNoObservableLoopInstruction(L) ||
                !planDeadLoopPhiReplacements(L, TCI->ExitBlock, PhiPlan)) {
                continue;
            }
        }

        if (!outsideUsesAreCovered(L, Blocks, PhiPlan)) continue;
        if (!deleteLoopCFG(L, F, PreHeader, TCI->ExitBlock,
                           Blocks, PhiPlan)) {
            continue;
        }
        Changed = true;
        break;
    }

    if (!Changed) return PreservationStatus::all();
    return PreservationStatus::none();
}
