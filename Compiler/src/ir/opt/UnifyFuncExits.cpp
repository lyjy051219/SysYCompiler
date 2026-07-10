#include "ir/opt/UnifyFuncExits.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <utility>
#include <vector>

PreservationStatus UnifyFuncExitsPass::run(Function &F, FunctionAnalysisPassManager &) {
	std::vector<std::pair<BasicBlock *, RetInst *>> retSites;

	for (BasicBlock *BB : F) {
		if (!BB || BB->empty()) continue;
		auto *RI = dynamic_cast<RetInst *>(BB->back());
		if (!RI) continue;
		retSites.emplace_back(BB, RI);
	}

	if (retSites.size() <= 1) {
		return PreservationStatus::all();
	}

	auto *UnifiedExit = new BasicBlock();
	F.pushBlock(UnifiedExit);

	const bool isVoidRet = (F.getIRType()->getEnum() == IR_VOID);
	PhiInst *retPhi = nullptr;

	if (isVoidRet) {
		UnifiedExit->genRetInst();
	} else {
		retPhi = new PhiInst(F.getIRType());
		UnifiedExit->push_back(retPhi);
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
