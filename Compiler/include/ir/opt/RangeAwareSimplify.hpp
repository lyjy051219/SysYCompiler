#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <unordered_set>

struct IntRange;
class DominatorTree;
class RangeInfo;

 
 
// 流程：1. 用范围与已知位补强局部表达式事实；2. 折叠单值和恒定比较；
// 3. 化简比较/select 组合及 abs/min/max；4. 在非负证明下削减 2 次幂除余。
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
