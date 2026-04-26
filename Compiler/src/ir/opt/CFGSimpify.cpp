#include "ir/opt/CFGSimpify.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>

static bool isKnownBoolConst(Value *V, bool &Out) {
	if (auto *CB = dynamic_cast<ConstBool *>(V)) {
		Out = CB->getVal();
		return true;
	}
	if (auto *CI = dynamic_cast<ConstInt *>(V)) {
		Out = (CI->getVal() != 0);
		return true;
	}
	if (auto *CF = dynamic_cast<ConstFloat *>(V)) {
		Out = (CF->getVal() != 0.0f);
		return true;
	}
	return false;
}

//消除死块
bool CFGSimplifyPass::removeDeadBlocks(Function &F) {
	bool Changed = false;
	BasicBlock *Entry = F.front();

	std::vector<BasicBlock *> Blocks;
	for (BasicBlock *BB : F) Blocks.push_back(BB);
    //排除入口块之后，检查前驱块：没有/唯一的前驱是自己，满足条件删除
	for (BasicBlock *BB : Blocks) {
		if (!BB || BB == Entry) continue;

		auto Preds = BB->getPredecessors();
		bool NoPred = Preds.empty();
		bool OnlySelfPred = (Preds.size() == 1 && Preds[0] == BB);
		if (!NoPred && !OnlySelfPred) continue;

		auto Succs = BB->getSuccessors();
		for (BasicBlock *Succ : Succs) {
			int EdgeCnt = BB->countEdgesToSucc(Succ);
            //如果该基本块到后继的边删除了：调用下面这个函数就更新掉后继中PHI来自该基本块的输入值
			if (EdgeCnt > 0) BasicBlock::removeEdgeFromSuccPhis(Succ, BB, EdgeCnt);
		}
        //删除基本块连同里面的所有指令
		BB->eraseBlockSafely();
		Changed = true;
	}

	return Changed;
}
//终结指令常量折叠
bool CFGSimplifyPass::foldTerminatorBranches(Function &F) {
	bool Changed = false;

	std::vector<BasicBlock *> Blocks;
	for (BasicBlock *BB : F) Blocks.push_back(BB);

	for (BasicBlock *BB : Blocks) {
		BrInst *Br = BB->getBranchTerminator();
		if (!BasicBlock::isCondBr(Br)) continue;

		auto &UL = Br->getUserList();
		Value *Cond = UL[0]->getValue();
		auto *TrueBB = dynamic_cast<BasicBlock *>(UL[1]->getValue());
		auto *FalseBB = dynamic_cast<BasicBlock *>(UL[2]->getValue());
		if (!TrueBB || !FalseBB) continue;

		if (TrueBB == FalseBB) {
			// cond 分支两条边都到同一个块：删掉一条多余边
			BasicBlock::removeEdgeFromSuccPhis(TrueBB, BB, 1);
			BasicBlock::replaceCondBrWithUncond(BB, TrueBB);
			Changed = true;
			continue;
		}

		bool CondVal = false;
		if (!isKnownBoolConst(Cond, CondVal)) continue;

		BasicBlock *Taken = CondVal ? TrueBB : FalseBB;
		BasicBlock *Untaken = CondVal ? FalseBB : TrueBB;

		BasicBlock::removeEdgeFromSuccPhis(Untaken, BB, 1);
		BasicBlock::replaceCondBrWithUncond(BB, Taken);
		Changed = true;
	}

	return Changed;
}
//消除重复PHI节点
bool CFGSimplifyPass::eliminateDuplicatePhis(Function &F) {
	bool Changed = false;

	for (BasicBlock *BB : F) {
		if (!BB) continue;

		bool LocalChanged = true;
		while (LocalChanged) {
			LocalChanged = false;

			auto Phis = BB->collectPhiNodes();
			std::unordered_set<PhiInst *> Removed;

			for (size_t i = 0; i < Phis.size(); ++i) {
				PhiInst *Base = Phis[i];
				if (!Base || Removed.count(Base)) continue;

				for (size_t j = i + 1; j < Phis.size(); ++j) {
					PhiInst *Cand = Phis[j];
					if (!Cand || Removed.count(Cand)) continue;

					if (!PhiInst::areEquivalent(Base, Cand)) continue;

					Cand->replaceAllUsesWith(Base);
					Removed.insert(Cand);
					LocalChanged = true;
					Changed = true;
					break;
				}

				if (LocalChanged) break;
			}

			for (PhiInst *P : Removed) {
				if (P) delete P;
			}
		}
	}

	return Changed;
}
//合并基本块
bool CFGSimplifyPass::mergeBlocks(Function &F) {
	bool Changed = false;
	BasicBlock *Entry = F.front();

	std::vector<BasicBlock *> Blocks;
	for (BasicBlock *BB : F) Blocks.push_back(BB);

	for (BasicBlock *BB : Blocks) {
		if (!BB || BB == Entry) continue;

		auto Preds = BB->getPredecessors();
		if (Preds.size() != 1) continue;

		BasicBlock *Pred = Preds[0];
		if (!Pred || Pred == BB) continue;

		auto PredSuccs = Pred->getSuccessors();
		if (PredSuccs.size() != 1 || PredSuccs[0] != BB) continue;

		bool HasSelfRefPhi = false;
		for (PhiInst *Phi : BB->collectPhiNodes()) {
			if (!Phi) continue;
			for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
				if (Phi->getIncomingValue(i) == Phi) {
					HasSelfRefPhi = true;
					break;
				}
			}
			if (HasSelfRefPhi) break;
		}
		if (HasSelfRefPhi) continue;

		auto Succs = BB->getSuccessors();

		// 1) 处理当前块中的 PHI（唯一前驱场景）
		for (PhiInst *Phi : BB->collectPhiNodes()) {
			if (!Phi) continue;
			Value *InV = Phi->getIncomingFromPred(Pred);
			if (!InV) InV = UndefValue::NewUndefValue(Phi->getIRType());
			Phi->replaceAllUsesWith(InV);
			delete Phi;
			Changed = true;
		}

		// 2) 后继 PHI incoming block: BB -> Pred
		for (BasicBlock *Succ : Succs) {
			BasicBlock::replaceIncomingPredInSuccPhis(Succ, BB, Pred);
		}

		// 3) 把 BB 的指令整体搬到 Pred，并删掉 Pred 原终结指令
		BrInst *PredBr = Pred->getBranchTerminator();
		if (!PredBr) continue;
		delete PredBr;

		std::vector<Instruction *> ToMove;
		for (Instruction *I : *BB) {
			ToMove.push_back(I);
		}
		for (Instruction *I : ToMove) {
			I->EraseFromParent();
			Pred->push_back(I);
		}

		BB->EraseFromParent();
		Changed = true;
	}

	return Changed;
}

