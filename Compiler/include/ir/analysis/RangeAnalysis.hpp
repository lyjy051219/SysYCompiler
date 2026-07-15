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

// =====================================================================
// RangeAnalysis — 值范围分析
//
// 在 SCCP 的 worklist 不动点框架上扩展
// SCCP 的 lattice 是 {Top, Constant, Bottom}，
// RangeAnalysis 把 lattice 扩展为 {Top, [min,max], Bottom}，
// 能追踪非常量但有界的值（例如循环变量、条件分支后的范围收窄）。
//
// 采用和 SCCP 相同的基础设施：
//   ExecutableBlocks — 可达块集合
//   FeasibleEdges   — 可达边集合
//   BBWorkList      — 块工作队列
//   InstWorkList    — 指令工作队列
//
// 【范围表示】
// 整数：IntRange，用 int64_t 存 i32 范围避免算术溢出
// 浮点：FloatRange 用 double 存 f32 数值包络，并显式跟踪 NaN
//        与 +/-0。这些状态不能只靠 [min,max] 表示：NaN 不参与
//        有序比较，而 +0/-0 在数值上相等但可被后续除法区分。
// 两者不走模板——分开写更清晰，也和别家实现保持结构差异
//
// 【上下文追踪】
// solver 内部的临时分支/GEP fact 可能早于 global lattice 收敛。
// 求解完成后会丢弃这些临时状态，再从当前 CFG 和最终 global fact
// 线性重建整数 contextual fact，避免把收敛前的过窄约束暴露给变换。
// SCEV 的显式 loop-local hint 仍通过独立的 BlockLocalRanges 提供。
//
// 【收敛保证】
// 每个值的范围更新次数累计超过 32 次时，直接 widen 到 Top 并锁定不再变窄
// 这保证分析一定收敛到保守结果，不会因为循环归纳变量反复扩宽而卡死
// MAX_ENQUEUE 是调节精度和性能的旋钮，可按需调整
//
// 【Narrowing 阶段】
// Forward 收敛后追加一轮 narrowing：updateRange 切到 intersect-only 语义
// （不再 widen、Widened 锁失效），重新跑 visit 一轮。backward 推导仅覆盖
// 已证明不会 i32 wrap 的整数 ADD/SUB；浮点舍入与 MUL/DIV/MOD 溢出语义
// 暂不做反推。
// 轮次由一个 O(instCount * 4) 预算硬兜底；若预算耗尽，整份变换可见结果
// 降级为 Top，绝不暴露部分 narrowing 的下近似。
//
// GEP/Load/Store 不作为值范围证明。仅因程序执行了
// arr[i] 就假定 i 合法，属于利用源程序未定义行为，不能
// 用于竞赛优化。变换可见范围只来自显式分支事实和
// SCEV 已证明的有限循环归纳范围。
//
// 【跨函数 Return Range】
// Function analysis cache 没有 module/call-graph proxy，callee IR 改写后
// caller 可能先重算并读到旧的 singleton summary。因此当前版本
// 不消费 process-static return cache：用户函数调用结果一律 Top。
// 只有实现 Module 级 call-graph SCC fixed-point 和正确 invalidation proxy
// 后才能重新开启。内联后的函数内 range 优化不受影响。
// =====================================================================

// ---------------------------------------------------------------------
// 整数范围
// ---------------------------------------------------------------------
struct IntRange {
    int64_t min;
    int64_t max;

    // KnownBits-lite：已知 LSB 连续有多少位一定是 0（alignment 信息）。
    // 只追踪 trailing zeros，不做完整 known-ones/known-zeros lattice。足够
    // 表达"值是 2^k 的倍数"，用于：
    //   - shl x, c 的结果对齐变紧
    //   - mul x, pow2 的结果对齐变紧
    //   - and/or with const mask 的结果对齐变紧
    // 0 表示"没有对齐信息"，常量值从构造时算出来（奇数 = 0）。
    int knownTrailingZeros = 0;

    static constexpr int64_t I32_MIN = INT32_MIN;
    static constexpr int64_t I32_MAX = INT32_MAX;

    // 计算一个整数值的 trailing zero 数（0 → 31 上限避免无意义无穷）
    static int trailingZerosOf(int64_t v);

    IntRange();
    explicit IntRange(int64_t val);
    IntRange(int64_t lo, int64_t hi);

