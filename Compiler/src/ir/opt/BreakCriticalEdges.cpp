#include "ir/opt/BreakCriticalEdges.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <utility>
#include <vector>


static bool hasMultipleDistinctSuccs(BasicBlock *BB) {
	if (!BB) return false;
	auto Succs = BB->getSuccessors();
	if (Succs.size() < 2) return false;
	return Succs[0] != Succs[1];
}

static bool isCriticalEdge(BasicBlock *From, BasicBlock *To) {
	if (!From || !To) return false;
	if (!hasMultipleDistinctSuccs(From)) return false;
	auto Preds = To->getPredecessors();
	return Preds.size() > 1;
}


PreservationStatus BreakCriticalEdgesPass::run(Function &F, FunctionAnalysisPassManager &) {
	std::vector<std::pair<BasicBlock *, BasicBlock *>> CriticalEdges;

	for (BasicBlock *From : F) {
		if (!From) continue;
		auto Succs = From->getSuccessors();
		if (Succs.size() != 2) continue;
		if (Succs[0] == Succs[1]) continue;

		for (BasicBlock *To : Succs) {
			if (isCriticalEdge(From, To)) {
				CriticalEdges.emplace_back(From, To);
			}
		}
	}

	if (CriticalEdges.empty()) {
		return PreservationStatus::all();
	}

	bool Changed = false;

	for (const auto &Edge : CriticalEdges) {
		BasicBlock *From = Edge.first;
		BasicBlock *To = Edge.second;
		if (!From || !To) continue;

		// 保护性重检：前面拆边可能改变 CFG 形态。
		if (!isCriticalEdge(From, To)) continue;

		auto *SplitBB = new BasicBlock();
		F.pushBlock(SplitBB);
		SplitBB->genUnCondInst(To);

		bool Redirected = BasicBlock::redirectBranchTarget(From, To, SplitBB);
		if (!Redirected) {
			SplitBB->eraseBlockSafely();
			continue;
		}

		BasicBlock::replaceIncomingPredInSuccPhis(To, From, SplitBB);
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
