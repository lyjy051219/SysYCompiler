#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 将 select 指令改写为条件分支 + PHI
class Select2BranchPass : public PassDescriptor<Select2BranchPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
