#include "ir/opt/LCSSA.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <set>

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

Value *LCSSAPass::getValueForBlock(BasicBlock *BB,
                                   Instruction *InstDef,
                                   const Loop &L,
                                   const DominatorTree &DT,
                                   std::unordered_map<BasicBlock *, Value *> &MapValue,
                                   InstructionWorkList &WorkList,
                                   bool &Changed) {
    //1,避免重复计算
    auto It = MapValue.find(BB);
    if (It != MapValue.end()) return It->second;

    // 2. 递归基底：如果回溯到了循环内部，直接返回原始定义
    if (L.contains(BB)) {
        // 安全检查：确保定义点确实支配这个内部块
        if (DT.dominates(InstDef->getParent(), BB)) {
            return MapValue[BB] = InstDef;
        } else {
            return MapValue[BB] = UndefValue::NewUndefValue(InstDef->getIRType());
        }
    }

    // 3. 获取直接支配块 (IDom)
    BasicBlock *IDom = DT.getIDom(BB);
    if (!IDom) {
        return MapValue[BB] = UndefValue::NewUndefValue(InstDef->getIRType());
    }

    // 4. IDom 在循环外：说明控制流在到达 BB 之前并没有发生新的汇合，
    // BB 的值必然来自于支配它的 IDom，所以这里是直线回溯的情况，我们直接递归向 IDom 要值即可
    if (!L.contains(IDom)) {
        Value *Val = getValueForBlock(IDom, InstDef, L, DT, MapValue,
                                      WorkList, Changed);
        return MapValue[BB] = Val;
    }

    // 5. IDom 在循环内：这是最核心的情况！
    // 此时 BB 一定是一个汇合点，它的前驱来自不同的出口。
    // 我们必须在这里原生地新建一个 PHI 节点，来收拢它的各个前驱路径。
    auto *BridgePhi = new PhiInst(InstDef->getIRType());
    BridgePhi->setIdent(BridgePhi->getIdent() + ".lcssa");

    MapValue[BB] = BridgePhi;

    for (BasicBlock *Pred : BB->getPredecessors()) {
        if (!Pred) continue;
        // 递归向每一个前驱要值
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

    // 将新生成的桥接 PHI 加入队列，触发可能的链式反应
    WorkList.push(BridgePhi);

    return BridgePhi;
}

bool LCSSAPass::runLCSSA(Instruction *I,
                                   Function &F,
                                   const LoopInfo &LI,
                                   const DominatorTree &DT,
                                   const std::unordered_set<BasicBlock *> &Reachable,
                                   InstructionWorkList &WorkList) {
    if (!I) return false;
    if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
        // 幂等性保护：LCSSA 自己生成的桥接/出口 PHI 不再重复参与构造。
        if (Phi->getIdent().find(".lcssa") != std::string::npos) {
            return false;
        }
    }
    BasicBlock *DefBB = I->getParent();
    if (!DefBB) return false;
    if (!I->hasUses()) return false;

    Loop *L = LI.getLoopFor(DefBB);
    if (!L) return false;
    //第一步：收集所有的ExitingBlock，如果没有（死循环），直接返回
    auto ExitBlocks = L->getExitBlocks();
    if (ExitBlocks.empty()) {
        return false;
    }
    //第一步：下面处理有ExitingBlock的情况
    bool Changed = false;

    //第二步，寻找这条指令在循环外部的Uses
    auto Uses = I->getUses();
    //定义在循环外部的Use集合
    std::vector<ExternalUse> Boundaries;
    for (Use *U : Uses) {
        if (!U || U->getValue() != I) continue;

        auto *UserInst = dynamic_cast<Instruction *>(U->getUser());
        if (!UserInst) continue;
        //情况1：如果User是Phi，那我们就收集这个Phi所在块的前驱块
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
            //情况2：如果正常指令，它的UseBlock就是它所在的基本块
            UseBlock = UserInst->getParent();
        }

        if (!UseBlock) continue;

        //情况3：处理不可达块
        if (Reachable.find(UseBlock) == Reachable.end()) {
            // 幂等性保护：已经是 undef 时，不要重复替换成新的 undef 实例。
            if (!dynamic_cast<UndefValue *>(U->getValue())) {
                Changed |= U->replaceUseeWith(UndefValue::NewUndefValue(I->getIRType()));
            }
            continue;
        }
        //如果上述收集的使用块在循环之外，就是我们要找的循环边界
        if (!L->contains(UseBlock)) {
            Boundaries.push_back({U, UserInst, UserPhi, UseBlock});
        }
    }

    if (Boundaries.empty()) return Changed;

    std::unordered_map<BasicBlock *, Value *> MapValue;

    // 第三步：在出口块插入最初的 LCSSA PHI 节点
    for (BasicBlock *ExitBB : ExitBlocks) {
        if (!ExitBB) continue;
        //情况1：指令所在块不支配ExitingBlock
        if (!DT.dominates(DefBB, ExitBB)) continue;
        //情况2：已经为ExitingBlock插入过值了
        if (MapValue.count(ExitBB)) continue;

        auto *LCPhi = new PhiInst(I->getIRType());
        LCPhi->setIdent(LCPhi->getIdent() + ".lcssa");
        bool HasIncoming = false;

        for (BasicBlock *Pred : ExitBB->getPredecessors()) {
            if (!Pred) continue;
            if (L->contains(Pred)) {
                //正常情况：指令所在块支配ExitingBlock，插入LCSSA
                LCPhi->addIncoming(I, Pred);
                HasIncoming = true;
            } else {
                //特殊情况：指令所在块不支配ExitingBlock，插入UndefValue占位
                LCPhi->addIncoming(UndefValue::NewUndefValue(I->getIRType()), Pred);
            }
        }

        if (!HasIncoming) {
            delete LCPhi;
            continue;
        }

        ExitBB->pushFront(LCPhi);
        MapValue[ExitBB] = LCPhi; // 将出口 PHI 存入缓存，作为递归的终点
        Changed = true;

        WorkList.push(LCPhi);
    }

    // 第四步：外部 Use 重写
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

PreservationStatus LCSSAPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    // 计算可达块
    std::unordered_set<BasicBlock *> Reachable;
    F.collectReachableBlocks(Reachable);

    InstructionWorkList WorkList;
    bool Changed = false;

    //1：获取所有的循环，并按照后序遍历 (Post-Order) 的顺序排列
    std::vector<Loop*> PostOrderLoops = LI.getLoopsInPostOrder();

    //2：按后序遍历的顺序，逐个处理循环
    for (Loop *L : PostOrderLoops) {
        auto ExitBlocks = L->getExitBlocks();
        if (ExitBlocks.empty()) continue; // 死循环直接跳过

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
                // Exit blocks only seed the walk; the old algorithm inserted
                // their in-loop IDom, never the exit roots themselves.
                if (ExitRoots.count(Curr))
                    return TraversalControl::Continue;
                ExitDominatingBlocks.insert(Curr);
                if (Curr == L->getHeader())
                    return TraversalControl::SkipSuccessors;
                return TraversalControl::Continue;
            });

        //我们直接遍历这些支配ExitingBlock，并且在循环中的基本块的这些指令即可
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

        // 4：局部收敛处理
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
    // LCSSA inserts PHIs, including pointer-typed PHIs.  Their base/offset and
    // ModRef relationships are absent from cached AA/side-effect results.
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
