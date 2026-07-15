#include "ir/opt/ADCE.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/TraverseTools.hpp"

#include <vector>

// =====================================================================
// ADCE — Aggressive Dead Code Elimination
//
// Mark-Sweep 算法:
//   1. Initialize: 收集 root-live 指令 (store, ret, 有副作用 call, 等)
//   2. Mark: worklist 反向传播活性
//      - 数据依赖: 保活 Instruction 操作数
//      - 控制依赖: 沿 postdom 链保活条件分支
//   3. Sweep:
//      - 死条件分支 → pickSurvivorSuccessor 选一个原始后继, 改无条件跳转
//      - 死非 terminator → replaceAllUsesWith(Undef) + delete
//
// 后续 CFGSimplifyPass 负责清理不可达块和 stale PHI。
// =====================================================================

// callHasObservableSideEffect [F1 修正]
// 只有明确标记 NoModRef 的 builtin 才可删。
// 用户函数调用全部 root-live。
// 理由: SideEffectAnalysis 对自递归调用跳过 (SideEffect.cpp:277),
//       getFunctionModRefInfo 不证明可终止。
bool ADCEPass::callHasObservableSideEffect(CallInst *CI) {
    Value *Callee = CI ? CI->getCallee() : nullptr;
    if (!Callee) return true;

    // Case 1: 内置函数
    if (auto *BIF = dynamic_cast<BuildInFunction *>(Callee)) {
        FuncAttr attr = classifyBuiltin(BIF);
        // 只有明确标记 NoModRef 的 builtin 才可删
        if (attr == FuncAttr::NoModRef)
            return false;
        // Sylib, MemReadOnly, MemWriteOnly, MemReadWrite, Untracked → 全部保活
        return true;
    }

    // Case 2: 用户定义函数 → 全部 root-live
    if (dynamic_cast<Function *>(Callee)) {
        return true;
    }

    // Case 3: 无法识别 → 保守
    return true;
}

// isRootLive [F5 + OQ 修正]
bool ADCEPass::isRootLive(
    Instruction *I, PostDominatorTree &PDT) {
    // 1. StoreInst → always root live
    if (dynamic_cast<StoreInst *>(I)) return true;

    // 2. RetInst → always root live
    if (dynamic_cast<RetInst *>(I)) return true;

    // Preserve instruction kinds with explicit observable execution.  Pure
    // operations that merely produce poison/undefined results are removable
    // when dead, matching local DCE.
    if (dceInstructionMustBePreserved(I)) return true;

    // 3. CallInst → 看副作用
    if (auto *CI = dynamic_cast<CallInst *>(I))
        return callHasObservableSideEffect(CI);

    // 4. BrInst
    if (auto *BI = dynamic_cast<BrInst *>(I)) {
        // 4a. 无条件分支 → always root live
        if (!BI->isConditional()) return true;

        // 4b. [OQ 修正] reverse-unreachable 块的 terminator → root live
        //     这些块 entry 可达但到不了 exit，ipostdom 关系不可靠
        //     保守保留其 CFG 结构
        BasicBlock *BB = BI->getParent();
        if (BB && !PDT.isReachableToExit(BB)) return true;

        // 4c. [F5 修正] 不再特判 entry block
        //     控制依赖会在有活指令依赖 entry 分支时自然保活它

        // 4d. 其他条件分支 → 不自动保活，等控制依赖判定
        return false;
    }

    // 5. 其他指令 → 不是 root
    return false;
}

// markLive
void ADCEPass::markLive(Instruction *I, ADCEContext &ctx) {
    if (!ctx.LiveInsts.insert(I).second)
        return;
    ctx.Worklist.push_back(I);
    BasicBlock *BB = I->getParent();
    if (BB) ctx.LiveBlocks.insert(BB);
}

// markControlDependences [OQ 修正]
//
// 控制依赖(B) ≈ postdom frontier(B) 的反向定义。
// 从 B 的每个前驱 P 开始，沿 ipostdom 链走到 ipostdom(B)，
// 路径上每个条件分支都控制 B 的执行 → 保活。
void ADCEPass::markControlDependences(
    BasicBlock *B, PostDominatorTree &PDT, ADCEContext &ctx) {
    if (!B) return;
    if (!ctx.CDProcessed.insert(B).second) return;

    // [OQ] reverse-unreachable 块的 postdom 关系不可靠，跳过
    // 其 terminator 已在 initialize 被 root-live 保活
    if (!PDT.isReachableToExit(B)) return;

    BasicBlock *stop = PDT.getIPostDom(B);
    // stop 可能是 nullptr（虚拟根）

    for (BasicBlock *P : B->getPredecessors()) {
        BasicBlock *runner = P;
        while (runner != stop) {
            if (runner == nullptr) break;

            // [OQ] runner 也必须是 exit-reachable 才能信任其 ipostdom
            if (!PDT.isReachableToExit(runner)) break;

            Instruction *term = runner->back();
            if (term) {
                auto *br = dynamic_cast<BrInst *>(term);
                if (br && br->isConditional()) {
                    markLive(term, ctx);
                }
            }
            runner = PDT.getIPostDom(runner);
        }
    }
}

