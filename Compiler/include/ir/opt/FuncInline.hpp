#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>

class FuncInlinePass : public PassDescriptor<FuncInlinePass> {
public:
	explicit FuncInlinePass(bool enableRecursiveInline = true)
		: EnableRecursiveInline(enableRecursiveInline) {}

	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
	bool EnableRecursiveInline = true;
};