bool CFGSimplifyPass::simplifyUncondBranch(Function &F, BasicBlock *BB) {
	if (!BB || BB == F.front()) return false;

	BrInst *Br = BB->getBranchTerminator();
	if (!BasicBlock::isUncondBr(Br)) return false;
	if (!BB->onlyPhiAndTerminator()) return false;

	auto *Succ = dynamic_cast<BasicBlock *>(Br->getUserList()[0]->getValue());
	if (!Succ || Succ == BB) return false;

	auto Preds = BB->getPredecessors();
	if (Preds.empty()) return false;

	// 防止和 Succ 已有前驱重叠，避免 PHI 语义歧义
	std::unordered_set<BasicBlock *> SuccPredSet;
	for (BasicBlock *P : Succ->getPredecessors()) {
		if (P) SuccPredSet.insert(P);
	}
	for (BasicBlock *P : Preds) {
		if (!P) return false;
		if (SuccPredSet.count(P)) return false;
	}

	// BB 中 PHI 必须只被 Succ 的 PHI 使用
	auto BBPhis = BB->collectPhiNodes();
	for (PhiInst *Phi : BBPhis) {
		if (!Phi) continue;
		for (auto It = Phi->getUseList().begin(); It != Phi->getUseList().end(); ++It) {
			Use *U = *It;
			if (!U || U->getValue() != Phi) continue;
			auto *UI = dynamic_cast<Instruction *>(U->getUser());
			if (!UI || UI->getParent() != Succ) return false;
			if (!dynamic_cast<PhiInst *>(UI)) return false;
		}
	}

	// 改写 Succ 的 PHI：把来自 BB 的 incoming 展开为来自 Preds 的 incoming
	auto SuccPhis = Succ->collectPhiNodes();
	for (PhiInst *SP : SuccPhis) {
		if (!SP) continue;

		auto In = SP->collectIncomingEdges();
		bool Touched = false;
		std::vector<PhiInst::IncomingEdge> NewIn;

		for (const auto &E : In) {
			if (E.Pred != BB) {
				NewIn.push_back(E);
				continue;
			}

			Touched = true;
			for (BasicBlock *P : Preds) {
				Value *V = E.V;
				if (auto *BBPhi = dynamic_cast<PhiInst *>(V)) {
					if (BBPhi->getParent() == BB) {
						Value *FromP = BBPhi->getIncomingFromPred(P);
						V = FromP ? FromP : UndefValue::NewUndefValue(BBPhi->getIRType());
					}
				}
				NewIn.push_back({V, P});
			}
		}

		if (Touched) {
			PhiInst::rebuildFromIncoming(SP, NewIn);
		}
	}

	// 重定向 Pred -> BB 到 Pred -> Succ
	bool Rewired = false;
	for (BasicBlock *P : Preds) {
		Rewired |= BasicBlock::redirectBranchTarget(P, BB, Succ);
	}
	if (!Rewired) return false;

	BB->eraseBlockSafely();
	return true;
}

bool CFGSimplifyPass::simplifyBranches(Function &F) {
	bool Changed = false;

	{
		std::vector<BasicBlock *> Blocks;
		for (BasicBlock *BB : F) Blocks.push_back(BB);
		for (BasicBlock *BB : Blocks) {
			if (!BB) continue;
			Changed |= simplifyUncondBranch(F, BB);
		}
	}

	return Changed;
}

PreservationStatus CFGSimplifyPass::run(Function &F, FunctionAnalysisPassManager &) {
	bool ChangedAny = false;

	while (true) {
		bool Changed = false;

		Changed |= removeDeadBlocks(F);
		Changed |= foldTerminatorBranches(F);
		Changed |= eliminateDuplicatePhis(F);
		Changed |= mergeBlocks(F);
		Changed |= simplifyBranches(F);

		ChangedAny |= Changed;
		if (!Changed) break;
	}

	if (!ChangedAny) return PreservationStatus::all();
	PreservationStatus PA;
	PA.discard<DominantAnalysis>();
	PA.discard<PostDominantAnalysis>();
	PA.discard<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
