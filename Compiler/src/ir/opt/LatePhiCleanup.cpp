#include "ir/opt/LatePhiCleanup.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
#include <unordered_set>
#include <vector>

Value *LatePhiCleanupPass::safeReplacement(PhiInst *Phi, const DominatorTree &DT) {
	if (!Phi) return nullptr;

	const unsigned N = Phi->getNumIncomingValues();
	if (N == 0) {
		return UndefValue::NewUndefValue(Phi->getIRType());
	}

	Value *Common = nullptr;
	bool SawUndef = false;
	std::unordered_set<BasicBlock *> Preds;

	bool Compatible = forEachPhiIncoming(
		Phi, [&](unsigned, Value *V, BasicBlock *Pred) {
			if (Pred) Preds.insert(Pred);
			if (!V || V == Phi) return true;
			if (V->isUndef()) {
				SawUndef = true;
				return true;
			}
			if (!Common) {
				Common = V;
				return true;
			}
			return Common == V;
		});
	if (!Compatible) return nullptr;

	if (!Common) return nullptr;

	BasicBlock *BB = Phi->getParent();
	const bool SoleCFGPred =
		BB && Preds.size() == 1 && BB->getPredecessors().size() == 1 &&
		BB->getPredecessors().front() == *Preds.begin();

	if (!SoleCFGPred && !DT.dominatesValue(Common, Phi)) return nullptr;

	if (SawUndef && !DT.dominatesValue(Common, Phi) && !SoleCFGPred) return nullptr;
	return Common;
}

bool LatePhiCleanupPass::cleanupFunction(Function &F, DominatorTree &DT) {
	bool Changed = false;
	bool LocalChanged = true;

	while (LocalChanged) {
		LocalChanged = false;
		std::vector<PhiInst *> Phis;
		for (BasicBlock *BB : F) {
			if (!BB) continue;
			for (PhiInst *Phi : BB->collectPhiNodes()) {
				if (Phi) Phis.push_back(Phi);
			}
		}

		for (PhiInst *Phi : Phis) {
			if (!Phi || !Phi->getParent()) continue;

			if (!Phi->hasUses()) {
				delete Phi;
				Changed = true;
				LocalChanged = true;
				continue;
			}

			Value *Replacement = safeReplacement(Phi, DT);
			if (!Replacement || Replacement == Phi) continue;

			Phi->replaceAllUsesWith(Replacement);
			delete Phi;
			Changed = true;
			LocalChanged = true;
		}
	}

	return Changed;
}

PreservationStatus LatePhiCleanupPass::run(Function &F,
                                           FunctionAnalysisPassManager &FAM) {
	auto &DT = FAM.getResult<DominantAnalysis>(F);
	bool Changed = cleanupFunction(F, DT);
	if (!Changed) return PreservationStatus::all();

	PreservationStatus PA;
	PA.keep<DominantAnalysis>();
	PA.keep<PostDominantAnalysis>();
	PA.keep<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
