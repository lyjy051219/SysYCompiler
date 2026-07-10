#include "ir/opt/Select2Branch.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <vector>

PreservationStatus Select2BranchPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
	(void)FAM;

	bool Changed = false;

	std::vector<SelectInst *> Selects;
	for (auto *BB : F) {
		if (!BB) continue;
		for (auto *I : *BB) {
			auto *SI = dynamic_cast<SelectInst *>(I);
			if (!SI) continue;
			Selects.push_back(SI);
		}
	}

	for (SelectInst *SI : Selects) {
		if (!SI) continue;

		BasicBlock *CurBB = SI->getParent();
		if (!CurBB) continue;

		Value *Cond = SI->getCondition();
		Value *TrueVal = SI->getTrueValue();
		Value *FalseVal = SI->getFalseValue();
		if (!Cond || !TrueVal || !FalseVal) continue;

		auto *MergeBB = new BasicBlock();
		auto *TrueBB = new BasicBlock();
		F.pushBlock(MergeBB);
		F.pushBlock(TrueBB);

		// 把 select 之后的后半段指令剪切到 MergeBB。
		Instruction *Walker = SI->getNext();
		while (Walker) {
			Instruction *Next = Walker->getNext();
			Walker->EraseFromParent();
			MergeBB->push_back(Walker);
			Walker = Next;
		}

		// TrueBB 无条件跳到 MergeBB。
		TrueBB->genUnCondInst(MergeBB);

		// 在 CurBB 尾部补条件分支：真->TrueBB，假->MergeBB。
		CurBB->genCondInst(Cond, TrueBB, MergeBB);

		// 在 MergeBB 顶部插入 PHI，替换原 select 的所有 uses。
		auto *Phi = new PhiInst(TrueVal->getIRType());
		MergeBB->push_front(Phi);
		Phi->addIncoming(TrueVal, TrueBB);
		Phi->addIncoming(FalseVal, CurBB);

		SI->replaceAllUsesWith(Phi);
		delete SI;

		Changed = true;
	}

	if (!Changed) return PreservationStatus::all();

	PreservationStatus PA;
	PA.discard<DominantAnalysis>();
	PA.discard<PostDominantAnalysis>();
	PA.discard<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
