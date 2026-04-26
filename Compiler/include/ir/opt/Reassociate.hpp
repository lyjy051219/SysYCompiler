#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 代数重结合优化
class ReassociatePass : public PassDescriptor<ReassociatePass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