    // isTop 只回答"值域是不是 i32 全范围" —— 和 KnownBits lattice 解耦。
    // 之前把 knownTrailingZeros==0 也塞进判定，结果：range 全范围但 tz 推出
    // 非零时会被当成"非 Top"，下游做端点比较又退化为无意义的 -1，纯精度
    // 损失没带来任何好处；现在还原成纯 range lattice 的语义。
    bool isTop() const;
    bool isBottom() const;
    bool isSingleValue() const;
    bool isNonNegative() const;
    bool isPositive() const;
    // 是否是 2^k 的倍数（k > 0 即至少偶数）
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

    // 位运算（扩展）：
    //   shl/lshr/ashr：移位量非常量时保守退化成 Top；常量移位走"按端点算+clamp"的
    //     保守估计（负数 shl 可能溢出，统一用 int64 算再 clamp i32）
    //   band/bor：只对非负或常量做安全估计，其余退到 Top
    IntRange shl(const IntRange &rhs) const;
    IntRange ashr(const IntRange &rhs) const;
    IntRange lshr(const IntRange &rhs) const;
    IntRange band(const IntRange &rhs) const;
    IntRange bor(const IntRange &rhs) const;

    static IntRange bottom();
    static IntRange nonNegative();
};

// ---------------------------------------------------------------------
// KnownBits — 32 位级别的"已知为 0 / 已知为 1"双掩码
//
// 两套 bitmask：
//   zero 的 bit i = 1 表示原值的第 i 位确定是 0
//   one  的 bit i = 1 表示原值的第 i 位确定是 1
//   zero & one 必须 = 0（否则冲突，对应 lattice 的 bottom = 不可达）
//   二者都为 0 即 Top（完全未知）
//
// 和 IntRange 的关系：
//   - IntRange 管"值域"（signed [min, max]）
//   - KnownBits 管"位级事实"（哪些 bit 锁死）
//   - 二者独立推导、独立存储；最后 toIntRange() 把 KB 反推成 IntRange，
//     solver 在更新 range 时会和它 intersect，两套 lattice 收敛到共同的
//     紧 bound
//
// 优势：and/or/xor/shl 这类位运算在纯 IntRange 上几乎退 Top；KB 能精确
//       追踪、再反哺 IntRange（KB 证 bit31=0 → IntRange 非负）
// ---------------------------------------------------------------------
struct IntRange; // 向前声明，供 KnownBits::toIntRange 使用

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

    // 低位连续的 known-zero 数（LSB 对齐信息）
    int getTrailingZeros() const;
    // 低位连续的 known-one 数
    int getTrailingOnes() const;
    // 符号位已知为 0 / 1
    bool knownNonNegative() const;
    bool knownNegative() const;

    // union（PHI merge）：同时确定且同值的 bit 才保留
    KnownBits unionWith(const KnownBits &o) const;
    // intersect（narrowing）：任一侧确定就保留；冲突 bit 同时丢掉避免污染
    KnownBits intersectWith(const KnownBits &o) const;

    bool operator==(const KnownBits &o) const;
    bool operator!=(const KnownBits &o) const;

    // 位运算 / 移位的 KB 传播
    // k==0 是 identity（原值不变，不能退 Top 丢信息）；k 超范围才保守 Top
    KnownBits shl(int k) const;
    KnownBits lshr(int k) const;
    KnownBits ashr(int k) const;
    KnownBits band(const KnownBits &o) const;
    KnownBits bor(const KnownBits &o) const;
    KnownBits bxor(const KnownBits &o) const;
    // add/sub：我们只保留"两侧 trailing zeros 的下界"（进位会污染所有其它位）
    KnownBits add(const KnownBits &o) const;
    KnownBits sub(const KnownBits &o) const;
    // mul：trailing zeros 相加；其余位全退 unknown
    KnownBits mul(const KnownBits &o) const;

    // 反推 IntRange（和 IntRange 联动的唯一桥）
    IntRange toIntRange() const;
};

// ---------------------------------------------------------------------
// 浮点范围
//
// [min,max] 只包络非 NaN 数值。mayNaN 单独表示 NaN 成员；
// mayPositiveZero/mayNegativeZero 保留 IEEE-754 零的符号。所有
// transform-facing 的“单值”证明都必须同时排除 NaN/Inf，并且
// 对数值 0 只能有一种符号。
// ---------------------------------------------------------------------
struct FloatRange {
    double min;
    double max;
    bool mayNaN;
    bool mayPositiveZero;
    bool mayNegativeZero;

