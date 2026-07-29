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

class Loop;
class SCEVResult;

// IntRange 是 i1/i32 的有符号区间格；Bottom 表示不可达，溢出风险统一退化为 Top。
struct IntRange {
    int64_t min;
    int64_t max;

    int knownTrailingZeros = 0;

    static constexpr int64_t I32_MIN = INT32_MIN;
    static constexpr int64_t I32_MAX = INT32_MAX;

    static int trailingZerosOf(int64_t v);

    IntRange();
    explicit IntRange(int64_t val);
    IntRange(int64_t lo, int64_t hi);

    bool isTop() const;
    bool isBottom() const;
    bool isSingleValue() const;
    bool isNonNegative() const;
    bool isPositive() const;

    bool isMultipleOf(int k) const;

    IntRange unionWith(const IntRange &other) const;
    IntRange intersectWith(const IntRange &other) const;

    bool operator==(const IntRange &o) const;
    bool operator!=(const IntRange &o) const;

    IntRange add(const IntRange &rhs) const;
    IntRange sub(const IntRange &rhs) const;
    IntRange mul(const IntRange &rhs) const;
    IntRange sdiv(const IntRange &rhs) const;
    IntRange srem(const IntRange &rhs) const;

    IntRange shl(const IntRange &rhs) const;
    IntRange ashr(const IntRange &rhs) const;
    IntRange lshr(const IntRange &rhs) const;
    IntRange band(const IntRange &rhs) const;
    IntRange bor(const IntRange &rhs) const;

    static IntRange bottom();
    static IntRange nonNegative();
};

// UIntRange 独立描述 i32 的无符号位模式区间；跨越回绕点而无法用单区间
// 表示时退化为 Top，不能把有符号端点直接当成无符号端点使用。
struct UIntRange {
    uint64_t min;
    uint64_t max;

    static constexpr uint64_t U32_MIN = 0;
    static constexpr uint64_t U32_MAX = UINT32_MAX;

    UIntRange();
    explicit UIntRange(uint64_t val);
    UIntRange(uint64_t lo, uint64_t hi);

    bool isTop() const;
    bool isBottom() const;
    bool isSingleValue() const;

    UIntRange unionWith(const UIntRange &other) const;
    UIntRange intersectWith(const UIntRange &other) const;

    bool operator==(const UIntRange &o) const;
    bool operator!=(const UIntRange &o) const;

    UIntRange add(const UIntRange &rhs) const;
    UIntRange sub(const UIntRange &rhs) const;
    UIntRange mul(const UIntRange &rhs) const;
    UIntRange udiv(const UIntRange &rhs) const;
    UIntRange urem(const UIntRange &rhs) const;
    UIntRange shl(const UIntRange &rhs) const;
    UIntRange lshr(const UIntRange &rhs) const;
    UIntRange band(const UIntRange &rhs) const;
    UIntRange bor(const UIntRange &rhs) const;

    IntRange toIntRange() const;
    static UIntRange fromIntRange(const IntRange &range);
    static UIntRange bottom();
};

// KnownBits 与 signed/unsigned 区间并行保存确定为零/一的位，并可双向收紧。
struct KnownBits {
    uint32_t zero = 0;
    uint32_t one  = 0;

    KnownBits();
    KnownBits(uint32_t z, uint32_t o);

    static KnownBits top();
    static KnownBits fromConst(int32_t v);

    bool isConflict() const;
    bool isTop() const;
    bool isFullyKnown() const;
    int32_t getConstant() const;

    int getTrailingZeros() const;

    int getTrailingOnes() const;

    bool knownNonNegative() const;
    bool knownNegative() const;

    KnownBits unionWith(const KnownBits &o) const;

    KnownBits intersectWith(const KnownBits &o) const;

    bool operator==(const KnownBits &o) const;
    bool operator!=(const KnownBits &o) const;

    KnownBits shl(int k) const;
    KnownBits lshr(int k) const;
    KnownBits ashr(int k) const;
    KnownBits band(const KnownBits &o) const;
    KnownBits bor(const KnownBits &o) const;
    KnownBits bxor(const KnownBits &o) const;

    KnownBits add(const KnownBits &o) const;
    KnownBits sub(const KnownBits &o) const;

    KnownBits mul(const KnownBits &o) const;

    IntRange toIntRange() const;
    UIntRange toUIntRange() const;

    static KnownBits fromUIntRange(const UIntRange &range);
};

// 三个整数抽象域在同一值上组成约化积。该函数只做保守交叉投影：
// 任一投影无法用单区间表达时不收窄，事实矛盾时统一返回 Bottom。
void reduceIntegerRangeProduct(IntRange &signedRange,
                               UIntRange &unsignedRange,
                               KnownBits &bits);

// FloatRange 用数值包络加 NaN 和有符号零标志描述 f32，避免实数区间丢失 IEEE 状态。
struct FloatRange {
    double min;
    double max;
    bool mayNaN;
    bool mayPositiveZero;
    bool mayNegativeZero;

    static double F_MIN_INF();
    static double F_MAX_INF();

    FloatRange();

