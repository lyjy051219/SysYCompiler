#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class LoadEliminationPass : public PassDescriptor<LoadEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