    static double F_MIN_INF();
    static double F_MAX_INF();

    // Top: any f32 value, including NaN and either signed zero.
    FloatRange();

    // Exact IEEE value.  In particular this preserves the sign of zero and
    // represents an exact NaN as "NaN-only", not as an unconstrained number.
    explicit FloatRange(double val);

    // Numeric interval known not to contain NaN.  An interval boundary at
    // numerical zero cannot distinguish its sign, so both zeros are retained.
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

    // 端点求值依赖具体算术操作，保留为真正的函数模板以避免类型擦除开销。
    template <typename Fn>
    static FloatRange evaluateFloatEndpoints(const FloatRange &L,
                                             const FloatRange &R,
                                             bool MayNaN, Fn Eval) {
        FloatRange Result = FloatRange::bottom();
        Result.mayNaN = MayNaN;

        if (!L.hasNumericValues() || !R.hasNumericValues()) return Result;

        // 精确值分支用于保留零的符号；区间包络在零可达时会保留两种符号。
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
        // 包络跨过数值零时，两种有符号零都可能由浮点舍入产生。
        if (Lo <= 0.0 && Hi >= 0.0) {
            Result.mayPositiveZero = true;
            Result.mayNegativeZero = true;
        }
        return Result;
    }
};

// ---------------------------------------------------------------------
// SCEV/RangeAnalysis 共享的简单整数归纳变量摘要。
// RangeAnalysis 只消费带有限 trip-count 的结果，并把它限制在循环体局部上下文。
// ---------------------------------------------------------------------
struct LoopInductionInfo {
    const Loop *RecLoop; // 所属循环；用于把范围限制在循环体上下文中
    int64_t initial;     // 初始值
    int64_t step;        // 每轮增量
    bool hasUpperBound;  // 是否已知上界
    int64_t upperBound;  // 上界（有效当且仅当 hasUpperBound）
    bool noWrap;         // 活动迭代中的递推已证明不回绕

    LoopInductionInfo();
};

// ---------------------------------------------------------------------
// 范围分析结果
// ---------------------------------------------------------------------
class RangeInfo {
public:
    // 整数全局范围
    std::unordered_map<Value*, IntRange> GlobalRanges;
    // 整数 per-block contextual 范围
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, IntRange>> ContextualRanges;
    // 最终 CFG 上的边事实。和 per-block 事实分开存储，因为
    // loop header 等多前驱块不能把某一条入边的条件当成
    // 整块上下文。键结构为 value -> pred -> succ -> fact。
    std::unordered_map<
        Value*, std::unordered_map<
            BasicBlock*, std::unordered_map<BasicBlock*, IntRange>>> EdgeRanges;
    // 只在精确 ctxBlock 生效的局部范围提示；不沿支配树继承
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, IntRange>> BlockLocalRanges;

    // 浮点全局范围
    std::unordered_map<Value*, FloatRange> FloatGlobalRanges;
    // 浮点 per-block contextual 范围
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, FloatRange>> FloatContextualRanges;

    // KnownBits（和 IntRange 并行的位级事实）
    std::unordered_map<Value*, KnownBits> GlobalBits;
    std::unordered_map<Value*, std::unordered_map<BasicBlock*, KnownBits>> ContextualBits;
    std::unordered_map<
        Value*, std::unordered_map<
            BasicBlock*, std::unordered_map<BasicBlock*, KnownBits>>> EdgeBits;

    // SCEV 归纳变量信息。RangeAnalysis 当前只把它作为查询期的局部上下文提示，
    // 不用它覆盖 PHI 的全局范围。
    std::unordered_map<Value*, LoopInductionInfo> LoopInductions;

    // 查询整数范围：在 ctxBlock 上下文中查询 val 的范围
    IntRange query(Value *val, BasicBlock *ctxBlock, const DominatorTree &DT) const;

    // 查询浮点范围
    FloatRange queryFloat(Value *val, BasicBlock *ctxBlock, const DominatorTree &DT) const;

