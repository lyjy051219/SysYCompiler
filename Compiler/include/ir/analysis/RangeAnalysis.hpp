// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/Dominant.hpp"
#include "lib/CFG.hpp"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
#include <cstdint>
#include <climits>
#include <cmath>
#include <algorithm>
 
//Loop表示RangeAnalysis引用的自然循环
class Loop;
//SCEVResult提供归纳变量及其循环范围提示
class SCEVResult;

//IntRange表示i1/i32的有符号区间格，Bottom表示不可达，溢出风险退化为Top
struct IntRange {
    //min记录区间包含的最小有符号整数
    int64_t min;
    //max记录区间包含的最大有符号整数
    int64_t max;
    //knownTrailingZeros记录区间内所有值至少具有的低位零数量
    int knownTrailingZeros = 0;
    //I32_MIN表示抽象域能够建模的最小有符号整数
    static constexpr int64_t I32_MIN = INT32_MIN;
    //I32_MAX表示抽象域能够建模的最大有符号整数
    static constexpr int64_t I32_MAX = INT32_MAX;
    //统计给定二补码整数在i32位模式中的低位零数量
    static int trailingZerosOf(int64_t v);
    //构造覆盖整个i32有符号值域的Top区间
    IntRange();
    //构造只包含给定整数的单点区间
    explicit IntRange(int64_t val);
    //使用给定上下界构造有符号闭区间
    IntRange(int64_t lo, int64_t hi);
    //判断当前区间是否覆盖整个i32有符号值域
    bool isTop() const;
    //判断当前区间是否为空集Bottom
    bool isBottom() const;
    //判断当前区间是否只包含一个确定值
    bool isSingleValue() const;
    //判断当前区间中的所有值是否都非负
    bool isNonNegative() const;
    //判断当前区间中的所有值是否都为正数
    bool isPositive() const;
    //判断当前区间中的所有值是否都包含至少k个低位零
    bool isMultipleOf(int k) const;
    //返回当前区间与另一有符号区间的保守并集
    IntRange unionWith(const IntRange &other) const;
    //返回当前区间与另一有符号区间的交集
    IntRange intersectWith(const IntRange &other) const;
    //判断两个有符号区间及其低位零事实是否完全相同
    bool operator==(const IntRange &o) const;
    //判断两个有符号区间或其低位零事实是否不同
    bool operator!=(const IntRange &o) const;
    //计算当前区间与右操作数相加后的有符号区间
    IntRange add(const IntRange &rhs) const;
    //计算当前区间减去右操作数后的有符号区间
    IntRange sub(const IntRange &rhs) const;
    //计算当前区间与右操作数相乘后的有符号区间
    IntRange mul(const IntRange &rhs) const;
    //计算当前区间除以右操作数后的有符号区间
    IntRange sdiv(const IntRange &rhs) const;
    //计算当前区间对右操作数取余后的有符号区间
    IntRange srem(const IntRange &rhs) const;
    //计算当前区间按右操作数左移后的有符号区间
    IntRange shl(const IntRange &rhs) const;
    //计算当前区间按右操作数算术右移后的有符号区间
    IntRange ashr(const IntRange &rhs) const;
    //计算当前区间按右操作数逻辑右移后的有符号区间
    IntRange lshr(const IntRange &rhs) const;
    //计算当前区间与右操作数按位与后的有符号区间
    IntRange band(const IntRange &rhs) const;
    //计算当前区间与右操作数按位或后的有符号区间
    IntRange bor(const IntRange &rhs) const;
    //构造不包含任何值的Bottom区间
    static IntRange bottom();
    //构造覆盖所有非负i32值的区间
    static IntRange nonNegative();
};

