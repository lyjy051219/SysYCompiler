#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_set>

// Late, control-flow-neutral strength reduction.
//
// This pass intentionally avoids the full RangeAwareSimplify comparison and
// constant folds, because after loop rotation/LCSSA those folds are much more
// sensitive to contextual range precision. It only rewrites signed div/rem by
// a positive power-of-two when the dividend is syntactically guaranteed
// non-negative.
class LateStrengthReducePass : public PassDescriptor<LateStrengthReducePass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool insertSignedDivPow2Sequence(
        BinaryInst *BI, Value *LHS, int32_t Divisor,
        Instruction *&Quotient);
    static bool isGuaranteedNonNegativeImpl(
        Value *V, int Depth, std::unordered_set<Value *> &Visiting);
    static bool isGuaranteedNonNegative(Value *V);
    static bool reduceFunction(Function &F);
};
