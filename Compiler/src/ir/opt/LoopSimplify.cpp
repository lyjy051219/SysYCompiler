#include "ir/opt/LoopSimplify.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <string>

//将Pred中指向Header的边重定向到PreHeader
bool LoopSimplifyPass::replaceWith(BasicBlock *Pred, BasicBlock *Header, BasicBlock *PreHeader) {
    if (!Pred || !Header || !PreHeader || Header == PreHeader) return false;
    //获取前驱块最后一条指令
    auto *br = dynamic_cast<BrInst *>(Pred->back());
    if (!br) return false;

    bool changed = false;
    //遍历这条Br的usee
    //如果发现Br的某个操作数是Header，就把这个操作数替换成PreHeader
    for (auto &useUPtr : br->getUserList()) {
        Use *U = useUPtr.get();
        if (U && U->getValue() == Header) {
            //将Use从Header中删除
            U->removeFromUseList(U->getUser());
            //改变操作数为PreHeader
            U->usee = PreHeader;
            //将Use挂载到PreHeader上
            PreHeader->getUseList().addUseNode(U); // 将该 Use 注册到新的目标 (PreHeader) 的使用链表中

            changed = true;
        }
    }
    return changed;
}

//Preds->MergeBlock->Target
//修改Target中的Phi节点--将来自Preds的边提升到MergeBlock的新Phi，并将MergeBlock这个新Phi作为Target
bool LoopSimplifyPass::rewritePhisForMergedPreds(
    BasicBlock *Target,
    const std::unordered_set<BasicBlock *> &Preds,
    BasicBlock *MergeBlock) {
    if (!Target || !MergeBlock || Preds.empty()) return false;

    //1. 收集Target中的Phi节点 (已重构为面向对象调用)
    auto phis = Target->collectPhiNodes();
    bool changed = false;

    //2.遍历每个Phi节点
    for (PhiInst *phi : phis) {
        std::vector<std::pair<Value *, BasicBlock *>> moved;
        std::vector<std::pair<Value *, BasicBlock *>> kept;
        //遍历当前Phi节点的所有输入边，收集来自Preds的边到moved里，其他边到kept里
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            //取出当前边的输入值 inValue 和来源块 inBlock
            Value *inValue = phi->getIncomingValue(i);
            BasicBlock *inBlock = phi->getIncomingBlock(i);
            //判断每个输入边的来源块是否在Preds中，如果在就放到moved里，否则放到kept里
            if (Preds.count(inBlock)) {
                moved.push_back({inValue, inBlock});
            } else {
                kept.push_back({inValue, inBlock});
            }
        }

        if (moved.empty()) continue;

        Value *mergedValue = nullptr;
        //如果moved只有一条边，说明Preds只有一个Value传递到了Target，那么就直接用这个Value作为mergedValue
        if (moved.size() == 1) {
            mergedValue = moved.front().first;
        } else {
            //如果moved包含多条边，用moved中的Value和BasicBlock创建一个新的Phi节点mergePhi
            auto *mergePhi = new PhiInst(phi->getIRType());
            for (auto &kv : moved) {
                mergePhi->addIncoming(kv.first, kv.second);
            }
            // 【致命Bug已修复】：必须使用 push_front 保证 PHI 节点在基本块的最顶端！
            MergeBlock->push_front(mergePhi);
            mergedValue = mergePhi;
        }

        //为Target创建一个newPhi，将kept和mergedValue作为输入边添加到newPhi中
        auto *newPhi = new PhiInst(phi->getIRType());
        for (auto &kv : kept) {
            newPhi->addIncoming(kv.first, kv.second);
        }
        newPhi->addIncoming(mergedValue, MergeBlock);

        //如果合并完newPhi只有一个输入，那么就删除newPhi
        //剩下的就是常规将使用Phi的地方替换成newPhi
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

bool LoopSimplifyPass::insertPreHeader(Function &F, Loop &L) {
    //第一步：获取该循环的循环头
    BasicBlock *header = L.getHeader();
    if (!header) return false;
    //第二步：获取所有来自循环外部（用LoopAnalysis去判定）的前驱块
    std::vector<BasicBlock *> outPreds;
    for (BasicBlock *pred : header->getPredecessors()) {
        if (!L.contains(pred)) {
            outPreds.push_back(pred);
        }
    }

    if (outPreds.empty()) return false;

    // 幂等性保护：如果已存在唯一外部前驱且该前驱只跳转到 Header，
    // 说明 PreHeader 形态已经满足，不应重复插入。
    if (outPreds.size() == 1) {
        BasicBlock *candidate = outPreds.front();
        auto succs = candidate ? candidate->getSuccessors() : std::vector<BasicBlock *>{};
        if (succs.size() == 1 && succs.front() == header) {
            return false;
        }
    }

    //第三步：创建PreHeader
    auto *preheader = new BasicBlock();
    F.pushBlock(preheader);
    //第四步：建立前驱块和PreHeader之间的联系，取消前驱块和Header之间的联系--其实就是改动Br
    for (BasicBlock *pred : outPreds) {
        replaceWith(pred, header, preheader);
    }
    //第五步：修改Header中的Phi节点
    std::unordered_set<BasicBlock *> outSet(outPreds.begin(), outPreds.end());
    rewritePhisForMergedPreds(header, outSet, preheader);
    //第六步：添加Br在PreHeader和Header之间
    preheader->genUnCondInst(header);
    return true;
}

bool LoopSimplifyPass::insertDedicatedExits(Function &F, Loop &L) {
    //Key:ExitingBlock Value:来自循环内部的前驱块集合
    std::map<BasicBlock *, std::set<BasicBlock *>> exitInsidePreds;
    //遍历所有的出口块，及其循环内部的前驱
    for (auto *bb : F) {
        if (!L.contains(bb)) continue;
        for (BasicBlock *succ : bb->getSuccessors()) {
            if (!L.contains(succ)) {
                exitInsidePreds[succ].insert(bb);
            }
        }
    }

    bool changed = false;

    for (auto &it : exitInsidePreds) {
        BasicBlock *exitBB = it.first;
        const std::set<BasicBlock *> &insidePredSet = it.second;
        if (!exitBB || insidePredSet.empty()) continue;
        //只有这个ExitingBlock有来自循环外部的前驱我们才需要加入DedicatedExit
        bool hasOutsidePred = false;
        for (BasicBlock *pred : exitBB->getPredecessors()) {
            if (!L.contains(pred)) {
                hasOutsidePred = true;
                break;
            }
        }

        if (!hasOutsidePred) continue;
        //创建DedicatedExit块
        auto *dedExit = new BasicBlock();
        F.pushBlock(dedExit);
        //依旧还是那三步
        std::unordered_set<BasicBlock *> insidePreds(insidePredSet.begin(), insidePredSet.end());
        for (BasicBlock *pred : insidePredSet) {
            replaceWith(pred, exitBB, dedExit);
        }

        rewritePhisForMergedPreds(exitBB, insidePreds, dedExit);
        dedExit->genUnCondInst(exitBB);
        changed = true;
    }

    return changed;
}

bool LoopSimplifyPass::insertLatch(Function &F, Loop &L) {
    BasicBlock *header = L.getHeader();
    if (!header) return false;
    //vector用来存放所有回边的来源块
    std::vector<BasicBlock *> backedgePreds;
    //遍历前驱依据LoopInfo找到所有回边来源块
    for (BasicBlock *pred : header->getPredecessors()) {
        if (L.contains(pred)) {
            backedgePreds.push_back(pred);
        }
    }

    if (backedgePreds.size() <= 1) return false;
    //创建Latch块
    auto *latch = new BasicBlock();
    F.pushBlock(latch);
    //依旧是那三步
    std::unordered_set<BasicBlock *> backedgeSet(backedgePreds.begin(), backedgePreds.end());
    for (BasicBlock *pred : backedgePreds) {
        replaceWith(pred, header, latch);
    }

    rewritePhisForMergedPreds(header, backedgeSet, latch);
    latch->genUnCondInst(header);
    return true;
}

bool LoopSimplifyPass::simplifyHeaderPhis(Loop &L) {
    BasicBlock *header = L.getHeader();
    if (!header) return false;

    // (已重构为面向对象调用)
    auto phis = header->collectPhiNodes();
    if (phis.empty()) return false;

    bool changed = false;
    for (PhiInst *phi : phis) {
        unsigned n = phi->getNumIncomingValues();
        // 情况 1：PHI 节点没有任何输入边
        if (n == 0) {
            phi->replaceAllUsesWith(UndefValue::NewUndefValue(phi->getIRType()));
            delete phi;
            changed = true;
            continue;
        }
        // 情况 2：退化 PHI —— 只有 1 条输入边
        if (n == 1) {
            phi->replaceAllUsesWith(phi->getIncomingValue(0));
            delete phi;
            changed = true;
            continue;
        }

        bool allSelf = true;
        for (unsigned i = 0; i < n; ++i) {
            if (phi->getIncomingValue(i) != phi) {
                allSelf = false;
                break;
            }
        }
        // 情况 3：退化 PHI —— 所有输入边都来自于 PHI 自己（自循环Phi）
        if (allSelf) {
            phi->replaceAllUsesWith(UndefValue::NewUndefValue(phi->getIRType()));
            delete phi;
            changed = true;
        }
    }

    return changed;
}

PreservationStatus LoopSimplifyPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    bool changed = false;
    bool cfgChanged = false;
    //只访问顶层循环，然后对每个顶层使用后序遍历（先修改最内层的loop结构，然后从内到外修改其他loop结构）
    //在LoopInfo中，每个顶层循环都是一棵森林
    //递归法实现的后序遍历
    for (Loop *topLoop : LI.getTopLevelLoops()) {
        if (!topLoop || !topLoop->getHeader()) continue;
        changed |= runOnLoop(F, *topLoop, cfgChanged);
    }

    if (!changed) {
        return PreservationStatus::all();
    }

    PreservationStatus PA;
    if (!cfgChanged) {
        // 仅做了退化 PHI 简化，不改 CFG，不触及内存读写。
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.keep<BasicAliasAnalysis>();
        PA.keep<SideEffectAnalysis>();
        return PA;
    }

    PA.discard<DominantAnalysis>();
    PA.discard<PostDominantAnalysis>();
    PA.discard<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}

bool LoopSimplifyPass::runOnLoop(Function &F, Loop &L, bool &cfgChanged) {
    bool changed = false;
    //后序遍历，先修改最内层的loop结构，然后从内到外修改其他loop结构
    for (Loop *subLoop : L.getSubLoops()) {
        if (!subLoop || !subLoop->getHeader()) continue;
        changed |= runOnLoop(F, *subLoop, cfgChanged);
    }

    if (insertPreHeader(F, L)) {
        changed = true;
        cfgChanged = true;
    }
    if (insertDedicatedExits(F, L)) {
        changed = true;
        cfgChanged = true;
    }
    if (insertLatch(F, L)) {
        changed = true;
        cfgChanged = true;
    }
    changed |= simplifyHeaderPhis(L);

    return changed;
}