    explicit FloatRange(double val);

    FloatRange(double lo, double hi);

    bool hasNumericValues() const;
    bool mayContainNaN() const;
    bool containsZero() const;
    bool mayContainInfinity() const;
    bool isTop() const;
    bool isBottom() const;
    bool isNaNOnly() const;
    bool isExactNumericValue() const;
    bool isSingleValue() const;
    bool isNonNegative() const;

    FloatRange unionWith(const FloatRange &other) const;
    FloatRange intersectWith(const FloatRange &other) const;

    bool operator==(const FloatRange &o) const;
    bool operator!=(const FloatRange &o) const;

    FloatRange add(const FloatRange &rhs) const;
    FloatRange sub(const FloatRange &rhs) const;
    FloatRange mul(const FloatRange &rhs) const;
    FloatRange fdiv(const FloatRange &rhs) const;

    static FloatRange sanitize(double Lo, double Hi, bool MayNaN = false);
    static FloatRange bottom();

private:
    static bool containsPosInf(const FloatRange &R);
    static bool containsNegInf(const FloatRange &R);
    static float exactF32Value(const FloatRange &R);
    static bool eitherNaNPropagates(const FloatRange &L,
                                    const FloatRange &R);

    // 浮点端点求值用真实 f32 运算；精确值保留零符号，异常组合单独传播 NaN。
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

// SCEV 向 RangeAnalysis 提供的归纳摘要只在有限且不回绕时形成循环体局部提示。
struct LoopInductionInfo {
    const Loop *RecLoop;
    int64_t initial;
    int64_t step;
    bool hasUpperBound;
    int64_t upperBound;
    bool noWrap;

    LoopInductionInfo();
};

// 分析结果分开保存全局、块局部和 CFG 边事实；查询时按支配上下文取交集。
class RangeInfo {
public:

    std::unordered_map<Value*, IntRange> GlobalRanges;
    std::unordered_map<Value*, UIntRange> GlobalUnsignedRanges;

    std::unordered_map<Value*, std::unordered_map<BasicBlock*, IntRange>> ContextualRanges;
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, UIntRange>>
        ContextualUnsignedRanges;

    std::unordered_map<
        Value*, std::unordered_map<
            BasicBlock*, std::unordered_map<BasicBlock*, IntRange>>> EdgeRanges;
    std::unordered_map<
        Value*, std::unordered_map<
            BasicBlock*, std::unordered_map<BasicBlock*, UIntRange>>>
        EdgeUnsignedRanges;

    std::unordered_map<Value*, std::unordered_map<BasicBlock*, IntRange>> BlockLocalRanges;
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, UIntRange>>
        BlockLocalUnsignedRanges;

    std::unordered_map<Value*, FloatRange> FloatGlobalRanges;

    std::unordered_map<Value*, std::unordered_map<BasicBlock*, FloatRange>> FloatContextualRanges;

    std::unordered_map<Value*, KnownBits> GlobalBits;
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, KnownBits>> ContextualBits;
    std::unordered_map<
        Value*, std::unordered_map<
            BasicBlock*, std::unordered_map<BasicBlock*, KnownBits>>> EdgeBits;

    std::unordered_map<Value*, LoopInductionInfo> LoopInductions;

    // 查询接口先读取全局格，再叠加受支配块或精确入边上的约束；缺失事实等价于 Top。
    IntRange query(Value *val, BasicBlock *ctxBlock, const DominatorTree &DT) const;
    IntRange query(Value *val, Instruction *ctxInst,
                   const DominatorTree &DT) const;

    UIntRange queryUnsigned(Value *val, BasicBlock *ctxBlock,
                            const DominatorTree &DT) const;
    UIntRange queryUnsigned(Value *val, Instruction *ctxInst,
                            const DominatorTree &DT) const;

    FloatRange queryFloat(Value *val, BasicBlock *ctxBlock, const DominatorTree &DT) const;

    IntRange queryOnEdge(Value *val, BasicBlock *predBB, BasicBlock *succBB,
                         const DominatorTree &DT) const;
    UIntRange queryUnsignedOnEdge(Value *val, BasicBlock *predBB,
                                  BasicBlock *succBB,
                                  const DominatorTree &DT) const;
    FloatRange queryFloatOnEdge(Value *val, BasicBlock *predBB, BasicBlock *succBB,
                                const DominatorTree &DT) const;

    // Select 的条件不会形成真实 CFG 边，因此按真假臂显式查询比较条件产生的
    // 指令级事实；不可执行的臂返回 Bottom。
    IntRange queryOnSelectArm(Value *val, SelectInst *select,
                              bool trueArm, const DominatorTree &DT) const;
    UIntRange queryUnsignedOnSelectArm(
        Value *val, SelectInst *select, bool trueArm,
        const DominatorTree &DT) const;

    KnownBits queryBits(Value *val) const;
    KnownBits queryBits(Value *val, BasicBlock *ctxBlock,
                        const DominatorTree &DT) const;
    KnownBits queryBitsOnEdge(Value *val, BasicBlock *predBB, BasicBlock *succBB,
                              const DominatorTree &DT) const;

