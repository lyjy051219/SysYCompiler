#pragma once

#include "ir/opt/NewPassManager.hpp"

class LoopDeletionPass : public PassDescriptor<LoopDeletionPass> {
public:
	PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
