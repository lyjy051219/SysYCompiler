#include "ir/opt/LCSSA.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <set>
//校验辅助：循环外使用必须是位于退出块的 PHI，且具体 use 对应的 PHI 入边
//前驱仍在循环内；普通循环外使用或绕过退出块的 PHI 均破坏 LCSSA。
bool LCSSAPass::isLCSSAForm(Loop *L) {
    if (!L) {
        return false;
    }
    std::vector<BasicBlock*> ExitBlocks = L->getExitBlocks();
    std::set<BasicBlock *> ExitSet(ExitBlocks.begin(), ExitBlocks.end());
	return forEachInstructionInBlocks(
		L->getBlocks(), [&ExitSet, L](Instruction *Def) {
			for (Use *U : Def->getUses()) {
				auto *UserInst = dynamic_cast<Instruction *>(
					U ? U->getUser() : nullptr);
				if (!UserInst) continue;
				auto *UserPhi = dynamic_cast<PhiInst *>(UserInst);
				BasicBlock *UseBlock = nullptr;
				if (UserPhi) {
					auto OpIdx = U->getOperandIndex();
					if (!OpIdx || (*OpIdx % 2) != 0) continue;
					UseBlock = UserPhi->getIncomingBlock(
						static_cast<unsigned>(*OpIdx / 2));
				} else {
					UseBlock = UserInst->getParent();
				}
				if (!UseBlock || L->contains(UseBlock)) continue;
				if (!UserPhi) return false;
				BasicBlock *PhiBB = UserPhi->getParent();
				if (!PhiBB || ExitSet.find(PhiBB) == ExitSet.end())
					return false;
				if (!L->contains(UseBlock)) return false;
			}
			return true;
		});
}
//步骤 3.2：求定义在指定块入口处可见的 LCSSA 值。递归沿直接支配关系向
//循环边界回溯，仅在从循环内 IDom 进入外部汇合块时建立桥接 PHI。
Value *LCSSAPass::getValueForBlock(BasicBlock *BB,
                                   Instruction *InstDef,
                                   const Loop &L,
                                   const DominatorTree &DT,
                                   std::unordered_map<BasicBlock *, Value *> &MapValue,
                                   InstructionWorkList &WorkList,
                                   bool &Changed) {
    //3.2.1 已求值块直接复用；预先缓存的新 PHI 也会截断 CFG 环递归。
    auto It = MapValue.find(BB);
    if (It != MapValue.end()) return It->second;
    //3.2.2 回溯到循环内部时，以支配性决定返回原定义还是 undef。
    if (L.contains(BB)) {
        if (DT.dominates(InstDef->getParent(), BB)) {
            return MapValue[BB] = InstDef;
        } else {
            return MapValue[BB] = UndefValue::NewUndefValue(InstDef->getIRType());
        }
    }
    //不可由支配树连接到边界的外部块没有可靠定义。
    BasicBlock *IDom = DT.getIDom(BB);
    if (!IDom) {
        return MapValue[BB] = UndefValue::NewUndefValue(InstDef->getIRType());
    }
    //3.2.3 IDom 也在循环外说明尚未发生新的路径汇合，继续沿直线支配链
    //复用上游值即可。
    if (!L.contains(IDom)) {
        Value *Val = getValueForBlock(IDom, InstDef, L, DT, MapValue,
                                      WorkList, Changed);
        return MapValue[BB] = Val;
    }
    //3.2.4 IDom 在循环内而当前块在循环外，说明这里跨越边界并可能汇合；
    //先缓存桥接 PHI，再递归向每个前驱取得输入值。
    auto *BridgePhi = new PhiInst(InstDef->getIRType());
    BridgePhi->setIdent(BridgePhi->getIdent() + ".lcssa");
    MapValue[BB] = BridgePhi;
    for (BasicBlock *Pred : BB->getPredecessors()) {
        if (!Pred) continue;
        Value *Incoming = getValueForBlock(Pred, InstDef, L, DT, MapValue,
                                           WorkList, Changed);
        if (!Incoming) {
            Incoming = UndefValue::NewUndefValue(InstDef->getIRType());
        }
        BridgePhi->addIncoming(Incoming, Pred);
    }
    if (BridgePhi->getNumIncomingValues() == 0) {
        delete BridgePhi;
        MapValue.erase(BB);
        return UndefValue::NewUndefValue(InstDef->getIRType());
    }
    BB->pushFront(BridgePhi);
    Changed = true;
    //新桥接 PHI 也可能被更外层循环视为跨界定义，压入工作队列继续闭包。
    WorkList.push(BridgePhi);
    return BridgePhi;
}
//步骤 2—4：把单条循环内定义的所有外部 use 改写为经出口 PHI 传递的值。
bool LCSSAPass::runLCSSA(Instruction *I,
                                   Function &F,
                                   const LoopInfo &LI,
                                   const DominatorTree &DT,
                                   const std::unordered_set<BasicBlock *> &Reachable,
                                   InstructionWorkList &WorkList) {
    if (!I) return false;
    if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
        //本 pass 生成的 PHI 已满足当前层边界，不重复为其构造同层出口 PHI。
        if (Phi->getIdent().find(".lcssa") != std::string::npos) {
            return false;
        }
    }
    BasicBlock *DefBB = I->getParent();
    if (!DefBB) return false;
    if (!I->hasUses()) return false;
    Loop *L = LI.getLoopFor(DefBB);
    if (!L) return false;
    //无退出边的死循环不存在可达的跨界传递点。
    auto ExitBlocks = L->getExitBlocks();
    if (ExitBlocks.empty()) {
        return false;
    }
    bool Changed = false;
    //2.快照 use，并把 PHI 的概念使用位置解释为对应 incoming 前驱块；
    //不可达 use 先替换为 undef，其余循环外 use 记为边界改写点。
    auto Uses = I->getUses();
    std::vector<ExternalUse> Boundaries;
    for (Use *U : Uses) {
        if (!U || U->getValue() != I) continue;
        auto *UserInst = dynamic_cast<Instruction *>(U->getUser());
        if (!UserInst) continue;
        auto *UserPhi = dynamic_cast<PhiInst *>(UserInst);
        BasicBlock *UseBlock = nullptr;
        if (UserPhi) {
            auto OpIndex = U->getOperandIndex();
            if (!OpIndex || (*OpIndex % 2) != 0) {
                continue;
            }
            UseBlock = UserPhi->getIncomingBlock(
                static_cast<unsigned>(*OpIndex / 2));
        } else {
            UseBlock = UserInst->getParent();
        }
        if (!UseBlock) continue;
        if (Reachable.find(UseBlock) == Reachable.end()) {
            if (!dynamic_cast<UndefValue *>(U->getValue())) {
                Changed |= U->replaceUseeWith(UndefValue::NewUndefValue(I->getIRType()));
            }
            continue;
        }
        if (!L->contains(UseBlock)) {
            Boundaries.push_back({U, UserInst, UserPhi, UseBlock});
        }
    }
    if (Boundaries.empty()) return Changed;
    std::unordered_map<BasicBlock *, Value *> MapValue;
    //3.1 为定义所支配的每个出口建立初始 LCSSA PHI：循环内前驱传原定义，
    //其他前驱传 undef。MapValue 以这些 PHI 作为递归求值的边界种子。
    for (BasicBlock *ExitBB : ExitBlocks) {
        if (!ExitBB) continue;
        if (!DT.dominates(DefBB, ExitBB)) continue;
        if (MapValue.count(ExitBB)) continue;
        auto *LCPhi = new PhiInst(I->getIRType());
        LCPhi->setIdent(LCPhi->getIdent() + ".lcssa");
        bool HasIncoming = false;
        for (BasicBlock *Pred : ExitBB->getPredecessors()) {
            if (!Pred) continue;
            if (L->contains(Pred)) {
                LCPhi->addIncoming(I, Pred);
                HasIncoming = true;
            } else {
                LCPhi->addIncoming(UndefValue::NewUndefValue(I->getIRType()), Pred);
            }
        }
        if (!HasIncoming) {
            delete LCPhi;
            continue;
        }
        ExitBB->pushFront(LCPhi);
        MapValue[ExitBB] = LCPhi;
        Changed = true;
        WorkList.push(LCPhi);
    }
    //4.对每个边界 use 递归取得其概念使用块上的值，并一次性替换原定义。
    for (const ExternalUse &EU : Boundaries) {
        if (!EU.U || EU.U->getValue() != I) continue;
        Value *Replacement = getValueForBlock(
            EU.UseBlock, I, *L, DT, MapValue, WorkList, Changed);
        if (!Replacement) {
            Replacement = UndefValue::NewUndefValue(I->getIRType());
        }
        Changed |= EU.U->replaceUseeWith(Replacement);
    }
    return Changed;
}
//按循环后序建立出口和桥接PHI并封闭全部跨循环SSA使用。
PreservationStatus LCSSAPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    //1.预先计算可达块，供单定义处理阶段隔离不可达 use。
    std::unordered_set<BasicBlock *> Reachable;
    F.collectReachableBlocks(Reachable);
    InstructionWorkList WorkList;
    bool Changed = false;
    //2.循环后序保证先封闭内层循环的数据流。
    std::vector<Loop*> PostOrderLoops = LI.getLoopsInPostOrder();
    for (Loop *L : PostOrderLoops) {
        auto ExitBlocks = L->getExitBlocks();
        if (ExitBlocks.empty()) continue;
        //2.1 从退出块沿 IDom 反向收集循环内、支配某条退出路径的块；退出块
        //只作为遍历根，不应自身进入内部定义集合。
        std::unordered_set<BasicBlock*> ExitDominatingBlocks;
        std::unordered_set<BasicBlock *> ExitRoots(
            ExitBlocks.begin(), ExitBlocks.end());
        std::vector<BasicBlock *> AncestorRoots(
            ExitBlocks.rbegin(), ExitBlocks.rend());
        forEachReachablePreOrderCustomSucc<BasicBlock>(
            AncestorRoots,
            [L, &DT](BasicBlock *Curr) {
                std::vector<BasicBlock *> Parent;
                if (!Curr || Curr == L->getHeader()) return Parent;
                BasicBlock *IDomBB = DT.getIDom(Curr);
                if (IDomBB && L->contains(IDomBB))
                    Parent.push_back(IDomBB);
                return Parent;
            },
            [L, &ExitRoots,
             &ExitDominatingBlocks](BasicBlock *Curr) {
                if (ExitRoots.count(Curr))
                    return TraversalControl::Continue;
                ExitDominatingBlocks.insert(Curr);
                if (Curr == L->getHeader())
                    return TraversalControl::SkipSuccessors;
                return TraversalControl::Continue;
            });
        //2.2 只把当前循环层中的有用定义加入去重工作队列；子循环已由后序
        //先行处理，本 pass 生成的 PHI 也不重复入队。
        forEachInstructionInBlocks(
            ExitDominatingBlocks,
            [&LI, L, &WorkList](BasicBlock *BB, Instruction *Inst) {
                if (LI.getLoopFor(BB) != L || !Inst->hasUses()) return;
                if (auto *Phi = dynamic_cast<PhiInst *>(Inst)) {
                    if (Phi->getIdent().find(".lcssa") != std::string::npos)
                        return;
                }
                WorkList.push(Inst);
            });
        //3—4.处理初始定义及其新建桥接 PHI，直到当前循环层局部收敛。
        while (!WorkList.empty()) {
            Instruction *I = WorkList.pop();
            Changed |= runLCSSA(I, F, LI, DT, Reachable, WorkList);
        }
    }
    if (!Changed) {
        return PreservationStatus::all();
    }
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    //5.仅插入 PHI 并改写 use，CFG 分析保持有效；指针 PHI 会改变基址、
    //偏移及 ModRef 推导，因此别名和副作用分析必须失效。
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