// pickSurvivorSuccessor [F3/F4 重设计]
//
// 始终从原始两个后继中选一个。
// 不跳到任意 postdom → 不需要 PHI repair。
BasicBlock *ADCEPass::pickSurvivorSuccessor(
    BasicBlock *BB, PostDominatorTree &PDT) {
    BrInst *br = dynamic_cast<BrInst *>(BB->back());
    if (!br) return nullptr;
    if (!br->isConditional()) return nullptr;

    BasicBlock *trueBB = br->getTrueSuccessor();
    BasicBlock *falseBB = br->getFalseSuccessor();
    if (!trueBB || !falseBB) return trueBB ? trueBB : falseBB;

    // 两个目标相同 → 选它
    if (trueBB == falseBB) return trueBB;

    // 优先选 ipostdom(BB)，如果它是原始后继之一
    BasicBlock *ipostdom = PDT.getIPostDom(BB);
    if (ipostdom == trueBB)  return trueBB;
    if (ipostdom == falseBB) return falseBB;

    // 选后支配另一方的那个（更接近 exit）
    if (PDT.postDominates(trueBB, falseBB))  return trueBB;
    if (PDT.postDominates(falseBB, trueBB))  return falseBB;

    // 都不后支配对方 → 选 trueBB（任选都正确）
    return trueBB;
}

// sweep [F3/F4 重设计]
bool ADCEPass::sweep(
    Function &F, PostDominatorTree &PDT, ADCEContext &ctx,
    bool &cfgChanged) {
    bool changed = false;
    cfgChanged = false;

    // === Phase 1: 死条件分支改写 ===
    std::vector<BasicBlock *> blocksToRewrite;
    for (auto bb : F) {
        Instruction *term = bb->back();
        if (!term) continue;
        if (ctx.LiveInsts.count(term)) continue;  // 活的不动
        auto *br = dynamic_cast<BrInst *>(term);
        if (br && br->isConditional()) {
            blocksToRewrite.push_back(bb);
        }
    }

    for (BasicBlock *BB : blocksToRewrite) {
        BrInst *br = dynamic_cast<BrInst *>(BB->back());
        BasicBlock *trueBB = br->getTrueSuccessor();
        BasicBlock *falseBB = br->getFalseSuccessor();

        // 选择保留哪条边
        BasicBlock *survivor = pickSurvivorSuccessor(BB, PDT);
        BasicBlock *dropped  = (survivor == trueBB) ? falseBB : trueBB;

        // 清理被丢弃边的 PHI incoming
        if (dropped != survivor) {
            BasicBlock::removeEdgeFromSuccPhis(dropped, BB, -1);
        }

        // 改写: 删条件分支，换无条件跳转
        BasicBlock::replaceCondBrWithUncond(BB, survivor);
        changed = true;
        cfgChanged = true;
    }

    // === Phase 2: 删除死的非 terminator 指令 ===
    std::vector<Instruction *> deadInsts;
    for (auto bb : F) {
        for (auto inst : *bb) {
            if (ctx.LiveInsts.count(inst)) continue;
            if (inst == bb->back()) continue;  // terminator 已在 Phase 1 处理
            deadInsts.push_back(inst);
        }
    }

    for (Instruction *I : deadInsts) {
        if (I->hasUses()) {
            I->replaceAllUsesWith(UndefValue::NewUndefValue(I->getIRType()));
        }
        delete I;
        changed = true;
    }

    return changed;
}

// ADCEPass::run
PreservationStatus ADCEPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    // 0. 获取分析
    PostDominatorTree &PDT = FAM.getResult<PostDominantAnalysis>(F);

    // 1. 初始化 context
    ADCEContext ctx;

    // 2. Initialize: 收集 root-live
    forEachInstruction(F, [&](Instruction *I) {
        if (isRootLive(I, PDT)) markLive(I, ctx);
    });

    // 3. Mark: worklist 驱动的反向传播 + 控制依赖补活
    while (!ctx.Worklist.empty()) {
        Instruction *I = ctx.Worklist.front();
        ctx.Worklist.pop_front();

        // 3a. 数据依赖: 保活所有 Instruction 型操作数
        forEachOperand(I, [&](size_t, Use *, Value *V) {
            if (auto *OpI = dynamic_cast<Instruction *>(V)) {
                markLive(OpI, ctx);
            }
        });

        // 3b. 控制依赖: I 所在块
        BasicBlock *BB = I->getParent();
        markControlDependences(BB, PDT, ctx);

        // 3c. 控制依赖: PhiInst 的每个 incoming block
        if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
            forEachPhiIncoming(
                Phi, [&](unsigned, Value *, BasicBlock *IncomingBlock) {
                    markControlDependences(IncomingBlock, PDT, ctx);
                });
        }
    }

    // 4. Sweep
    bool cfgChanged = false;
    bool changed = sweep(F, PDT, ctx, cfgChanged);

    // 5. Preservation
    if (!changed) return PreservationStatus::all();

    PreservationStatus PA;
    if (!cfgChanged) {
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
    }
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
