#include "ir/opt/ADCE.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/IRUtils.hpp"

#include <vector>

bool ADCEPass::callHasObservableSideEffect(CallInst *CI) {
    // 1.1 仅明确标为 NoModRef 的 builtin 可作为死根删除；普通函数和未知 callee
    // 还可能不返回或具有未被局部摘要覆盖的行为，统一视为可观察。
    Value *Callee = CI ? CI->getCallee() : nullptr;
    if (!Callee) return true;

    if (auto *BIF = dynamic_cast<BuildInFunction *>(Callee)) {
        FuncAttr attr = classifyBuiltin(BIF);

        if (attr == FuncAttr::NoModRef)
            return false;

        return true;
    }

    if (dynamic_cast<Function *>(Callee)) {
        return true;
    }

    return true;
}

bool ADCEPass::isRootLive(
    Instruction *I, PostDominatorTree &PDT) {
    // 1.2 根活跃集合包含写内存、返回、未知语义指令和有副作用调用；无条件分支
    // 保持 CFG 连通，无法到达出口区域的条件分支也因后支配信息不可靠而保留。

    if (dynamic_cast<StoreInst *>(I)) return true;

    if (dynamic_cast<RetInst *>(I)) return true;

    if (dceInstructionMustBePreserved(I)) return true;

    if (auto *CI = dynamic_cast<CallInst *>(I))
        return callHasObservableSideEffect(CI);

    if (auto *BI = dynamic_cast<BrInst *>(I)) {

        if (!BI->isConditional()) return true;

        BasicBlock *BB = BI->getParent();
        if (BB && !PDT.isReachableToExit(BB)) return true;

        return false;
    }

    return false;
}

void ADCEPass::markLive(Instruction *I, ADCEContext &ctx) {
    // 2.1 活跃指令只入队一次，并同步标记其所在块，供数据与控制依赖闭包使用。
    if (!ctx.LiveInsts.insert(I).second)
        return;
    ctx.Worklist.push_back(I);
    BasicBlock *BB = I->getParent();
    if (BB) ctx.LiveBlocks.insert(BB);
}

void ADCEPass::markControlDependences(
    BasicBlock *B, PostDominatorTree &PDT, ADCEContext &ctx) {
    // 2.2 对每个活跃块，从其前驱沿立即后支配链走到该块的立即后支配者；路径上的
    // 条件分支决定该块是否执行，必须一并标活。每个块只展开一次控制依赖。
    if (!B) return;
    if (!ctx.CDProcessed.insert(B).second) return;

    if (!PDT.isReachableToExit(B)) return;

    BasicBlock *stop = PDT.getIPostDom(B);

    for (BasicBlock *P : B->getPredecessors()) {
        BasicBlock *runner = P;
        while (runner != stop) {
            if (runner == nullptr) break;

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

BasicBlock *ADCEPass::pickSurvivorSuccessor(
    BasicBlock *BB, PostDominatorTree &PDT) {
    // 3.1 死条件分支只能保留原有后继，避免凭空增加边和修复任意 PHI；优先选择
    // 立即后支配者，其次选后支配另一侧的后继，最后稳定选择 true 边。
    BrInst *br = dynamic_cast<BrInst *>(BB->back());
    if (!br) return nullptr;
    if (!br->isConditional()) return nullptr;

    BasicBlock *trueBB = br->getTrueSuccessor();
    BasicBlock *falseBB = br->getFalseSuccessor();
    if (!trueBB || !falseBB) return trueBB ? trueBB : falseBB;

    if (trueBB == falseBB) return trueBB;

    BasicBlock *ipostdom = PDT.getIPostDom(BB);
    if (ipostdom == trueBB)  return trueBB;
    if (ipostdom == falseBB) return falseBB;

    if (PDT.postDominates(trueBB, falseBB))  return trueBB;
    if (PDT.postDominates(falseBB, trueBB))  return falseBB;

    return trueBB;
}

bool ADCEPass::sweep(
    Function &F, PostDominatorTree &PDT, ADCEContext &ctx,
    bool &cfgChanged) {
    // 3. 第一阶段先快照并重写死条件分支，删除弃边对应的 PHI incoming，再改成
    // 无条件跳转；终结指令先处理可保证第二阶段不破坏基本块结构。
    bool changed = false;
    cfgChanged = false;

    std::vector<BasicBlock *> blocksToRewrite;
    for (auto bb : F) {
        Instruction *term = bb->back();
        if (!term) continue;
        if (ctx.LiveInsts.count(term)) continue;
        auto *br = dynamic_cast<BrInst *>(term);
        if (br && br->isConditional()) {
            blocksToRewrite.push_back(bb);
        }
    }

    for (BasicBlock *BB : blocksToRewrite) {
        BrInst *br = dynamic_cast<BrInst *>(BB->back());
        BasicBlock *trueBB = br->getTrueSuccessor();
        BasicBlock *falseBB = br->getFalseSuccessor();

        BasicBlock *survivor = pickSurvivorSuccessor(BB, PDT);
        BasicBlock *dropped  = (survivor == trueBB) ? falseBB : trueBB;

        if (dropped != survivor) {
            BasicBlock::removeEdgeFromSuccPhis(dropped, BB, -1);
        }

        BasicBlock::replaceCondBrWithUncond(BB, survivor);
        changed = true;
        cfgChanged = true;
    }

    // 3.2 第二阶段收集所有非终结死指令；若仍被其他死节点引用，先用同型 undef
    // 断开 use，再统一删除整个死值图。
    std::vector<Instruction *> deadInsts;
    for (auto bb : F) {
        for (auto inst : *bb) {
            if (ctx.LiveInsts.count(inst)) continue;
            if (inst == bb->back()) continue;
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

PreservationStatus ADCEPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    // 1. 获取后支配树并从所有可观察根初始化活跃工作表。
    PostDominatorTree &PDT = FAM.getResult<PostDominantAnalysis>(F);

    ADCEContext ctx;

    forEachInstruction(F, [&](Instruction *I) {
        if (isRootLive(I, PDT)) markLive(I, ctx);
    });

    // 2. 迭代闭包：沿操作数标记数据依赖，同时从普通指令所在块和 PHI 的每条
    // incoming 边补充控制依赖，直到工作表清空。
    while (!ctx.Worklist.empty()) {
        Instruction *I = ctx.Worklist.front();
        ctx.Worklist.pop_front();

        forEachOperand(I, [&](size_t, Use *, Value *V) {
            if (auto *OpI = dynamic_cast<Instruction *>(V)) {
                markLive(OpI, ctx);
            }
        });

        BasicBlock *BB = I->getParent();
        markControlDependences(BB, PDT, ctx);

        if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
            forEachPhiIncoming(
                Phi, [&](unsigned, Value *, BasicBlock *IncomingBlock) {
                    markControlDependences(IncomingBlock, PDT, ctx);
                });
        }
    }

    // 3. 标记完成后执行分支改写与死指令清扫。
    bool cfgChanged = false;
    bool changed = sweep(F, PDT, ctx, cfgChanged);

    if (!changed) return PreservationStatus::all();

    // 4. 若只删除普通指令则保留 CFG 分析；一旦重写分支，支配/后支配/循环均失效。
    // 两种情况下值图和调用集合都变化，因此 AA 与副作用分析均失效。
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