//UIntRange独立描述i32无符号位模式区间，无法表示回绕区间时退化为Top
struct UIntRange {
    //min记录区间包含的最小无符号位模式
    uint64_t min;
    //max记录区间包含的最大无符号位模式
    uint64_t max;
    //U32_MIN表示抽象域能够建模的最小无符号整数
    static constexpr uint64_t U32_MIN = 0;
    //U32_MAX表示抽象域能够建模的最大无符号整数
    static constexpr uint64_t U32_MAX = UINT32_MAX;
    //构造覆盖整个i32无符号位模式空间的Top区间
    UIntRange();
    //构造只包含给定无符号整数的单点区间
    explicit UIntRange(uint64_t val);
    //使用给定上下界构造无符号闭区间
    UIntRange(uint64_t lo, uint64_t hi);
    //判断当前区间是否覆盖整个i32无符号位模式空间
    bool isTop() const;
    //判断当前区间是否为空集Bottom
    bool isBottom() const;
    //判断当前区间是否只包含一个确定值
    bool isSingleValue() const;
    //返回当前区间与另一无符号区间的保守并集
    UIntRange unionWith(const UIntRange &other) const;
    //返回当前区间与另一无符号区间的交集
    UIntRange intersectWith(const UIntRange &other) const;
    //判断两个无符号区间是否完全相同
    bool operator==(const UIntRange &o) const;
    //判断两个无符号区间是否不同
    bool operator!=(const UIntRange &o) const;
    //计算当前区间与右操作数相加后的无符号区间
    UIntRange add(const UIntRange &rhs) const;
    //计算当前区间减去右操作数后的无符号区间
    UIntRange sub(const UIntRange &rhs) const;
    //计算当前区间与右操作数相乘后的无符号区间
    UIntRange mul(const UIntRange &rhs) const;
    //计算当前区间除以右操作数后的无符号区间
    UIntRange udiv(const UIntRange &rhs) const;
    //计算当前区间对右操作数取余后的无符号区间
    UIntRange urem(const UIntRange &rhs) const;
    //计算当前区间按右操作数左移后的无符号区间
    UIntRange shl(const UIntRange &rhs) const;
    //计算当前区间按右操作数逻辑右移后的无符号区间
    UIntRange lshr(const UIntRange &rhs) const;
    //计算当前区间与右操作数按位与后的无符号区间
    UIntRange band(const UIntRange &rhs) const;
    //计算当前区间与右操作数按位或后的无符号区间
    UIntRange bor(const UIntRange &rhs) const;
    //把无符号位模式区间保守投影为有符号区间
    IntRange toIntRange() const;
    //把有符号区间保守投影为无符号位模式区间
    static UIntRange fromIntRange(const IntRange &range);
    //构造不包含任何值的Bottom区间
    static UIntRange bottom();
};

//KnownBits保存i32中确定为零或一的位并与有符号和无符号区间相互收紧
struct KnownBits {
    //zero的置位位置表示对应位确定为零
    uint32_t zero = 0;
    //one的置位位置表示对应位确定为一
    uint32_t one  = 0;
    //构造不包含任何确定位的Top位事实
    KnownBits();
    //使用确定为零和确定为一的掩码构造位事实
    KnownBits(uint32_t z, uint32_t o);
    //构造不包含任何确定位的Top位事实
    static KnownBits top();
    //从给定i32常量构造所有位均确定的位事实
    static KnownBits fromConst(int32_t v);
    //判断是否有某一位同时被标记为零和一
    bool isConflict() const;
    //判断当前位事实是否未确定任何位
    bool isTop() const;
    //判断当前位事实是否确定了全部32位
    bool isFullyKnown() const;
    //返回全部位已知时对应的i32常量
    int32_t getConstant() const;
    //返回当前位事实能够保证的低位零数量
    int getTrailingZeros() const;
    //返回当前位事实能够保证的低位一数量
    int getTrailingOnes() const;
    //判断符号位是否确定为零
    bool knownNonNegative() const;
    //判断符号位是否确定为一
    bool knownNegative() const;
    //返回两组控制流事实汇合后共同确定的位
    KnownBits unionWith(const KnownBits &o) const;
    //返回同时满足两组约束的确定位事实
    KnownBits intersectWith(const KnownBits &o) const;
    //判断两组确定位掩码是否完全相同
    bool operator==(const KnownBits &o) const;
    //判断两组确定位掩码是否不同
    bool operator!=(const KnownBits &o) const;
    //计算左移给定位数后的确定位事实
    KnownBits shl(int k) const;
    //计算逻辑右移给定位数后的确定位事实
    KnownBits lshr(int k) const;
    //计算算术右移给定位数后的确定位事实
    KnownBits ashr(int k) const;
    //计算与另一位事实按位与后的确定位事实
    KnownBits band(const KnownBits &o) const;
    //计算与另一位事实按位或后的确定位事实
    KnownBits bor(const KnownBits &o) const;
    //计算与另一位事实按位异或后的确定位事实
    KnownBits bxor(const KnownBits &o) const;
    //计算与另一位事实相加后的确定位事实
    KnownBits add(const KnownBits &o) const;
    //计算减去另一位事实后的确定位事实
    KnownBits sub(const KnownBits &o) const;
    //计算与另一位事实相乘后的确定位事实
    KnownBits mul(const KnownBits &o) const;
    //把确定位事实投影为有符号整数区间
    IntRange toIntRange() const;
    //把确定位事实投影为无符号位模式区间
    UIntRange toUIntRange() const;
    //从无符号区间推导必然确定的位
    static KnownBits fromUIntRange(const UIntRange &range);
};

