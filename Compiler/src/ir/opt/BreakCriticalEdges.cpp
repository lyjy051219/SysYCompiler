#include "ir/opt/BreakCriticalEdges.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <vector>


PreservationStatus BreakCriticalEdgesPass::run(Function &F, FunctionAnalysisPassManager &) {
	std::vector<CFGEdge> CriticalEdges;
	CFGSnapshot CFG(F);

	for (BasicBlock *From : F) {
		if (!From) continue;
		const auto &Successors = CFG.successorEdges(From);
		if (Successors.size() != 2 || Successors[0] == Successors[1]) {
			continue;
		}

		for (unsigned Index = 0; Index < Successors.size(); ++Index) {
			BasicBlock *To = Successors[Index];
			if (CFG.distinctPredecessors(To).size() <= 1) continue;
			CriticalEdges.push_back({From, Index, To});
		}
	}

	if (CriticalEdges.empty()) {
		return PreservationStatus::all();
	}

	bool Changed = false;

	for (const CFGEdge &Edge : CriticalEdges) {
		// 保护性重检：前面拆边可能改变 CFG 形态。
		if (!BasicBlock::isCriticalEdge(Edge)) continue;
		if (BasicBlock::splitEdge(F, Edge)) Changed = true;
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
