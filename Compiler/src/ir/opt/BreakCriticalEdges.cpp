// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/BreakCriticalEdges.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <utility>
#include <vector>

bool BreakCriticalEdgesPass::isCriticalEdge(
	Function &F, BasicBlock *From, BasicBlock *To) {
    //关键边不能跨函数
	if (!From || !To || From->getParent() != &F || To->getParent() != &F) {
		return false;
	} 
    // 前驱块必须有两个不同的后继块，后继块必须有多个不同的前驱块
	auto Successors = From->getSuccessors();
	if (Successors.size() != 2 || Successors[0] == Successors[1]) {
		return false;
	}

	return From->countEdgesToSucc(To) == 1 &&
	       To->getPredecessors().size() > 1;
}

bool BreakCriticalEdgesPass::splitCriticalEdge(
	Function &F, BasicBlock *From, BasicBlock *To) {
	//如果已经不是关键边了，就不拆分
	if (!isCriticalEdge(F, From, To)) return false;

	// 先验证目标块的 PHI 与待拆 CFG 边一致，避免只改写一半
	for (PhiInst *Phi : To->collectPhiNodes()) {
		if (!Phi || Phi->countIncomingFromPred(From) == 0) return false;
	}

    //2.创建一个新的基本块 SplitBB，并将其插入到函数 F 中,SplitBB 的终结指令是无条件分支到 To
	auto *SplitBB = new BasicBlock();
	F.pushBlock(SplitBB);
	SplitBB->genUnCondInst(To);

	//3.CFG重定向：修改From的Br指令
	if (!BasicBlock::redirectBranchTarget(From, To, SplitBB)) {
		delete SplitBB;
		return false;
	}
    //4.修改To的PHI指令：将所有来自From的incoming项改为来自SplitBB
	BasicBlock::replaceIncomingPredInSuccPhis(To, From, SplitBB);
	return true;
}

PreservationStatus BreakCriticalEdgesPass::run(Function &F, FunctionAnalysisPassManager &) {
	std::vector<std::pair<BasicBlock *, BasicBlock *>> CriticalEdges;

    //1.寻找关键边：前驱块有两个不同的后继块，后继块有多个不同的前驱块
	for (BasicBlock *From : F) {
		if (!From) continue;
		auto Successors = From->getSuccessors();
		if (Successors.size() != 2 || Successors[0] == Successors[1]) {
			continue;
		}

		for (BasicBlock *To : Successors) {
			if (!To || To->getPredecessors().size() <= 1) continue;
			CriticalEdges.emplace_back(From, To);
		}
	}

	if (CriticalEdges.empty()) {
		return PreservationStatus::all();
	}

	bool Changed = false;

	for (const auto &[From, To] : CriticalEdges) {
		Changed |= splitCriticalEdge(F, From, To);
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
