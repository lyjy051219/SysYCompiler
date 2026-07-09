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
	explicit FuncInlinePass(bool enableRecursiveInline = true)
		: EnableRecursiveInline(enableRecursiveInline) {}

	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	bool EnableRecursiveInline = true;
};
