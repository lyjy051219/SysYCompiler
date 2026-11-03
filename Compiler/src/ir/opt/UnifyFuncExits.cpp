// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/UnifyFuncExits.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"

#include <utility>
#include <vector>

PreservationStatus UnifyFuncExitsPass::run(Function &F, FunctionAnalysisPassManager &) {
	std::vector<std::pair<BasicBlock *, RetInst *>> retSites;

	forEachInstruction<RetInst>(
		F, [&](BasicBlock *BB, RetInst *RI) {
			if (BB->back() == RI) retSites.emplace_back(BB, RI);
		}); 

	if (retSites.size() <= 1) {
		return PreservationStatus::all();
	}

	auto *UnifiedExit = new BasicBlock();
	F.pushBlock(UnifiedExit);

	const bool isVoidRet = F.getIRType()->isVoid();
	PhiInst *retPhi = nullptr;

	if (isVoidRet) {
		UnifiedExit->genRetInst();
	} else {
		retPhi = new PhiInst(F.getIRType());
		UnifiedExit->pushBack(retPhi);
		UnifiedExit->genRetInst(retPhi);
	}

	for (const auto &site : retSites) {
		BasicBlock *BB = site.first;
		RetInst *RI = site.second;

		if (!isVoidRet) {
			Value *retVal = RI->getReturnValue();
			if (!retVal) {
				retVal = UndefValue::NewUndefValue(F.getIRType());
			}
			retPhi->addIncoming(retVal, BB);
		}

		delete RI;
		BB->genUnCondInst(UnifiedExit);
	}

	PreservationStatus PA;
	PA.discard<DominantAnalysis>();
	PA.discard<PostDominantAnalysis>();
	PA.discard<LoopAnalysis>();
	PA.discard<BasicAliasAnalysis>();
	PA.discard<SideEffectAnalysis>();
	return PA;
}
