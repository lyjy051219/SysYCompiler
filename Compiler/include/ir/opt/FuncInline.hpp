#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>

struct CalleeInlineProfile {
    bool HasBody = false;
    bool HasOtherCalls = false;
    bool SideEffectFreeLocalOnly = false;
    bool SafeToMemoize = false;
    int TotalInstCount = 0;
    int SelfRecursiveCallSites = 0;
};

class FuncInlinePass : public PassDescriptor<FuncInlinePass> {
public:
	explicit FuncInlinePass(bool enableRecursiveInline = true);

	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	static CalleeInlineProfile analyzeCallee(
		Function *Callee, FunctionAnalysisPassManager &FAM);
	static bool isInlineSite(
		Function &Caller, CallInst *CI, Function *Callee,
		const std::unordered_map<Function *, int> &CalleeFrequency,
		bool EnableRecursiveInline, FunctionAnalysisPassManager &FAM,
		CalleeInlineProfile &OutProfile);
	bool EnableRecursiveInline = true;
};
