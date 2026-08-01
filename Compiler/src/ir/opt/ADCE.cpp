#include "ir/opt/ADCE.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/IRUtils.hpp"
#include <vector>
//接口1：ADCE一开始就活跃的根指令
bool ADCEPass::isRootLive(
    Instruction *I, PostDominatorTree &PDT,
    FunctionAnalysisPassManager &FAM) {
    //Store、Ret、Call（仅无写效果且已证明必然返回时可不活跃）、未知语义指令、
    //无条件Br，以及无法到达真实出口的条件Br。
    if (dynamic_cast<StoreInst *>(I)) return true;
    if (dynamic_cast<RetInst *>(I)) return true;
    if (dceInstructionMustBePreserved(I)) return true; //未知的二元指令
    if (auto *CI = dynamic_cast<CallInst *>(I)) {
        if (Function *Callee = CI->getDirectCallee()) {
            const SideEffectInfo &SEI =
                FAM.getResult<SideEffectAnalysis>(*Callee);
            ModRefInfo MRI = getFunctionModRefInfo(SEI);
            if (MRI != ModRefInfo::NoModRef && MRI != ModRefInfo::Ref)
                return true;
            //ModRef只描述内存与可观察效果；即使调用为纯/只读，也必须
            //递归证明被调函数及其调用闭包全部有限返回后才能删除。
            return !dceCallIsGuaranteedToReturn(CI);
        }
        return true;
    }
    if (auto *BI = dynamic_cast<BrInst *>(I)) {
        if (!BI->isConditional()) return true;
        BasicBlock *BB = BI->getParent();
        if (BB && !PDT.isReachableToExit(BB)) return true;
        return false;
    }
    return false;
}
//接口2：标记活跃性（加入到LiveInsts，WorkList，LiveBlocks）
void ADCEPass::markLive(Instruction *I, ADCEContext &ctx) {
    //2.1 活跃指令只入队一次，并同步标记其所在块，供数据与控制依赖闭包使用。
    if (!ctx.LiveInsts.insert(I).second)
        return;
    ctx.Worklist.push_back(I);
    BasicBlock *BB = I->getParent();
    if (BB) ctx.LiveBlocks.insert(BB);
}
//接口3：标记控制依赖（沿后支配树从目标块的前驱向上遍历，直到遇到目标块的立即后支配者）
void ADCEPass::markControlDependences(
    BasicBlock *B, PostDominatorTree &PDT, ADCEContext &ctx) {
    //只处理可以到达真实出口的基本块，并且每个基本块只分析一次
    if (!B) return;
    if (!ctx.CDProcessed.insert(B).second) return;
    if (!PDT.isReachableToExit(B)) return;
    //从目标基本块的前驱沿着后支配链遍历
    BasicBlock *stop = PDT.getIPostDom(B);
    for (BasicBlock *P : B->getPredecessors()) {
        BasicBlock *runner = P;
        while (runner != stop) {
            if (runner == nullptr) break;
            if (!PDT.isReachableToExit(runner)) break;
            //对于路径上的基本块取得他的终结指令，如果是条件Br则标记为活跃
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
//为死条件分支从原后继中选择保持可达性的幸存边。
BasicBlock *ADCEPass::pickSurvivorSuccessor(
    BasicBlock *BB, PostDominatorTree &PDT) {
    //3.1 死条件分支只能保留原有后继，避免凭空增加边和修复任意 PHI；优先选择
    //立即后支配者，其次选后支配另一侧的后继，最后稳定选择 true 边。
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
//执行清扫
bool ADCEPass::sweep(
    Function &F, PostDominatorTree &PDT, ADCEContext &ctx,
    bool &cfgChanged) {
    bool changed = false;
    cfgChanged = false;
    //1.收集所有未标活的条件分支指令
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
        //如果PostIDom是A后继，或者分支A PostIDom 分支B，选择A
        BasicBlock *survivor = pickSurvivorSuccessor(BB, PDT);
        BasicBlock *dropped  = (survivor == trueBB) ? falseBB : trueBB;
        if (dropped != survivor) {
            BasicBlock::removeEdgeFromSuccPhis(dropped, BB, -1);
        }
        BasicBlock::replaceCondBrWithUncond(BB, survivor);
        changed = true;
        cfgChanged = true;
    }
    //2.收集所有未标活的非终结指令
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
    //1.获取后支配树
    PostDominatorTree &PDT = FAM.getResult<PostDominantAnalysis>(F);
    //2.初始化ADCE上下文
    ADCEContext ctx;
    //3.标记初始活跃的指令
    forEachInstruction(F, [&](Instruction *I) {
        if (isRootLive(I, PDT, FAM)) markLive(I, ctx);
    });
    //4.迭代求活跃闭包
    while (!ctx.Worklist.empty()) {
        Instruction *I = ctx.Worklist.front();
        ctx.Worklist.pop_front();
        //指令的操作数也一并活跃
        forEachOperand(I, [&](size_t, Use *, Value *V) {
            if (auto *OpI = dynamic_cast<Instruction *>(V)) {
                markLive(OpI, ctx);
            }
        });
        BasicBlock *BB = I->getParent();
        //标记控制依赖（沿后支配树从目标块的前驱向上遍历，直到遇到目标块的立即后支配者）
        markControlDependences(BB, PDT, ctx);
        if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
        //不仅标活incoming value对应的指令，还标活incoming block的条件分支指令
            forEachPhiIncoming(
                Phi, [&](unsigned, Value *, BasicBlock *IncomingBlock) {
                    markControlDependences(IncomingBlock, PDT, ctx);
                });
        }
    }
    //3.执行清扫
    bool cfgChanged = false;
    bool changed = sweep(F, PDT, ctx, cfgChanged);
    if (!changed) return PreservationStatus::all();

    //失效逻辑
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