//在有符号区间、无符号区间和确定位之间执行保守交叉约化
void reduceIntegerRangeProduct(IntRange &signedRange,
                               UIntRange &unsignedRange,
                               KnownBits &bits);

//FloatRange使用数值包络、NaN和有符号零标志描述f32抽象值
struct FloatRange {
    //min记录可能数值中的最小浮点值
    double min;
    //max记录可能数值中的最大浮点值
    double max;
    //mayNaN标记抽象值是否可能为NaN
    bool mayNaN;
    //mayPositiveZero标记抽象值是否可能包含正零
    bool mayPositiveZero;
    //mayNegativeZero标记抽象值是否可能包含负零
    bool mayNegativeZero;
    //返回浮点抽象域使用的负无穷下界
    static double F_MIN_INF();
    //返回浮点抽象域使用的正无穷上界
    static double F_MAX_INF();
    //构造包含全部f32数值状态的Top区间
    FloatRange();
    //构造只包含给定浮点值的精确区间
    explicit FloatRange(double val);
    //使用给定上下界构造数值闭区间
    FloatRange(double lo, double hi);
    //判断当前抽象值是否包含至少一个非NaN数值
    bool hasNumericValues() const;
    //判断当前抽象值是否可能包含NaN
    bool mayContainNaN() const;
    //判断当前数值包络是否可能包含正零或负零
    bool containsZero() const;
    //判断当前抽象值是否可能包含正无穷或负无穷
    bool mayContainInfinity() const;
    //判断当前抽象值是否覆盖全部f32状态
    bool isTop() const;
    //判断当前抽象值是否不包含任何状态
    bool isBottom() const;
    //判断当前抽象值是否只可能为NaN
    bool isNaNOnly() const;
    //判断当前抽象值是否只包含一个确定数值及其零符号
    bool isExactNumericValue() const;
    //判断当前抽象值是否只表示一个非NaN值
    bool isSingleValue() const;
    //判断当前抽象值中的全部数值是否非负且不含NaN
    bool isNonNegative() const;
    //返回当前浮点区间与另一浮点区间的保守并集
    FloatRange unionWith(const FloatRange &other) const;
    //返回当前浮点区间与另一浮点区间的交集
    FloatRange intersectWith(const FloatRange &other) const;
    //判断两个浮点区间的数值包络和IEEE状态是否完全相同
    bool operator==(const FloatRange &o) const;
    //判断两个浮点区间的数值包络或IEEE状态是否不同
    bool operator!=(const FloatRange &o) const;
    //计算当前区间与右操作数相加后的浮点区间
    FloatRange add(const FloatRange &rhs) const;
    //计算当前区间减去右操作数后的浮点区间
    FloatRange sub(const FloatRange &rhs) const;
    //计算当前区间与右操作数相乘后的浮点区间
    FloatRange mul(const FloatRange &rhs) const;
    //计算当前区间除以右操作数后的浮点区间
    FloatRange fdiv(const FloatRange &rhs) const;
    //规范化给定包络及NaN标志并构造浮点区间
    static FloatRange sanitize(double Lo, double Hi, bool MayNaN = false);
    //构造不包含任何浮点状态的Bottom区间
    static FloatRange bottom();

private:
    //判断当前浮点区间是否包含正无穷
    static bool containsPosInf(const FloatRange &R);
    //判断当前浮点区间是否包含负无穷
    static bool containsNegInf(const FloatRange &R);
    //返回精确单点区间对应的真实f32值并保留零符号
    static float exactF32Value(const FloatRange &R);
    //判断两个操作数的NaN状态是否会传播到结果
    static bool eitherNaNPropagates(const FloatRange &L,
                                    const FloatRange &R);
    //使用给定f32运算求值端点并传播NaN和有符号零状态
    template <typename Fn>
    static FloatRange evaluateFloatEndpoints(const FloatRange &L,
                                             const FloatRange &R,
                                             bool MayNaN, Fn Eval) {
        FloatRange Result = FloatRange::bottom();
        Result.mayNaN = MayNaN;

        if (!L.hasNumericValues() || !R.hasNumericValues()) return Result;

        if (L.isExactNumericValue() && R.isExactNumericValue()) {
            float V = Eval(exactF32Value(L), exactF32Value(R));
            FloatRange Exact(static_cast<double>(V));
            Exact.mayNaN = Exact.mayNaN || MayNaN;
            return Exact;
        }

        const float LVals[2] = {static_cast<float>(L.min),
                                static_cast<float>(L.max)};
        const float RVals[2] = {static_cast<float>(R.min),
                                static_cast<float>(R.max)};
        bool HaveNumeric = false;
        double Lo = 0.0;
        double Hi = 0.0;
        bool SawNaN = MayNaN;
        for (float LV : LVals) {
            for (float RV : RVals) {
                float V = Eval(LV, RV);
                if (std::isnan(V)) {
                    SawNaN = true;
                    continue;
                }
                double D = static_cast<double>(V);
                if (!HaveNumeric) {
                    Lo = Hi = D;
                    HaveNumeric = true;
                } else {
                    if (D < Lo) Lo = D;
                    if (D > Hi) Hi = D;
                }
            }
        }

        if (!HaveNumeric) {
            Result.mayNaN = SawNaN;
            return Result;
        }

        Result = FloatRange(Lo, Hi);
        Result.mayNaN = SawNaN;

        if (Lo <= 0.0 && Hi >= 0.0) {
            Result.mayPositiveZero = true;
            Result.mayNegativeZero = true;
        }
        return Result;
    }
};

