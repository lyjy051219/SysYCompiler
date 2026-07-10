#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <unordered_set>

struct IntRange;
class DominatorTree;
class RangeInfo;

// 基于整数范围分析的指令化简：
// 强度削减（除法→移位、取模→位与）、常量折叠、比较折叠、死分支消除
class RangeAwareSimplifyPass : public PassDescriptor<RangeAwareSimplifyPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool isPureIntExprOp(BinaryInst::Operation Op);
    static IntRange queryIntExprImpl(
        Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT,
        int Depth, std::unordered_set<Value *> &Visiting);
    static IntRange queryIntExpr(
        Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT);
    static bool isKnownNonNegative(
        Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT);
    static int evalTruth(
        Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT);
    static bool strengthReduce(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    static bool foldConstants(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    static bool foldComparisons(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    static int evalIntCmp(
        BinaryInst::Operation Op, const IntRange &LHS,
        const IntRange &RHS);
    static bool isIntCmpOp(BinaryInst::Operation Op);
    static bool foldIcmpOfSelect(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    static bool foldSelectByConditionRange(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    static bool foldAbsViaSelect(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    static bool foldMinMaxViaSelect(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    static bool convertSignedToUnsigned(
        Function &F, RangeInfo &RI, DominatorTree &DT);
};
