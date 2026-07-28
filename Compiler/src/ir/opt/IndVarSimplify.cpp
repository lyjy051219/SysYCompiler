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

// 步骤 1.1：比较递推更新的操作数时，仅允许两个递推 PHI 互相对应，其他值
// 必须是同一个 SSA 定义。
bool IndVarSimplifyPass::correspondingRecurrenceValue(
    Value *A, Value *B, PhiInst *APhi, PhiInst *BPhi) {
    if (A == APhi || B == BPhi) return A == APhi && B == BPhi;
    return A == B;
}

// 步骤 1.2：核验更新指令的类型和操作码，并仅对可交换运算允许交换操作数。
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

// 步骤 1.3：按前驱逐项比较两个头部 PHI；循环外起始值必须相同，循环内
// 回边值必须是步骤 1.2 认可的同形更新。
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

// 步骤 1—2：SCEV 键只负责快速发现候选，同键 PHI 仍须通过精确 SSA 核验；
// 通过后统一替换使用并删除重复 PHI。
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

// 步骤 3：利用已验证的退出信息识别右侧归纳变量，反转谓词和操作数，生成
// 语义等价的规范比较；不改变退出边或循环迭代域。
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
    // 1. 按内到外顺序寻找可规范化循环；含计时调用的循环拒绝删除或重排
    // 每轮指令，以保持计时边界内的动态执行次数。
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
        if (HasTimingCall) continue;
        // 2—3. 先合并重复递推，否则规范退出比较；提交首个变化后立刻停止。
        if (mergeExactRecurrences(L, SE) || orientExitComparison(L, SE)) {
            Changed = true;
            break;
        }
    }

    if (!Changed) return PassResult::unchanged();
    // 4. 当前 SCEV 缓存描述改写前的 SSA，交由外层不动点重新构建；CFG 与
    // 循环结构未变，但指针递推替换可能改变别名和副作用结论。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PassResult::changed(std::move(PA));
}