//RangeValueSummary保存跨直接调用边传播的标量参数或返回值抽象事实
struct RangeValueSummary {
    //HasInteger标记当前摘要包含i1/i32整数事实
    bool HasInteger = false;
    //SignedRange保存整数的有符号区间
    IntRange SignedRange;
    //UnsignedRange保存整数的无符号位模式区间
    UIntRange UnsignedRange;
    //Bits保存整数的确定位事实
    KnownBits Bits;
    //HasFloat标记当前摘要包含f32浮点事实
    bool HasFloat = false;
    //FloatValueRange保存浮点数值及NaN/有符号零状态
    FloatRange FloatValueRange;

    //把另一条调用路径的抽象事实并入当前摘要
    void merge(const RangeValueSummary &Other);
    //判断两个摘要是否携带完全相同的抽象事实
    bool operator==(const RangeValueSummary &Other) const;
    //判断两个摘要是否不同
    bool operator!=(const RangeValueSummary &Other) const;
};

//LoopInductionInfo保存SCEV提供的有限无回绕循环归纳摘要
struct LoopInductionInfo {
    //RecLoop指向归纳变量所属的自然循环
    const Loop *RecLoop;
    //initial记录归纳变量的常量初值
    int64_t initial;
    //step记录每次迭代增加的常量步长
    int64_t step;
    //hasUpperBound标记是否存在可用的常量上界
    bool hasUpperBound;
    //upperBound记录归纳变量可达到的常量上界
    int64_t upperBound;
    //noWrap标记递推过程是否已证明不会发生i32回绕
    bool noWrap;
    //构造没有归纳事实的空摘要
    LoopInductionInfo();
};

