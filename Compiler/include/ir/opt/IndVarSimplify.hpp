#pragma once

#include "ir/opt/NewPassManager.hpp"

// Canonicalize the induction-variable surface seen by later loop passes.
// The pass intentionally performs only equality-preserving rewrites: exact
// duplicate integer recurrences are merged and exit comparisons are oriented
// with the recurrence on the left.  Trip-count expansion and CFG rewriting
// remain the responsibility of their dedicated consumers.
class IndVarSimplifyPass : public PassDescriptor<IndVarSimplifyPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);
};