    // 边敏感查询：沿 predBB → succBB 这条边进入 succBB 时 val 的 range。
    //   若 succBB 是 predBB 条件分支的唯一后继 —— 走 per-block contextual
    //     因为条件收窄就写在 succBB 上（只在 succBB 是单前驱时写入）
    //   否则退化为 query(val, predBB, DT)（pred 的上下文就是进入 succ 前的视图）
    // 是一个只读不存的边敏感查询辅助。
    IntRange queryOnEdge(Value *val, BasicBlock *predBB, BasicBlock *succBB,
                         const DominatorTree &DT) const;
    FloatRange queryFloatOnEdge(Value *val, BasicBlock *predBB, BasicBlock *succBB,
                                const DominatorTree &DT) const;

    // KnownBits 查询：和 IntRange 并列的另一条 lattice
    //   无上下文：只查 GlobalBits
    //   带上下文：沿 idom 链 intersect ContextualBits（和 IntRange 的 query 对齐）
    //   边敏感：同 queryOnEdge 的思路
    KnownBits queryBits(Value *val) const;
    KnownBits queryBits(Value *val, BasicBlock *ctxBlock,
                        const DominatorTree &DT) const;
    KnownBits queryBitsOnEdge(Value *val, BasicBlock *predBB, BasicBlock *succBB,
                              const DominatorTree &DT) const;

    // SCEV 接口：保留手动填充归纳变量信息的入口
    void setLoopInduction(Value *v, const LoopInductionInfo &info);

    bool invalidate(Function &F, const PreservationStatus &PA);
};

// ---------------------------------------------------------------------
// 范围求解器
//
// 保存前向传播、收窄阶段和边敏感事实所需的完整状态。实现统一放在
// RangeAnalysis.cpp，避免大型算法体内联到所有使用该头文件的翻译单元。
// ---------------------------------------------------------------------
class RangeSolver {
public:
    std::unordered_set<BasicBlock *> ExecutableBlocks;
    std::set<std::pair<BasicBlock *, BasicBlock *>> FeasibleEdges;
    std::vector<BasicBlock *> BBWorkList;
    std::vector<Instruction *> InstWorkList;

    std::unordered_map<Value *, IntRange> ValueRanges;
    std::unordered_map<Value *, FloatRange> FloatValueRanges;

    std::unordered_map<Value *, std::unordered_map<BasicBlock *, IntRange>>
        ContextualRanges;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, IntRange>>
        BlockLocalRanges;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, FloatRange>>
        FloatContextualRanges;
    std::unordered_map<
        Value *, std::unordered_map<
            BasicBlock *, std::unordered_map<BasicBlock *, IntRange>>>
        EdgeRanges;

    std::unordered_map<Value *, KnownBits> ValueBits;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, KnownBits>>
        ContextualBits;
    std::unordered_map<
        Value *, std::unordered_map<
            BasicBlock *, std::unordered_map<BasicBlock *, KnownBits>>>
        EdgeBits;

    static constexpr int MAX_ENQUEUE = 32;
    std::unordered_map<Value *, int> EnqueueCount;
    std::unordered_set<Value *> Widened;
    Function *CurFunc = nullptr;
    bool NarrowingMode = false;
    const std::unordered_map<Value *, LoopInductionInfo> *ScevInductions =
        nullptr;

    IntRange &getRange(Value *V);
    KnownBits getBits(Value *V);
    IntRange getRangeInBlock(Value *V, BasicBlock *BB);
    KnownBits getBitsInBlock(Value *V, BasicBlock *BB);
    void enqueueInstructionUsers(Value *V);
    bool updateBits(Value *V, const KnownBits &NewBits);
    void updateBitsContextual(Value *V, BasicBlock *Block,
                              const KnownBits &Bits);
    IntRange tightenWithBits(Value *V, const IntRange &NewRange);
    bool updateRange(Value *V, const IntRange &NewRange);
    bool updateFloatRange(Value *V, const FloatRange &NewRange);
    void updateContextual(Value *V, BasicBlock *Block,
                          const IntRange &Range);
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
    RangeInfo solve(Function &F, SCEVResult *SCEV = nullptr);
};

// ---------------------------------------------------------------------
// 分析 Pass
// ---------------------------------------------------------------------
class RangeAnalysis : public AnalysisDescriptor<RangeAnalysis> {
public:
    using Result = RangeInfo;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<RangeAnalysis>;
    static PassID Key;
};