//RangeInfo保存全局、块局部和CFG边上的范围事实并提供上下文查询
class RangeInfo {
public:
    //GlobalRanges保存每个整数值的全局有符号区间
    std::unordered_map<Value*, IntRange> GlobalRanges;
    //GlobalUnsignedRanges保存每个整数值的全局无符号区间
    std::unordered_map<Value*, UIntRange> GlobalUnsignedRanges;
    //ContextualRanges保存值在受支配基本块中的有符号局部约束
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, IntRange>> ContextualRanges;
    //ContextualUnsignedRanges保存值在受支配基本块中的无符号局部约束
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, UIntRange>>
        ContextualUnsignedRanges;
    //EdgeRanges保存值沿指定CFG边成立的有符号区间
    std::unordered_map<
        Value*, std::unordered_map<
            BasicBlock*, std::unordered_map<BasicBlock*, IntRange>>> EdgeRanges;
    //EdgeUnsignedRanges保存值沿指定CFG边成立的无符号区间
    std::unordered_map<
        Value*, std::unordered_map<
            BasicBlock*, std::unordered_map<BasicBlock*, UIntRange>>>
        EdgeUnsignedRanges;
    //BlockLocalRanges保存只在单个基本块内部成立的有符号事实
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, IntRange>> BlockLocalRanges;
    //BlockLocalUnsignedRanges保存只在单个基本块内部成立的无符号事实
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, UIntRange>>
        BlockLocalUnsignedRanges;
    //FloatGlobalRanges保存每个浮点值的全局浮点区间
    std::unordered_map<Value*, FloatRange> FloatGlobalRanges;
    //FloatContextualRanges保存浮点值在受支配基本块中的局部约束
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, FloatRange>> FloatContextualRanges;
    //GlobalBits保存每个整数值的全局确定位事实
    std::unordered_map<Value*, KnownBits> GlobalBits;
    //ContextualBits保存值在受支配基本块中的确定位事实
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, KnownBits>> ContextualBits;
    //EdgeBits保存值沿指定CFG边成立的确定位事实
    std::unordered_map<
        Value*, std::unordered_map<
            BasicBlock*, std::unordered_map<BasicBlock*, KnownBits>>> EdgeBits;
    //LoopInductions保存值对应的SCEV循环归纳摘要
    std::unordered_map<Value*, LoopInductionInfo> LoopInductions;
    //查询值在给定基本块上下文中的有符号区间
    IntRange query(Value *val, BasicBlock *ctxBlock, const DominatorTree &DT) const;
    //查询值在给定指令位置上的有符号区间
    IntRange query(Value *val, Instruction *ctxInst,
                   const DominatorTree &DT) const;
    //查询值在给定基本块上下文中的无符号区间
    UIntRange queryUnsigned(Value *val, BasicBlock *ctxBlock,
                            const DominatorTree &DT) const;
    //查询值在给定指令位置上的无符号区间
    UIntRange queryUnsigned(Value *val, Instruction *ctxInst,
                            const DominatorTree &DT) const;
    //查询值在给定基本块上下文中的浮点区间
    FloatRange queryFloat(Value *val, BasicBlock *ctxBlock, const DominatorTree &DT) const;
    //查询值沿指定CFG边成立的有符号区间
    IntRange queryOnEdge(Value *val, BasicBlock *predBB, BasicBlock *succBB,
                         const DominatorTree &DT) const;
    //查询值沿指定CFG边成立的无符号区间
    UIntRange queryUnsignedOnEdge(Value *val, BasicBlock *predBB,
                                  BasicBlock *succBB,
                                  const DominatorTree &DT) const;
    //查询值沿指定CFG边成立的浮点区间
    FloatRange queryFloatOnEdge(Value *val, BasicBlock *predBB, BasicBlock *succBB,
                                const DominatorTree &DT) const;
    //查询值在Select真假臂之一成立时的有符号区间
    IntRange queryOnSelectArm(Value *val, SelectInst *select,
                              bool trueArm, const DominatorTree &DT) const;
    //查询值在Select真假臂之一成立时的无符号区间
    UIntRange queryUnsignedOnSelectArm(
        Value *val, SelectInst *select, bool trueArm,
        const DominatorTree &DT) const;
    //查询值的全局确定位事实
    KnownBits queryBits(Value *val) const;
    //查询值在给定基本块上下文中的确定位事实
    KnownBits queryBits(Value *val, BasicBlock *ctxBlock,
                        const DominatorTree &DT) const;
    //查询值沿指定CFG边成立的确定位事实
    KnownBits queryBitsOnEdge(Value *val, BasicBlock *predBB, BasicBlock *succBB,
                              const DominatorTree &DT) const;
    //为给定值记录SCEV提供的循环归纳摘要
    void setLoopInduction(Value *v, const LoopInductionInfo &info);
    //根据PreservationStatus判断当前分析结果是否失效
    bool invalidate(Function &F, const PreservationStatus &PA);
};

