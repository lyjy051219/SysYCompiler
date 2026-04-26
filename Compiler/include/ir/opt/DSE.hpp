#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 死存储消除
class DSEPass : public PassDescriptor<DSEPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
