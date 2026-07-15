#pragma once

#include "ir/opt/NewPassManager.hpp"

class Loop;
class PhiInst;
class SCEVResult;
class Value;

// Canonicalize the induction-variable surface seen by later loop passes.
// The pass intentionally performs only equality-preserving rewrites: exact
// duplicate integer recurrences are merged and exit comparisons are oriented
// with the recurrence on the left.  Trip-count expansion and CFG rewriting
// remain the responsibility of their dedicated consumers.
class IndVarSimplifyPass : public PassDescriptor<IndVarSimplifyPass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    // 精确比较 SSA 递推形状，避免仅凭 SCEV 数学等价误合并。
    static bool correspondingRecurrenceValue(
        Value *A, Value *B, PhiInst *APhi, PhiInst *BPhi);
    static bool haveExactUpdateShape(
        Value *A, Value *B, PhiInst *APhi, PhiInst *BPhi);
    static bool haveExactRecurrenceShape(PhiInst *A, PhiInst *B, Loop *L);
    static bool mergeExactRecurrences(Loop *L, SCEVResult &SE);
    static bool orientExitComparison(Loop *L, SCEVResult &SE);
};