//RangeSolver通过前向固定点、变窄、事实重建和SCEV提示求解范围分析
class RangeSolver {
public:
    //ExecutableBlocks记录当前已证明可执行的基本块
    std::unordered_set<BasicBlock *> ExecutableBlocks;
    //FeasibleEdges记录当前已证明可行的CFG边
    std::set<std::pair<BasicBlock *, BasicBlock *>> FeasibleEdges;
    //BBWorkList保存等待处理的可执行基本块
    std::vector<BasicBlock *> BBWorkList;
    //InstWorkList保存因操作数更新而等待重算的指令
    std::vector<Instruction *> InstWorkList;
    //ValueRanges保存求解中的全局有符号区间
    std::unordered_map<Value *, IntRange> ValueRanges;
    //UnsignedValueRanges保存求解中的全局无符号区间
    std::unordered_map<Value *, UIntRange> UnsignedValueRanges;
    //FloatValueRanges保存求解中的全局浮点区间
    std::unordered_map<Value *, FloatRange> FloatValueRanges;
    //ContextualRanges保存求解中的块上下文有符号区间
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, IntRange>>
        ContextualRanges;
    //ContextualUnsignedRanges保存求解中的块上下文无符号区间
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, UIntRange>>
        ContextualUnsignedRanges;
    //BlockLocalRanges保存求解中的块内有符号事实
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, IntRange>>
        BlockLocalRanges;
    //BlockLocalUnsignedRanges保存求解中的块内无符号事实
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, UIntRange>>
        BlockLocalUnsignedRanges;
    //FloatContextualRanges保存求解中的块上下文浮点区间
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, FloatRange>>
        FloatContextualRanges;
    //EdgeRanges保存求解中的CFG边有符号区间
    std::unordered_map<
        Value *, std::unordered_map<
            BasicBlock *, std::unordered_map<BasicBlock *, IntRange>>>
        EdgeRanges;
    //EdgeUnsignedRanges保存求解中的CFG边无符号区间
    std::unordered_map<
        Value *, std::unordered_map<
            BasicBlock *, std::unordered_map<BasicBlock *, UIntRange>>>
        EdgeUnsignedRanges;
    //ValueBits保存求解中的全局确定位事实
    std::unordered_map<Value *, KnownBits> ValueBits;
    //ContextualBits保存求解中的块上下文确定位事实
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, KnownBits>>
        ContextualBits;
    //EdgeBits保存求解中的CFG边确定位事实
    std::unordered_map<
        Value *, std::unordered_map<
            BasicBlock *, std::unordered_map<BasicBlock *, KnownBits>>>
        EdgeBits;
    //MAX_ENQUEUE限制单个值触发用户重新入队的次数
    static constexpr int MAX_ENQUEUE = 32;
    //EnqueueCount记录有符号区间更新触发的入队次数
    std::unordered_map<Value *, int> EnqueueCount;
    //UnsignedEnqueueCount记录无符号区间更新触发的入队次数
    std::unordered_map<Value *, int> UnsignedEnqueueCount;
    //Widened记录已因预算耗尽而扩宽到Top的有符号值
    std::unordered_set<Value *> Widened;
    //UnsignedWidened记录已因预算耗尽而扩宽到Top的无符号值
    std::unordered_set<Value *> UnsignedWidened;
    //CurFunc指向当前正在求解的函数
    Function *CurFunc = nullptr;
    //NarrowingMode标记求解器当前是否处于变窄阶段
    bool NarrowingMode = false;
    //ScevInductions指向当前可用的SCEV循环归纳摘要
    const std::unordered_map<Value *, LoopInductionInfo> *ScevInductions =
        nullptr;
    //DomTree指向当前函数的支配树
    const DominatorTree *DomTree = nullptr;
    //ParameterSeeds保存由全部直接调用点汇合得到的形参初始事实
    const std::unordered_map<Value *, RangeValueSummary> *ParameterSeeds = nullptr;
    //DirectReturnSummaries保存普通直接被调函数的返回值摘要
    const std::unordered_map<Function *, RangeValueSummary> *
        DirectReturnSummaries = nullptr;
    //返回值的有符号区间引用并按类型或常量初始化
    IntRange &getRange(Value *V);
    //返回值的无符号区间并按类型或常量初始化
    UIntRange getUnsignedRange(Value *V);
    //返回值的确定位事实并按常量或现有范围初始化
    KnownBits getBits(Value *V);
    //返回值在给定基本块上下文中的有符号区间
    IntRange getRangeInBlock(Value *V, BasicBlock *BB);
    //返回值在给定基本块上下文中的无符号区间
    UIntRange getUnsignedRangeInBlock(Value *V, BasicBlock *BB);
    //返回值在给定基本块上下文中的确定位事实
    KnownBits getBitsInBlock(Value *V, BasicBlock *BB);
    //把给定值的所有指令用户加入重新求解队列
    void enqueueInstructionUsers(Value *V);
    //使用新事实更新值的全局确定位并返回是否发生变化
    bool updateBits(Value *V, const KnownBits &NewBits);
    //把确定位事实合并到值的块上下文约束
    void updateBitsContextual(Value *V, BasicBlock *Block,
                              const KnownBits &Bits);
    //使用新区间更新值的全局无符号范围并返回是否变化
    bool updateUnsignedRange(Value *V, const UIntRange &NewRange);
    //约化并同时更新值的有符号、无符号和确定位事实
    bool updateIntegerFacts(Value *V, const IntRange &NewSignedRange,
                            const UIntRange &NewUnsignedRange,
                            const KnownBits &NewBits);
    //使用新区间更新值的全局有符号范围并返回是否变化
    bool updateRange(Value *V, const IntRange &NewRange);
    //使用新区间更新值的全局浮点范围并返回是否变化
    bool updateFloatRange(Value *V, const FloatRange &NewRange);
    //把有符号区间合并到值的块上下文约束
    void updateContextual(Value *V, BasicBlock *Block,
                          const IntRange &Range);
    //把无符号区间合并到值的块上下文约束
    void updateUnsignedContextual(Value *V, BasicBlock *Block,
                                  const UIntRange &Range);
    //把浮点区间合并到值的块上下文约束
    void updateFloatContextual(Value *V, BasicBlock *Block,
                               const FloatRange &Range);
    //标记基本块可执行并在首次发现时加入工作列表
    void markBlockExecutable(BasicBlock *BB);
    //标记CFG边可行并在首次发现时激活目标基本块
    bool markEdgeFeasible(BasicBlock *From, BasicBlock *To);
    //把有限无回绕的SCEV归纳摘要转换为有符号区间
    IntRange rangeFromInduction(const LoopInductionInfo &Info) const;
    //把SCEV循环归纳摘要应用为循环体上下文提示
    void applySCEVContextHints(SCEVResult *SCEV);
    //求值PHI并汇合所有可行入边上的抽象事实
    void visitPhi(PhiInst *Phi);
    //根据二元指令操作码传播整数或浮点范围事实
    void visitBinary(BinaryInst *BI);
    //从二元指令结果反向收紧两个操作数的有符号区间
    void narrowBinaryBackward(BinaryInst *BI, Value *LHS, Value *RHS,
                              const IntRange &LRange,
                              const IntRange &RRange);
    //根据分支条件更新可行边及边上下文约束
    void visitBranch(BrInst *Br);
    //根据已知库函数语义传播调用结果范围
    void visitCall(CallInst *CI);
    //根据条件和两个候选值传播Select结果范围
    void visitSelect(SelectInst *SI);
    //根据类型转换语义传播源值与结果值的范围
    void visitCast(Instruction *I);
    //按照具体指令类型分派范围传递逻辑
    void visitInstruction(Instruction *I);
    //运行工作列表驱动的前向固定点求解
    void runForward();
    //执行预算化反向传播以收紧前向固定点结果
    bool runNarrowing(Function &F);
    //根据最终整数格重新构建块局部和CFG边上下文事实
    void rebuildIntegerContextualFacts(Function &F);
    //运行完整求解流程并封装RangeInfo结果
    RangeInfo solve(
        Function &F, SCEVResult *SCEV = nullptr,
        const DominatorTree *DT = nullptr,
        const std::unordered_map<Value *, RangeValueSummary> *Seeds = nullptr,
        const std::unordered_map<Function *, RangeValueSummary> *
            ReturnSummaries = nullptr);
};

//RangeAnalysis是构建函数级范围信息的分析Pass
class RangeAnalysis : public AnalysisDescriptor<RangeAnalysis> {
public:
    //分析Pass返回的结果类型
    using Result = RangeInfo;
    //跨函数调用边变化时必须同步失效全部已发布范围摘要
    static constexpr bool InvalidateAcrossIRUnits = true;
    //运行范围分析并返回函数的完整范围信息
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    //允许AnalysisDescriptor访问本分析Pass的唯一标识
    friend AnalysisDescriptor<RangeAnalysis>;
    //当前分析Pass的唯一标识
    inline static PassID Key;
};
