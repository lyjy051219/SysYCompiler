#include "ir/opt/IndVarSimplify.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

bool IndVarSimplifyPass::correspondingRecurrenceValue(
    Value *A, Value *B, PhiInst *APhi, PhiInst *BPhi) {
    if (A == APhi || B == BPhi) return A == APhi && B == BPhi;
    return A == B;
}

bool IndVarSimplifyPass::haveExactUpdateShape(
    Value *A, Value *B, PhiInst *APhi, PhiInst *BPhi) {
    auto *AI = dynamic_cast<BinaryInst *>(A);
    auto *BI = dynamic_cast<BinaryInst *>(B);
    if (!AI || !BI || AI->instType != BI->instType ||
        AI->getOp() != BI->getOp() || !AI->getIRType() ||
        !BI->getIRType() ||
        !AI->getIRType()->hasSameShape(*BI->getIRType()))
        return false;

    Value *AL = AI->getOperand(0);
    Value *AR = AI->getOperand(1);
    Value *BL = BI->getOperand(0);
    Value *BR = BI->getOperand(1);
    if (correspondingRecurrenceValue(AL, BL, APhi, BPhi) &&
        correspondingRecurrenceValue(AR, BR, APhi, BPhi))
        return true;

    return BinaryInst::isCommutativeOp(AI->getOp()) &&
           correspondingRecurrenceValue(AL, BR, APhi, BPhi) &&
           correspondingRecurrenceValue(AR, BL, APhi, BPhi);
}

// SCEV is used to group candidates, but equality is committed only after an
// exact SSA recurrence check.  This keeps signed/unsigned operations and
// casts distinct even if a future analysis canonicalization happens to give
// them the same mathematical key.
bool IndVarSimplifyPass::haveExactRecurrenceShape(
    PhiInst *A, PhiInst *B, Loop *L) {
    if (!A || !B || !L || A->getParent() != B->getParent() ||
        A->getNumIncomingValues() != B->getNumIncomingValues())
        return false;

    std::unordered_set<BasicBlock *> Seen;
    if (!forEachPhiIncoming(
            A, [&](unsigned, Value *AV, BasicBlock *Pred) {
                if (!Pred || !Seen.insert(Pred).second) return false;
                Value *BV = B->getIncomingFromPred(Pred);
                if (!AV || !BV) return false;

                if (!L->contains(Pred)) return AV == BV;
                return haveExactUpdateShape(AV, BV, A, B);
            })) {
        return false;
    }
    return Seen.size() == B->getNumIncomingValues();
}

bool IndVarSimplifyPass::mergeExactRecurrences(Loop *L, SCEVResult &SE) {
    BasicBlock *Header = L ? L->getHeader() : nullptr;
    if (!Header) return false;

    bool Changed = false;
    std::unordered_map<std::string, PhiInst *> Leaders;
    std::vector<PhiInst *> Phis = Header->collectPhiNodes();
    for (PhiInst *Phi : Phis) {
        if (!Phi || !Phi->getParent() || !Phi->getIRType() ||
            !Phi->getIRType()->isInt()) {
            continue;
        }
        const SCEVExpr *E = SE.getSCEV(Phi);
        if (!E || !E->isRecurrence() || E->RecLoop != L || E->Key.empty())
            continue;

        auto [It, Inserted] = Leaders.emplace(E->Key, Phi);
        if (Inserted) continue;
        PhiInst *Leader = It->second;
        if (!Leader || Leader == Phi ||
            Leader->getIRType() != Phi->getIRType() ||
            !haveExactRecurrenceShape(Leader, Phi, L)) {
            continue;
        }

        Phi->replaceAllUsesWith(Leader);
        delete Phi;
        Changed = true;
    }
    return Changed;
}

bool IndVarSimplifyPass::orientExitComparison(Loop *L, SCEVResult &SE) {
    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->Valid || !TCI->ExitCmp ||
        !TCI->ExitCmp->getParent()) {
        return false;
    }

    BinaryInst *Cmp = TCI->ExitCmp;
    Value *LHS = Cmp->getOperand(0);
    Value *RHS = Cmp->getOperand(1);
    if (!LHS || !RHS || !BinaryInst::isCompareOp(Cmp->getOp()) ||
        !LHS->getIRType() || !LHS->getIRType()->isInt()) {
        return false;
    }

    auto LAffine = SE.getAffine(LHS);
    auto RAffine = SE.getAffine(RHS);
    const bool LeftIsIV = LAffine && LAffine->RecLoop == L;
    const bool RightIsIV = RAffine && RAffine->RecLoop == L;
    if (LeftIsIV || !RightIsIV) return false;

    auto *Canonical = new BinaryInst(
        RHS, BinaryInst::reversePredicate(Cmp->getOp()), LHS);
    if (!Instruction::insertBefore(Cmp, Canonical)) {
        delete Canonical;
        return false;
    }
    Cmp->replaceAllUsesWith(Canonical);
    delete Cmp;
    return true;
}

PassResult IndVarSimplifyPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);

    bool Changed = false;
    // Do not continue using SCEV after a rewrite: its cache refers to the old
    // SSA graph.  A surrounding fixed point will rebuild analyses and visit
    // remaining loops deterministically.
    for (Loop *L : LI.getLoopsInPostOrder()) {
        if (!L) continue;
        bool HasTimingCall = false;
        for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
            if (!BB) continue;
            for (Instruction *I : *BB) {
                auto *CI = dynamic_cast<CallInst *>(I);
                if (CI && CI->mayReachTimingCall()) {
                    HasTimingCall = true;
                    break;
                }
            }
            if (HasTimingCall) break;
        }
        // 合并递推会删除一套每轮更新，计时循环中必须保留原执行次数。
        if (HasTimingCall) continue;
        if (mergeExactRecurrences(L, SE) || orientExitComparison(L, SE)) {
            Changed = true;
            break;
        }
    }

    if (!Changed) return PassResult::unchanged();
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PassResult::changed(std::move(PA));
}
