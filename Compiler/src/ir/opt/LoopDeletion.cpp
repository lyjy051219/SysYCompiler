#include "ir/opt/LoopDeletion.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/LCSSA.hpp"

#include <functional>
#include <utility>
#include <unordered_set>
#include <vector>

PreservationStatus LoopDeletionPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	auto& DT = FAM.getResult<DominantAnalysis>(F);
	auto& LI = FAM.getResult<LoopAnalysis>(F);
	auto& AA = FAM.getResult<BasicAliasAnalysis>(F);

	bool Changed = false;

	std::vector<Loop*> PostOrderLoops;
	std::vector<std::pair<Loop*, bool>> LoopStack;
	for (Loop* Top : LI.getTopLevelLoops()) {
		if (Top) LoopStack.push_back({Top, false});
	}
	while (!LoopStack.empty()) {
		auto Item = LoopStack.back();
		LoopStack.pop_back();
		Loop* Cur = Item.first;
		bool Expanded = Item.second;
		if (!Cur) continue;
		if (Expanded) {
			PostOrderLoops.push_back(Cur);
			continue;
		}
		LoopStack.push_back({Cur, true});
		for (Loop* Sub : Cur->getSubLoops()) {
			if (Sub) LoopStack.push_back({Sub, false});
		}
	}
    //TopLevel+后序遍历所有循环
	for (Loop* L : PostOrderLoops) {
		if (!L || !L->getHeader()) continue;

		// 1) 必须满足 LCSSA
		if (!LCSSAPass::isLCSSAForm(L)) continue;

		// 2) 必须满足 LoopSimplify，并拿到 PreHeader/Latch
		BasicBlock* PreHeader = nullptr;
		BasicBlock* Latch = nullptr;
		if (!L->getLoopSimplifyForm(PreHeader, Latch)) continue;
		if (!PreHeader) continue;

		// 3) 先获取 ExitBlock 列表
		auto ExitBlocks = L->getExitBlocks();
		if (ExitBlocks.empty()) continue;
		BasicBlock* ExitBlock = (ExitBlocks.size() == 1) ? ExitBlocks.front() : nullptr;

		// 4) 循环永远不执行：检查不可进入时直接删
		bool CanEnter = true;
	    if (F.front() == PreHeader) {
			CanEnter = true;
		} else {
			auto Preds = PreHeader->getPredecessors();
			for (BasicBlock* Pred : Preds) {
				if (!Pred || Pred->Size() == 0) {
					CanEnter = true;
					break;
				}
				Instruction* Term = Pred->back();
				if (!Term || Term->instType != Instruction::BR) {
					CanEnter = true;
					break;
				}

				auto& UL = Term->getUserList();
				if (UL.size() != 3 || !UL[0] || !UL[1] || !UL[2]) {
					CanEnter = true;
					break;
				}

				Value* Cond = UL[0]->getValue();
				auto* TrueBB = dynamic_cast<BasicBlock*>(UL[1]->getValue());
				auto* FalseBB = dynamic_cast<BasicBlock*>(UL[2]->getValue());
				if (!TrueBB || !FalseBB) {
					CanEnter = true;
					break;
				}

				bool KnownCond = false;
				bool CondVal = false;
				if (auto* CB = dynamic_cast<ConstBool*>(Cond)) {
					KnownCond = true;
					CondVal = CB->getVal();
				} else if (auto* CI = dynamic_cast<ConstInt*>(Cond)) {
					KnownCond = true;
					CondVal = (CI->getVal() != 0);
				}

				if (!KnownCond) {
					CanEnter = true;
					break;
				}

				BasicBlock* Taken = CondVal ? TrueBB : FalseBB;
				if (Taken == PreHeader) {
					CanEnter = true;
					break;
				}

				CanEnter = false;
			}
		}

		if (!CanEnter) {
			if (PreHeader->Size() > 0) {
				if (Instruction* OldTerm = PreHeader->back()) {
					delete OldTerm;
				}
			}
			PreHeader->genUnCondInst(ExitBlock);

			auto ExitPhis = ExitBlock->collectPhiNodes();
			for (PhiInst* Phi : ExitPhis) {
				if (!Phi) continue;
				unsigned N = Phi->getNumIncomingValues();
				if (N == 0) {
					Phi->replaceAllUsesWith(UndefValue::NewUndefValue(Phi->getIRType()));
					delete Phi;
					continue;
				}

				Value* KeepV = Phi->getIncomingValue(0);
				auto* NewPhi = new PhiInst(Phi->getIRType());
				NewPhi->addIncoming(KeepV, PreHeader);
				Phi->replaceAllUsesWith(NewPhi);
				Phi->replaceWith(NewPhi);
				delete Phi;
			}

			std::vector<BasicBlock*> LoopBlocks;
			std::vector<Loop*> Stack;
			Stack.push_back(L);
			std::unordered_set<BasicBlock*> Seen;
			while (!Stack.empty()) {
				Loop* Cur = Stack.back();
				Stack.pop_back();
				if (!Cur) continue;
				for (BasicBlock* BB : Cur->getBlocks()) {
					if (BB && Seen.insert(BB).second) {
						LoopBlocks.push_back(BB);
					}
				}
				for (Loop* Sub : Cur->getSubLoops()) {
					if (Sub) Stack.push_back(Sub);
				}
			}

			for (BasicBlock* BB : LoopBlocks) {
				if (!BB) continue;
				for (auto It = BB->begin(); It != BB->end(); ++It) {
					Instruction* I = *It;
					if (!I) continue;
					if (!I->getIRType() || I->getIRType()->getEnum() == IR_VOID) continue;

					std::vector<Use*> Uses;
					for (auto Uit = I->getUseList().begin(); Uit != I->getUseList().end(); ++Uit) {
						if (*Uit) Uses.push_back(*Uit);
					}

					for (Use* U : Uses) {
						if (!U || U->getValue() != I) continue;
						auto* UserI = dynamic_cast<Instruction*>(U->getUser());
						if (!UserI) continue;
						BasicBlock* UserBB = UserI->getParent();
						if (!UserBB || !L->contains(UserBB)) {
							U->replaceUseeWith(UndefValue::NewUndefValue(I->getIRType()));
						}
					}
				}
			}

			for (BasicBlock* BB : LoopBlocks) {
				if (!BB) continue;
				for (auto It = BB->begin(); It != BB->end(); ++It) {
					Instruction* I = *It;
					if (!I) continue;
					if (!I->getIRType() || I->getIRType()->getEnum() == IR_VOID) continue;
					if (!I->getUseList().is_empty()) {
						I->replaceAllUsesWith(UndefValue::NewUndefValue(I->getIRType()));
					}
				}
				BB->clear();
				BB->EraseFromParent();
			}

			Changed = true;
			continue;
		}

		// 5) 多出口的情况：直接返回（跳过该循环）
		if (ExitBlocks.size() > 1) continue;

		// 6.1) 常规循环删除前检查 Exit PHI 是否平凡
		bool ExitPhiOK = true;
		for (PhiInst* Phi : ExitBlock->collectPhiNodes()) {
			if (!Phi) continue;
			unsigned N = Phi->getNumIncomingValues();
			if (N == 0) continue;

			Value* First = Phi->getIncomingValue(0);
			for (unsigned i = 1; i < N; ++i) {
				if (Phi->getIncomingValue(i) != First) {
					ExitPhiOK = false;
					break;
				}
			}
			if (!ExitPhiOK) break;

			if (auto* DefI = dynamic_cast<Instruction*>(First)) {
				BasicBlock* DefBB = DefI->getParent();
				if (DefBB && L->contains(DefBB)) {
					ExitPhiOK = false;
					break;
				}
			}
		}
		if (!ExitPhiOK) continue;

		// 6.1) 常规循环删除前检查循环级副作用
		if (LoopSideEffect(*L, AA, LI, FAM)) continue;

		// 6.1) SCEV 回边次数检查预留（SCEV 尚未实现，先注释）
		// const auto& S = FAM.getResult<SCEVAnalysis>(F);
		// auto BTC = S.getMaxBackedgeTakenCount(*L);
		// if (!BTC.isComputed()) continue;

		// 6.2) 执行删除（与 4.2 同逻辑）
		if (PreHeader->Size() > 0) {
			if (Instruction* OldTerm = PreHeader->back()) {
				delete OldTerm;
			}
		}
		PreHeader->genUnCondInst(ExitBlock);

		auto ExitPhis = ExitBlock->collectPhiNodes();
		for (PhiInst* Phi : ExitPhis) {
			if (!Phi) continue;
			unsigned N = Phi->getNumIncomingValues();
			if (N == 0) {
				Phi->replaceAllUsesWith(UndefValue::NewUndefValue(Phi->getIRType()));
				delete Phi;
				continue;
			}

			Value* KeepV = Phi->getIncomingValue(0);
			auto* NewPhi = new PhiInst(Phi->getIRType());
			NewPhi->addIncoming(KeepV, PreHeader);
			Phi->replaceAllUsesWith(NewPhi);
			Phi->replaceWith(NewPhi);
			delete Phi;
		}

		std::vector<BasicBlock*> LoopBlocks;
		std::vector<Loop*> Stack;
		Stack.push_back(L);
		std::unordered_set<BasicBlock*> Seen;
		while (!Stack.empty()) {
			Loop* Cur = Stack.back();
			Stack.pop_back();
			if (!Cur) continue;
			for (BasicBlock* BB : Cur->getBlocks()) {
				if (BB && Seen.insert(BB).second) {
					LoopBlocks.push_back(BB);
				}
			}
			for (Loop* Sub : Cur->getSubLoops()) {
				if (Sub) Stack.push_back(Sub);
			}
		}

		for (BasicBlock* BB : LoopBlocks) {
			if (!BB) continue;
			for (auto It = BB->begin(); It != BB->end(); ++It) {
				Instruction* I = *It;
				if (!I) continue;
				if (!I->getIRType() || I->getIRType()->getEnum() == IR_VOID) continue;

				std::vector<Use*> Uses;
				for (auto Uit = I->getUseList().begin(); Uit != I->getUseList().end(); ++Uit) {
					if (*Uit) Uses.push_back(*Uit);
				}

				for (Use* U : Uses) {
					if (!U || U->getValue() != I) continue;
					auto* UserI = dynamic_cast<Instruction*>(U->getUser());
					if (!UserI) continue;
					BasicBlock* UserBB = UserI->getParent();
					if (!UserBB || !L->contains(UserBB)) {
						U->replaceUseeWith(UndefValue::NewUndefValue(I->getIRType()));
					}
				}
			}
		}

		for (BasicBlock* BB : LoopBlocks) {
			if (!BB) continue;
			for (auto It = BB->begin(); It != BB->end(); ++It) {
				Instruction* I = *It;
				if (!I) continue;
				if (!I->getIRType() || I->getIRType()->getEnum() == IR_VOID) continue;
				if (!I->getUseList().is_empty()) {
					I->replaceAllUsesWith(UndefValue::NewUndefValue(I->getIRType()));
				}
			}
			BB->clear();
			BB->EraseFromParent();
		}

		Changed = true;
	}

	(void)DT;
	if (!Changed) return PreservationStatus::all();

	PreservationStatus PA;
	PA.discard<DominantAnalysis>();
	PA.discard<PostDominantAnalysis>();
	PA.discard<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	// SCEVAnalysis 尚未实现，待补齐后在此处添加失效处理。
	return PA;
}