    void setLoopInduction(Value *v, const LoopInductionInfo &info);

    bool invalidate(Function &F, const PreservationStatus &PA);
};

// 求解器分五阶段工作：前向固定点、预算化 narrowing、重建边事实、叠加 SCEV 提示、封装结果。
// 临时上下文只用于加速求解，变换可见事实必须从最终格重新构造。
class RangeSolver {
public:
    std::unordered_set<BasicBlock *> ExecutableBlocks;
    std::set<std::pair<BasicBlock *, BasicBlock *>> FeasibleEdges;
    std::vector<BasicBlock *> BBWorkList;
    std::vector<Instruction *> InstWorkList;

    std::unordered_map<Value *, IntRange> ValueRanges;
    std::unordered_map<Value *, UIntRange> UnsignedValueRanges;
    std::unordered_map<Value *, FloatRange> FloatValueRanges;

    std::unordered_map<Value *, std::unordered_map<BasicBlock *, IntRange>>
        ContextualRanges;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, UIntRange>>
        ContextualUnsignedRanges;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, IntRange>>
        BlockLocalRanges;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, UIntRange>>
        BlockLocalUnsignedRanges;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, FloatRange>>
        FloatContextualRanges;
    std::unordered_map<
        Value *, std::unordered_map<
            BasicBlock *, std::unordered_map<BasicBlock *, IntRange>>>
        EdgeRanges;
    std::unordered_map<
        Value *, std::unordered_map<
            BasicBlock *, std::unordered_map<BasicBlock *, UIntRange>>>
        EdgeUnsignedRanges;

    std::unordered_map<Value *, KnownBits> ValueBits;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, KnownBits>>
        ContextualBits;
    std::unordered_map<
        Value *, std::unordered_map<
            BasicBlock *, std::unordered_map<BasicBlock *, KnownBits>>>
        EdgeBits;

    static constexpr int MAX_ENQUEUE = 32;
    std::unordered_map<Value *, int> EnqueueCount;
    std::unordered_map<Value *, int> UnsignedEnqueueCount;
    std::unordered_set<Value *> Widened;
    std::unordered_set<Value *> UnsignedWidened;
    Function *CurFunc = nullptr;
    bool NarrowingMode = false;
    const std::unordered_map<Value *, LoopInductionInfo> *ScevInductions =
        nullptr;
    const DominatorTree *DomTree = nullptr;

    IntRange &getRange(Value *V);
    UIntRange getUnsignedRange(Value *V);
    KnownBits getBits(Value *V);
    IntRange getRangeInBlock(Value *V, BasicBlock *BB);
    UIntRange getUnsignedRangeInBlock(Value *V, BasicBlock *BB);
    KnownBits getBitsInBlock(Value *V, BasicBlock *BB);
    void enqueueInstructionUsers(Value *V);
    bool updateBits(Value *V, const KnownBits &NewBits);
    void updateBitsContextual(Value *V, BasicBlock *Block,
                              const KnownBits &Bits);
    bool updateUnsignedRange(Value *V, const UIntRange &NewRange);
    bool updateIntegerFacts(Value *V, const IntRange &NewSignedRange,
                            const UIntRange &NewUnsignedRange,
                            const KnownBits &NewBits);

    // 更新接口在前向阶段允许扩宽并设置收敛锁，在 narrowing 阶段仅允许取交变窄。
    bool updateRange(Value *V, const IntRange &NewRange);
    bool updateFloatRange(Value *V, const FloatRange &NewRange);
    void updateContextual(Value *V, BasicBlock *Block,
                          const IntRange &Range);
    void updateUnsignedContextual(Value *V, BasicBlock *Block,
                                  const UIntRange &Range);
    void updateFloatContextual(Value *V, BasicBlock *Block,
                               const FloatRange &Range);
    void markBlockExecutable(BasicBlock *BB);
    bool markEdgeFeasible(BasicBlock *From, BasicBlock *To);

    IntRange rangeFromInduction(const LoopInductionInfo &Info) const;
    void applySCEVContextHints(SCEVResult *SCEV);
    void visitPhi(PhiInst *Phi);
    void visitBinary(BinaryInst *BI);
    void narrowBinaryBackward(BinaryInst *BI, Value *LHS, Value *RHS,
                              const IntRange &LRange,
                              const IntRange &RRange);
    void visitBranch(BrInst *Br);
    void visitCall(CallInst *CI);
    void visitSelect(SelectInst *SI);
    void visitCast(Instruction *I);
    void visitInstruction(Instruction *I);
    void runForward();
    bool runNarrowing(Function &F);
    void rebuildIntegerContextualFacts(Function &F);
    RangeInfo solve(Function &F, SCEVResult *SCEV = nullptr,
                    const DominatorTree *DT = nullptr);
};

// Pass 入口获取 SCEV 与 DominatorTree，随后运行完整 RangeSolver 流程。
class RangeAnalysis : public AnalysisDescriptor<RangeAnalysis> {
public:
    using Result = RangeInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<RangeAnalysis>;
    inline static PassID Key;
};
