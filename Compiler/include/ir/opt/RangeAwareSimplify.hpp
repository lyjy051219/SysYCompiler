#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <unordered_set>

//IntRange表示RangeAnalysis求得的整数区间格值
struct IntRange;
//DominatorTree提供程序点范围查询所需的支配关系
class DominatorTree;
//RangeInfo提供整数、浮点和已知位的程序点事实
class RangeInfo;

//RangeAwareSimplifyPass依据范围和已知位证明化简整数及浮点表达式
class RangeAwareSimplifyPass : public PassDescriptor<RangeAwareSimplifyPass> {
public:
    //运行范围感知化简并返回分析保留状态
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //判断二元操作是否支持局部整数区间递归求值
    static bool isPureIntExprOp(BinaryInst::Operation Op);
    //在深度和环检测预算内递归求解同块整数表达式区间
    static IntRange queryIntExprImpl(
        Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT,
        int Depth, std::unordered_set<Value *> &Visiting);
    //查询并补强给定程序点的整数表达式区间
    static IntRange queryIntExpr(
        Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT);
    //根据区间和已知位判断整数值是否必定非负
    static bool isKnownNonNegative(
        Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT);
    //判断整数条件在给定程序点是否恒假、恒真或未知
    static int evalTruth(
        Value *V, BasicBlock *BB, RangeInfo &RI, DominatorTree &DT);
    //把已证非负的二次幂有符号除余改写为移位或掩码
    static bool strengthReduce(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    //折叠被区间或已知位证明为单值的纯计算
    static bool foldConstants(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    //折叠被整数或浮点范围证明恒真的比较
    static bool foldComparisons(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    //根据两个整数区间判断比较恒真、恒假或未知
    static int evalIntCmp(
        BinaryInst::Operation Op, const IntRange &LHS,
        const IntRange &RHS);
    //判断二元操作是否为支持的整数比较谓词
    static bool isIntCmpOp(BinaryInst::Operation Op);
    //折叠比较select结果时两臂均可证明的比较结论
    static bool foldIcmpOfSelect(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    //根据条件范围直接选择select的确定分支
    static bool foldSelectByConditionRange(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    //根据操作数符号范围折叠select构成的绝对值模式
    static bool foldAbsViaSelect(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    //根据两侧区间顺序折叠select构成的最小值或最大值模式
    static bool foldMinMaxViaSelect(
        Function &F, RangeInfo &RI, DominatorTree &DT);
    //把商区间受限的有符号余数改写为减法或单次条件校正
    static bool foldBoundedSignedRemainders(
        Function &F, RangeInfo &RI, DominatorTree &DT);
};
