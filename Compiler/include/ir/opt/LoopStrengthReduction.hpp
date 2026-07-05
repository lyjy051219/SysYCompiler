#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// Rewrites affine final-index GEPs into pointer induction variables and shares
// one pointer IV across nearby constant offsets when doing so is profitable.
class LoopStrengthReductionPass
    : public PassDescriptor<LoopStrengthReductionPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
