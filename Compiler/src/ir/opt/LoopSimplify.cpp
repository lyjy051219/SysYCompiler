#include "ir/opt/LoopSimplify.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <string>
//CFG 合并步骤的公共 PHI 修复：把 Preds 对 Target 的多条输入先汇入
//MergeBlock，再让 Target 只接收一条来自 MergeBlock 的输入。新建 PHI 始终
//放在块首，保持“PHI 先于普通指令”的结构不变量。
bool LoopSimplifyPass::rewritePhisForMergedPreds(
    BasicBlock *Target,
    const std::unordered_set<BasicBlock *> &Preds,
    BasicBlock *MergeBlock) {
    if (!Target || !MergeBlock || Preds.empty()) return false;
    //1.快照目标块 PHI，避免替换和删除过程中迭代器失效。
    auto phis = Target->collectPhiNodes();
    bool changed = false;
    //2.对每个 PHI 拆分待合并输入与原样保留的输入。
    for (PhiInst *phi : phis) {
        std::vector<std::pair<Value *, BasicBlock *>> moved;
        std::vector<std::pair<Value *, BasicBlock *>> kept;
        forEachPhiIncoming(
            phi, [&](unsigned, Value *inValue, BasicBlock *inBlock) {
            if (Preds.count(inBlock)) {
                moved.push_back({inValue, inBlock});
            } else {
                kept.push_back({inValue, inBlock});
            }
        });
        if (moved.empty()) continue;
        Value *mergedValue = nullptr;
        //3.多条输入在 MergeBlock 内新建桥接 PHI；单条输入可直接透传。
        if (moved.size() == 1) {
            mergedValue = moved.front().first;
        } else {
            auto *mergePhi = new PhiInst(phi->getIRType());
            for (auto &kv : moved) {
                mergePhi->addIncoming(kv.first, kv.second);
            }
            MergeBlock->pushFront(mergePhi);
            mergedValue = mergePhi;
        }
        //4.用“保留输入 + 合并输入”重建目标 PHI，并消去仅剩单输入的退化项。
        auto *newPhi = new PhiInst(phi->getIRType());
        for (auto &kv : kept) {
            newPhi->addIncoming(kv.first, kv.second);
        }
        newPhi->addIncoming(mergedValue, MergeBlock);
        if (newPhi->getNumIncomingValues() == 1) {
            phi->replaceAllUsesWith(newPhi->getIncomingValue(0));
            delete newPhi;
        } else {
            phi->replaceAllUsesWith(newPhi);
            phi->replaceWith(newPhi);
        }
        delete phi;
        changed = true;
    }
    return changed;
}
//步骤 1：收集全部循环外前驱；若尚无唯一直连预头，则插入合并块、重定向入口边并通过公共 PHI 修复保持每条入边的原值。
bool LoopSimplifyPass::insertPreHeader(Function &F, Loop &L) {
    BasicBlock *header = L.getHeader();
    if (!header) return false;
    std::vector<BasicBlock *> outPreds;
    for (BasicBlock *pred : header->getPredecessors()) {
        if (!L.contains(pred)) {
            outPreds.push_back(pred);
        }
    }
    if (outPreds.empty()) return false;
    //已有唯一外部前驱且只通向循环头时，它本身就是合格预头。
    if (outPreds.size() == 1) {
        BasicBlock *candidate = outPreds.front();
        auto succs = candidate ? candidate->getSuccessors() : std::vector<BasicBlock *>{};
        if (succs.size() == 1 && succs.front() == header) {
            return false;
        }
    }
    auto *preheader = new BasicBlock();
    F.pushBlock(preheader);
    //先重定向 CFG，再按同一前驱集合重建头部 PHI，最后补上预头到头部的边。
    for (BasicBlock *pred : outPreds) {
        BasicBlock::redirectBranchTarget(pred, header, preheader);
    }
    std::unordered_set<BasicBlock *> outSet(outPreds.begin(), outPreds.end());
    rewritePhisForMergedPreds(header, outSet, preheader);
    preheader->genUnCondInst(header);
    return true;
}
//步骤 2：为同时含循环内外前驱的出口插入专用出口。
bool LoopSimplifyPass::insertDedicatedExits(Function &F, Loop &L) {
    std::map<BasicBlock *, std::set<BasicBlock *>> exitInsidePreds;
    //2.1 按退出目标归并所有来自循环内部的边。
    for (auto *bb : F) {
        if (!L.contains(bb)) continue;
        for (BasicBlock *succ : bb->getSuccessors()) {
            if (!L.contains(succ)) {
                exitInsidePreds[succ].insert(bb);
            }
        }
    }
    for (auto &it : exitInsidePreds) {
        BasicBlock *exitBB = it.first;
        const std::set<BasicBlock *> &insidePredSet = it.second;
        if (!exitBB || insidePredSet.empty()) continue;
        //2.2 只有还存在循环外前驱时，目标块才不是专用出口。
        bool hasOutsidePred = false;
        for (BasicBlock *pred : exitBB->getPredecessors()) {
            if (!L.contains(pred)) {
                hasOutsidePred = true;
                break;
            }
        }
        if (!hasOutsidePred) continue;
        //2.3 插入中转块、重定向循环内退出边并同步折叠目标 PHI 输入。
        auto *dedExit = new BasicBlock();
        F.pushBlock(dedExit);
        std::unordered_set<BasicBlock *> insidePreds(insidePredSet.begin(), insidePredSet.end());
        for (BasicBlock *pred : insidePredSet) {
            BasicBlock::redirectBranchTarget(pred, exitBB, dedExit);
        }
        rewritePhisForMergedPreds(exitBB, insidePreds, dedExit);
        dedExit->genUnCondInst(exitBB);
        //LoopInfo 对应入口时的 CFG 快照；提交一次结构变化后立即返回。
        return true;
    }
    return false;
}
//步骤 3：多个循环内前驱都回到头部时，插入唯一 latch，并用桥接 PHI 合并每条回边携带的递推值。
bool LoopSimplifyPass::insertLatch(Function &F, Loop &L) {
    BasicBlock *header = L.getHeader();
    if (!header) return false;
    std::vector<BasicBlock *> backedgePreds;
    for (BasicBlock *pred : header->getPredecessors()) {
        if (L.contains(pred)) {
            backedgePreds.push_back(pred);
        }
    }
    if (backedgePreds.size() <= 1) return false;
    auto *latch = new BasicBlock();
    F.pushBlock(latch);
    std::unordered_set<BasicBlock *> backedgeSet(backedgePreds.begin(), backedgePreds.end());
    for (BasicBlock *pred : backedgePreds) {
        BasicBlock::redirectBranchTarget(pred, header, latch);
    }
    rewritePhisForMergedPreds(header, backedgeSet, latch);
    latch->genUnCondInst(header);
    return true;
}
//步骤 3.2：在 CFG 已规范时清理头部的空 PHI、单输入 PHI 和纯自环 PHI；
//这些改写只改变 SSA，不改变控制流。
bool LoopSimplifyPass::simplifyHeaderPhis(Loop &L) {
    BasicBlock *header = L.getHeader();
    if (!header) return false;
    auto phis = header->collectPhiNodes();
    if (phis.empty()) return false;
    bool changed = false;
    for (PhiInst *phi : phis) {
        unsigned n = phi->getNumIncomingValues();
        //空输入和纯自环没有外部可达定义，统一替换为 undef。
        if (n == 0) {
            phi->replaceAllUsesWith(UndefValue::NewUndefValue(phi->getIRType()));
            delete phi;
            changed = true;
            continue;
        }
        //单输入 PHI 直接转发唯一值。
        if (n == 1) {
            phi->replaceAllUsesWith(phi->getIncomingValue(0));
            delete phi;
            changed = true;
            continue;
        }
        bool allSelf = forEachPhiIncoming(
            phi, [phi](unsigned, Value *Incoming, BasicBlock *) {
                return Incoming == phi;
            });
        if (allSelf) {
            phi->replaceAllUsesWith(UndefValue::NewUndefValue(phi->getIRType()));
            delete phi;
            changed = true;
        }
    }
    return changed;
}
//按预头、专用出口、唯一锁存块和PHI清理顺序规范化单个循环。
bool LoopSimplifyPass::runOnLoop(Function &F, Loop &L, bool &cfgChanged) {
    if (insertPreHeader(F, L)) {
        cfgChanged = true;
        return true;
    }
    if (insertDedicatedExits(F, L)) {
        cfgChanged = true;
        return true;
    }
    if (insertLatch(F, L)) {
        cfgChanged = true;
        return true;
    }
    return simplifyHeaderPhis(L);
}
PreservationStatus LoopSimplifyPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    bool changed = false;
    bool cfgChanged = false;
    //1.以循环森林后序处理内层循环；发生首个CFG变化便停止，外层不动点会在重建支配树和LoopInfo后再次调用本pass
    for (Loop *L : LI.getLoopsInPostOrder()) {
        if (!L || !L->getHeader()) continue;
        bool loopCFGChanged = false;
        if (runOnLoop(F, *L, loopCFGChanged)) changed = true;
        if (loopCFGChanged) {
            cfgChanged = true;
            break;
        }
    }
    if (!changed) {
        return PreservationStatus::all();
    }
    PreservationStatus PA;
    if (!cfgChanged) {
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.discard<BasicAliasAnalysis>();
        PA.discard<SideEffectAnalysis>();
        return PA;
    }
    PA.discard<DominantAnalysis>();
    PA.discard<PostDominantAnalysis>();
    PA.discard<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
