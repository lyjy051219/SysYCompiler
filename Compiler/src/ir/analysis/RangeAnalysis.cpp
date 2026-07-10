#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/SCEV.hpp"

#include <queue>
#include <algorithm>
#include <cstdlib>

// =====================================================================
// IntRange 算术运算
//
// 所有运算用 int64 防止 i32 溢出，结果 clamp 到 [I32_MIN, I32_MAX]。
// 对 Top（全范围）操作数做保守处理。
// =====================================================================

int64_t IntRange::clampI32(int64_t v) {
    if (v < IntRange::I32_MIN) return IntRange::I32_MIN;
    if (v > IntRange::I32_MAX) return IntRange::I32_MAX;
    return v;
}

IntRange IntRange::add(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    IntRange r(clampI32(min + rhs.min), clampI32(max + rhs.max));
    // 对齐：a + b 的 trailing zeros ≥ min(a.tz, b.tz)
    r.knownTrailingZeros = std::min(knownTrailingZeros, rhs.knownTrailingZeros);
    return r;
}

IntRange IntRange::sub(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    IntRange r(clampI32(min - rhs.max), clampI32(max - rhs.min));
    r.knownTrailingZeros = std::min(knownTrailingZeros, rhs.knownTrailingZeros);
    return r;
}

IntRange IntRange::mul(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    // 四端点取 min/max
    int64_t v1 = min * rhs.min;
    int64_t v2 = min * rhs.max;
    int64_t v3 = max * rhs.min;
    int64_t v4 = max * rhs.max;
    int64_t lo = std::min({v1, v2, v3, v4});
    int64_t hi = std::max({v1, v2, v3, v4});
    IntRange r(clampI32(lo), clampI32(hi));
    // 对齐：a * b 的 trailing zeros = a.tz + b.tz，上限 31
    int tz = knownTrailingZeros + rhs.knownTrailingZeros;
    r.knownTrailingZeros = std::min(tz, 31);
    return r;
}

IntRange IntRange::sdiv(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    // 除数包含 0 → 全范围
    if (rhs.min <= 0 && rhs.max >= 0) return IntRange();
    // 四端点取 min/max
    int64_t v1 = min / rhs.min;
    int64_t v2 = min / rhs.max;
    int64_t v3 = max / rhs.min;
    int64_t v4 = max / rhs.max;
    int64_t lo = std::min({v1, v2, v3, v4});
    int64_t hi = std::max({v1, v2, v3, v4});
    return IntRange(clampI32(lo), clampI32(hi));
}

IntRange IntRange::srem(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (rhs.min <= 0 && rhs.max >= 0) return IntRange();
    // |a % b| < |b|，符号跟随被除数
    int64_t absMax = std::max(std::abs(rhs.min), std::abs(rhs.max)) - 1;
    if (min >= 0) return IntRange(0, std::min(max, absMax));
    if (max <= 0) return IntRange(std::max(min, -absMax), 0);
    return IntRange(-absMax, absMax);
}

// =====================================================================
// 位运算（shl / lshr / ashr / and / or）
//
// 移位：只处理"移位量是常量"这个强情形；变量移位一律 Top。
//   理由：非常量移位对端点不单调（例如 x>>y，y ∈ [0, 31] 端点翻飞），
//   严格区间算得到的端点没有参考价值，退到 Top 让下游 pass 不做错误推断。
//
// shl：按 int64 计算避免 i32 溢出，结果 clamp 到 i32 范围。移位常量超出
//   [0, 31] 退 Top（SysY 里 >= 32 位移位实质 UB，但分析阶段不能假设）。
//
// ashr：算术右移，对负数端点仍然单调（-1 >> 1 = -1, -2 >> 1 = -1），所以
//   端点推导精确可行。
//
// lshr：逻辑右移。只在 x 非负时退化成 ashr 的语义；否则 Top（i32 的符号位
//   被当 MSB 填入，结果变成大正数，和 signed 区间语义脱节）。
//
// and/or：只对"非负 ∩ 非负"这一快路径做紧范围估计；混符号场合一律 Top。
//   这是轻量近似，不做完整 known-bits lattice。
// =====================================================================

IntRange IntRange::shl(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (!rhs.isSingleValue()) return IntRange();
    int64_t k = rhs.min;
    if (k < 0 || k >= 31) return IntRange();
    // IR 语义是 i32 按位宽归一化（参见 SCCP::normalizeSignedToWidth）：
    //   shl 结果在 i32 里会环绕（wraparound），1<<31 = 0x80000000 = INT32_MIN
    // 我们的 IntRange 是 "在 i32 范围内的真实值的紧包络"，不是饱和模型；
    // 所以一旦左移越过 i32 边界就必须 Top，不能截断端点 —— 截断会把 overflow
    // 后的实际值（负数）错报成 I32_MAX，导致下游 isNonNegative 判错。
    //
    // 另外：C++14+ 下负数左移是 UB，所以必须先在非负 path 上做。
    if (min < 0) return IntRange(); // 负数左移，保守 Top（也避开 UB）
    // 用无符号 64 位乘以 2^k 做等价移位，纯数学安全
    uint64_t mul = (uint64_t)1 << k;
    uint64_t u1 = (uint64_t)min * mul;
    uint64_t u2 = (uint64_t)max * mul;
    // 任一端点越过 INT32_MAX，wraparound 语义不能用端点表示 → Top
    if (u1 > (uint64_t)I32_MAX || u2 > (uint64_t)I32_MAX) return IntRange();
    IntRange r((int64_t)u1, (int64_t)u2);
    // 对齐：shl 往 LSB 填 k 个 0，所以 tz 至少 k，原来的 tz 被吃进去
    int tz = knownTrailingZeros + (int)k;
    r.knownTrailingZeros = std::min(tz, 31);
    return r;
}

IntRange IntRange::ashr(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (!rhs.isSingleValue()) return IntRange();
    int64_t k = rhs.min;
    if (k < 0 || k >= 32) return IntRange();
    // 算术右移：按符号填充；端点单调保持
    int64_t v1 = min >> k;
    int64_t v2 = max >> k;
    return IntRange(std::min(v1, v2), std::max(v1, v2));
}

IntRange IntRange::lshr(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (!rhs.isSingleValue()) return IntRange();
    int64_t k = rhs.min;
    if (k < 0 || k >= 32) return IntRange();
    // 负数走 lshr 会把符号位塞进来，signed 区间无法表示，退 Top
    if (min < 0) return IntRange();
    int64_t v1 = (uint32_t)min >> k;
    int64_t v2 = (uint32_t)max >> k;
    return IntRange(std::min<int64_t>(v1, v2), std::max<int64_t>(v1, v2));
}

IntRange IntRange::band(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    // 对齐：AND 的结果 tz ≥ max(a.tz, b.tz)（任一侧有 0 的位，结果也是 0）
    int tz = std::max(knownTrailingZeros, rhs.knownTrailingZeros);
    // 双方都非负时，结果 ∈ [0, min(lMax, rMax)]
    if (min >= 0 && rhs.min >= 0) {
        IntRange r(0, std::min(max, rhs.max));
        r.knownTrailingZeros = std::min(tz, 31);
        return r;
    }
    // 一侧是非负常量 c，and 的结果一定 ∈ [0, c]（位压在掩码内）
    if (min == max && min >= 0) {
        IntRange r(0, min);
        r.knownTrailingZeros = std::min(tz, 31);
        return r;
    }
    if (rhs.min == rhs.max && rhs.min >= 0) {
        IntRange r(0, rhs.min);
        r.knownTrailingZeros = std::min(tz, 31);
        return r;
    }
    IntRange r; // Top 范围，但 alignment 还是可以保留
    r.knownTrailingZeros = std::min(tz, 31);
    return r;
}

// =====================================================================
// KnownBits → IntRange 反推
//
// 把 KB 翻成一条信号：
//   - 冲突：不可达 → bottom
//   - 某些 bit 锁定后，可以给出 unsigned minUnsigned / maxUnsigned
//   - 再结合符号位的状态判断 signed 范围
//   - 符号位未知：signed 范围会被分成两段 [neg_min, neg_max] ∪ [pos_min, pos_max]
//     这里用"两段的并集 minmax"做保守近似（= 全 i32 范围），无害
// =====================================================================
IntRange KnownBits::toIntRange() const {
    if (isConflict()) return IntRange::bottom();
    if (isTop()) return IntRange();

    uint32_t unknown = ~(zero | one);
    uint32_t unsignedMin = one;                 // 未知位都当 0
    uint32_t unsignedMax = one | unknown;       // 未知位都当 1

    bool signKnown0 = (zero & 0x80000000u) != 0;
    bool signKnown1 = (one  & 0x80000000u) != 0;

    if (signKnown0) {
        // 非负 → unsigned 值等于 signed 值
        return IntRange((int64_t)unsignedMin, (int64_t)unsignedMax);
    }
    if (signKnown1) {
        // 负数 → 位型 cast 到 int32 得到负 signed
        int64_t lo = (int32_t)unsignedMin;
        int64_t hi = (int32_t)unsignedMax;
        // 翻成负后 lo 可能比 hi 大（极端情况），用 min/max 整理
        return IntRange(std::min(lo, hi), std::max(lo, hi));
    }
    // 符号位未知：值可正可负 → 最松 i32 全范围；但 trailing zeros 还是能保留
    IntRange r;
    r.knownTrailingZeros = getTrailingZeros();
    return r;
}

IntRange IntRange::bor(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    // 对齐：OR 的结果 tz ≥ min(a.tz, b.tz)（两侧都是 0 的位才保证是 0）
    int tz = std::min(knownTrailingZeros, rhs.knownTrailingZeros);
    // 双方都非负时，结果非负且不会小于任一输入的下界，但上界不好估紧
    // 保守给 [max(minL, minR), I32_MAX]
    if (min >= 0 && rhs.min >= 0) {
        IntRange r(std::max(min, rhs.min), I32_MAX);
        r.knownTrailingZeros = tz;
        return r;
    }
    IntRange r;
    r.knownTrailingZeros = tz;
    return r;
}

// =====================================================================
// FloatRange 算术运算
//
// 用 double 存 float 范围，NaN 输入保守返回 Top。
// 无穷大参与运算按 IEEE 754 规则传播（inf + 正数 = inf, inf - inf = NaN→Top）。
// =====================================================================

// 规范化：如果计算结果含 NaN，返回 Top
FloatRange FloatRange::sanitize(double lo, double hi) {
    if (std::isnan(lo) || std::isnan(hi)) return FloatRange();
    if (lo > hi) return FloatRange::bottom();
    return FloatRange(lo, hi);
}

FloatRange FloatRange::add(const FloatRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    return sanitize(min + rhs.min, max + rhs.max);
}

FloatRange FloatRange::sub(const FloatRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    return sanitize(min - rhs.max, max - rhs.min);
}

FloatRange FloatRange::mul(const FloatRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    double v1 = min * rhs.min;
    double v2 = min * rhs.max;
    double v3 = max * rhs.min;
    double v4 = max * rhs.max;
    if (std::isnan(v1) || std::isnan(v2) || std::isnan(v3) || std::isnan(v4))
        return FloatRange();
    double lo = std::min({v1, v2, v3, v4});
    double hi = std::max({v1, v2, v3, v4});
    return sanitize(lo, hi);
}

FloatRange FloatRange::fdiv(const FloatRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    // 除数区间跨 0 → 结果可能是 inf 或 -inf，保守返回 Top
    if (rhs.min <= 0.0 && rhs.max >= 0.0) return FloatRange();
    double v1 = min / rhs.min;
    double v2 = min / rhs.max;
    double v3 = max / rhs.min;
    double v4 = max / rhs.max;
    if (std::isnan(v1) || std::isnan(v2) || std::isnan(v3) || std::isnan(v4))
        return FloatRange();
    double lo = std::min({v1, v2, v3, v4});
    double hi = std::max({v1, v2, v3, v4});
    return sanitize(lo, hi);
}

// =====================================================================
// RangeInfo 查询接口
// =====================================================================

IntRange RangeInfo::query(Value *val, BasicBlock *ctxBlock,
                          const DominatorTree &DT) const {
    // 常量直接返回精确范围
    if (auto *CI = dynamic_cast<ConstInt *>(val))
        return IntRange(CI->getVal());

    // 全局范围
    IntRange range;
    auto gIt = GlobalRanges.find(val);
    if (gIt != GlobalRanges.end())
        range = gIt->second;

    // 沿支配树向上找 contextual 收窄，取交集
    if (ctxBlock) {
        auto cIt = ContextualRanges.find(val);
        if (cIt != ContextualRanges.end()) {
            auto &blockMap = cIt->second;
            // 从 ctxBlock 沿 idom 链向上走，最多 8 层
            BasicBlock *runner = ctxBlock;
            for (int depth = 0; depth < 8 && runner; depth++) {
                auto bIt = blockMap.find(runner);
                if (bIt != blockMap.end()) {
                    range = range.intersectWith(bIt->second);
                }
                runner = DT.getIDom(runner);
            }
        }

        auto lIt = BlockLocalRanges.find(val);
        if (lIt != BlockLocalRanges.end()) {
            auto bIt = lIt->second.find(ctxBlock);
            if (bIt != lIt->second.end())
                range = range.intersectWith(bIt->second);
        }
    }

    return range;
}

FloatRange RangeInfo::queryFloat(Value *val, BasicBlock *ctxBlock,
                                  const DominatorTree &DT) const {
    // 常量浮点直接返回精确范围
    if (auto *CF = dynamic_cast<ConstFloat *>(val))
        return FloatRange((double)CF->getVal());

    FloatRange range;
    auto gIt = FloatGlobalRanges.find(val);
    if (gIt != FloatGlobalRanges.end())
        range = gIt->second;

    if (ctxBlock) {
        auto cIt = FloatContextualRanges.find(val);
        if (cIt != FloatContextualRanges.end()) {
            auto &blockMap = cIt->second;
            BasicBlock *runner = ctxBlock;
            for (int depth = 0; depth < 8 && runner; depth++) {
                auto bIt = blockMap.find(runner);
                if (bIt != blockMap.end()) {
                    range = range.intersectWith(bIt->second);
                }
                runner = DT.getIDom(runner);
            }
        }
    }

    return range;
}

// =====================================================================
// 边敏感查询（沿 predBB → succBB 边进入 succBB 时的值）
//
// 我们在 visitBranch 里已经把条件分支的收窄写进"succ 块的 contextual"，
// 前提是 succ 是唯一前驱。所以：
//   - succ 单前驱 → 直接查 succ 的 per-block contextual，取和 global 的交集
//   - succ 多前驱 → 该边上的收窄没被存，退化为 query(val, predBB, DT)
//     这返回的是 predBB 视角下 val 的最紧 range，等价于"进入 succ 那一刻"
//     的保守上界（不考虑 predBB 内条件对 val 的进一步收窄，无害）。
// =====================================================================
IntRange RangeInfo::queryOnEdge(Value *val, BasicBlock *predBB,
                                 BasicBlock *succBB,
                                 const DominatorTree &DT) const {
    if (auto *CI = dynamic_cast<ConstInt *>(val)) return IntRange(CI->getVal());
    if (!succBB) return query(val, predBB, DT);

    bool singlePred = (succBB->getPredecessors().size() == 1);
    if (!singlePred) {
        // 多前驱：边上信息没存，退到 pred 视角
        return query(val, predBB, DT);
    }
    // 单前驱：succ 块的 contextual 已经是"条件后"的范围
    return query(val, succBB, DT);
}

// =====================================================================
// KnownBits 查询 API（和 IntRange 的 query 对齐）
// =====================================================================
KnownBits RangeInfo::queryBits(Value *val) const {
    if (auto *CI = dynamic_cast<ConstInt *>(val))
        return KnownBits::fromConst(CI->getVal());
    auto it = GlobalBits.find(val);
    if (it != GlobalBits.end()) return it->second;
    return KnownBits::top();
}

KnownBits RangeInfo::queryBits(Value *val, BasicBlock *ctxBlock,
                                const DominatorTree &DT) const {
    if (auto *CI = dynamic_cast<ConstInt *>(val))
        return KnownBits::fromConst(CI->getVal());

    KnownBits kb;
    auto gIt = GlobalBits.find(val);
    if (gIt != GlobalBits.end()) kb = gIt->second;

    if (ctxBlock) {
        auto cIt = ContextualBits.find(val);
        if (cIt != ContextualBits.end()) {
            auto &blockMap = cIt->second;
            BasicBlock *runner = ctxBlock;
            for (int depth = 0; depth < 8 && runner; depth++) {
                auto bIt = blockMap.find(runner);
                if (bIt != blockMap.end()) {
                    kb = kb.intersectWith(bIt->second);
                }
                runner = DT.getIDom(runner);
            }
        }
    }
    return kb;
}

KnownBits RangeInfo::queryBitsOnEdge(Value *val, BasicBlock *predBB,
                                      BasicBlock *succBB,
                                      const DominatorTree &DT) const {
    if (auto *CI = dynamic_cast<ConstInt *>(val))
        return KnownBits::fromConst(CI->getVal());
    if (!succBB) return queryBits(val, predBB, DT);
    bool singlePred = (succBB->getPredecessors().size() == 1);
    if (!singlePred) return queryBits(val, predBB, DT);
    return queryBits(val, succBB, DT);
}

FloatRange RangeInfo::queryFloatOnEdge(Value *val, BasicBlock *predBB,
                                        BasicBlock *succBB,
                                        const DominatorTree &DT) const {
    if (auto *CF = dynamic_cast<ConstFloat *>(val))
        return FloatRange((double)CF->getVal());
    if (!succBB) return queryFloat(val, predBB, DT);
    bool singlePred = (succBB->getPredecessors().size() == 1);
    if (!singlePred) return queryFloat(val, predBB, DT);
    return queryFloat(val, succBB, DT);
}

bool RangeInfo::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(RangeAnalysis::ID())) return false;
    return true;
}

// =====================================================================
// 跨函数 Range Summary 缓存（文件内 static）
//
// RangeAnalysis 是 Function-level pass，但 visitCall 需要查 callee 的
// 返回值 range，因此用一个 process-scope 的静态缓存做"懒更新"：
//
//   1) 分析 F 结束时，把 F 所有 RetInst 操作数 union 成 F 的 return range
//      写入 cache.RetRanges[F]
//   2) 后续分析 H（调用 F）时，visitCall 从 cache 拿 F 的 return range，
//      没 cache 就退到 Top
//
// 分析顺序敏感：如果 G 在 F 之前被分析，F 分析时 cache[G].RetRange 可用；
// 反之第一次看 cache 为空只能拿 Top。pipeline 后续调用（有 invalidate 触发
// 重算时）能渐进收敛。
//
// 递归（F 调 F）：cache 不触发递归分析，visitCall 只查表，查不到就 Top。
//
// 参数 range 传播：当前版本**不做**。若把"所有 call site 实参的 union"作为
// 参数初值 seed，会让 F 被偏紧参数分析，return range 被低估 → caller 拿到
// 错误 range → RangeAwareSimplify 做错误常量折叠 → WA（已验证）。安全实现
// 需要 pipeline 级多轮不动点迭代，复杂度不值得现在上。
// =====================================================================

InterprocRangeCache& InterprocRangeCache::get() {
    static InterprocRangeCache cache;
    return cache;
}

// =====================================================================
// 范围求解器 — 复用 SCCP 的 worklist 不动点框架
//
// 和 SCCP 相同的基础设施：
//   ExecutableBlocks, FeasibleEdges, BBWorkList, InstWorkList
// 不同的 lattice：从 {Top, Constant, Bottom} 扩展为 IntRange [min,max]
//
// 两阶段：
//   Phase 1 Forward（widening）：从 def 推 use，遇到循环靠 MAX_ENQUEUE
//     兜底 widen 到 Top 保证收敛
//   Phase 2 Narrowing（backward constraint propagation）：widening 收敛后
//     解锁 Widened，用 intersect-only 语义重跑若干轮，对二元运算做
//     backward 推导（z = x + y ⟹ x = z - y），把过宽的结果收回来
// =====================================================================

class RangeSolver {
public:
    // 和 SCCP 相同的可达性追踪
    std::unordered_set<BasicBlock *> ExecutableBlocks;
    std::set<std::pair<BasicBlock *, BasicBlock *>> FeasibleEdges;
    std::vector<BasicBlock *> BBWorkList;
    std::vector<Instruction *> InstWorkList;

    // 范围 lattice
    std::unordered_map<Value *, IntRange> ValueRanges;
    std::unordered_map<Value *, FloatRange> FloatValueRanges;

    // 上下文范围（条件分支收窄）
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, IntRange>> ContextualRanges;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, IntRange>> BlockLocalRanges;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, FloatRange>> FloatContextualRanges;

    // KnownBits：和 IntRange 并列的位级 lattice（forward/narrowing 语义
    // 和 IntRange 对齐；edge-sensitive contextual 也对齐）
    std::unordered_map<Value *, KnownBits> ValueBits;
    std::unordered_map<Value *, std::unordered_map<BasicBlock *, KnownBits>> ContextualBits;

    // 收敛控制
    static constexpr int MAX_ENQUEUE = 32;
    std::unordered_map<Value *, int> EnqueueCount;
    // 已 widen 到 Top 的值集合：一旦进入此集合，范围锁定为 Top 不再变窄
    std::unordered_set<Value *> Widened;

    // 当前函数（供 narrowing / interproc 查 param 用）
    Function *CurFunc = nullptr;

    // Narrowing 模式：forward 收敛后切换到 true，updateRange 改为 intersect-only
    // 语义，不再 widen（Widened 集合在此模式下被忽略）
    bool NarrowingMode = false;

    // --- 基础设施 ---

    IntRange &getRange(Value *V) {
        // 全局范围默认初始化为 Top（全范围）。
        // 每条指令首次 visit 时会被 visitXxx 赋予计算出的范围。
        return ValueRanges[V];
    }

    // KnownBits 取值：常量直接返回精确 KB，其他值查表
    KnownBits getBits(Value *V) {
        if (auto *CI = dynamic_cast<ConstInt *>(V))
            return KnownBits::fromConst(CI->getVal());
        return ValueBits[V]; // 缺省 Top
    }

    // 边敏感查询（solver 内部用）：取 V 在某 BB 视角下的 range/KB
    //
    // 动机：solver 主循环用 getRange(V) 只拿 global 值。如果 V 经过一条
    //   条件分支到了 BB，条件收窄已经写入 ContextualRanges[V][BB]，但
    //   global 还是宽的。PHI 合并、visitBinary 等拿 operand 时，沿着操作
    //   数"在自己 user 所在块"的视角能吃到这条收窄，收敛更快、更紧。
    //
    // 只读 BB 本层的 contextual（不沿 idom 链往上走）——solver 没有 DT 引用，
    //   而 per-block 就已经能吃到"直接前驱对 V 的收窄"这个最高频的场景。
    //   更深层的由 forward worklist 迭代自然带上来。
    IntRange getRangeInBlock(Value *V, BasicBlock *BB) {
        IntRange r = getRange(V);
        if (!BB) return r;
        auto it = ContextualRanges.find(V);
        if (it != ContextualRanges.end()) {
            auto it2 = it->second.find(BB);
            if (it2 != it->second.end()) r = r.intersectWith(it2->second);
        }
        return r;
    }

    KnownBits getBitsInBlock(Value *V, BasicBlock *BB) {
        if (auto *CI = dynamic_cast<ConstInt *>(V))
            return KnownBits::fromConst(CI->getVal());
        KnownBits kb = ValueBits[V];
        if (!BB) return kb;
        auto it = ContextualBits.find(V);
        if (it != ContextualBits.end()) {
            auto it2 = it->second.find(BB);
            if (it2 != it->second.end()) kb = kb.intersectWith(it2->second);
        }
        return kb;
    }

    // KnownBits 更新。forward 覆盖；narrowing 走 intersect。
    // 任何一方 bit 变化都把 users 放回 worklist，让 IntRange 有机会通过
    // KB.toIntRange() 被重新拉紧。
    bool updateBits(Value *V, const KnownBits &newBits) {
        auto &cur = ValueBits[V];
        KnownBits merged = NarrowingMode ? cur.intersectWith(newBits) : newBits;
        if (merged.isConflict()) return false; // 不可达路径保守忽略
        if (merged == cur) return false;
        cur = merged;
        for (auto it = V->getUseList().begin(); it != V->getUseList().end(); ++it) {
            auto *user = dynamic_cast<Instruction *>((*it)->getUser());
            if (user) InstWorkList.push_back(user);
        }
        return true;
    }

    void updateBitsContextual(Value *V, BasicBlock *block, const KnownBits &kb) {
        auto it = ContextualBits[V].find(block);
        // Top 约束等于没约束，删除旧的（和 updateContextual 对齐）
        if (kb.isTop()) {
            if (it == ContextualBits[V].end()) return;
            ContextualBits[V].erase(it);
            for (auto uit = V->getUseList().begin(); uit != V->getUseList().end(); ++uit) {
                auto *user = dynamic_cast<Instruction *>((*uit)->getUser());
                if (user) InstWorkList.push_back(user);
            }
            return;
        }
        KnownBits &cur = ContextualBits[V][block];
        if (kb == cur) return;
        cur = kb;
        for (auto uit = V->getUseList().begin(); uit != V->getUseList().end(); ++uit) {
            auto *user = dynamic_cast<Instruction *>((*uit)->getUser());
            if (user) InstWorkList.push_back(user);
        }
    }

    // 联动：从 KnownBits 反推一条 IntRange，和 newRange intersect 作为最终写入
    IntRange tightenWithBits(Value *V, const IntRange &newRange) {
        KnownBits kb = getBits(V);
        if (kb.isTop()) return newRange;
        IntRange fromKB = kb.toIntRange();
        if (fromKB.isBottom()) return IntRange::bottom();
        return newRange.intersectWith(fromKB);
    }

    // 全局范围更新：赋值语义（forward）或 intersect-only 语义（narrowing）
    //
    // Forward 模式：直接覆盖（union/算术计算出的新值）；MAX_ENQUEUE 到阈值
    //   widen 到 Top 并锁定
    // Narrowing 模式：只允许变窄（intersect with current）；widen 锁不生效
    //   这是 backward constraint propagation 的核心：让 widening 到 Top 的值
    //   在约束回推时被收回
    bool updateRange(Value *V, const IntRange &newRange) {
        // ---- Narrowing 路径 ----
        if (NarrowingMode) {
            auto &cur = ValueRanges[V];
            IntRange intersected = cur.intersectWith(newRange);
            // 不引入 bottom（空区间意味着不可达，narrowing 阶段保守忽略）
            if (intersected.isBottom()) return false;
            if (intersected == cur) return false;
            cur = intersected;
            for (auto it = V->getUseList().begin(); it != V->getUseList().end(); ++it) {
                auto *user = dynamic_cast<Instruction *>((*it)->getUser());
                if (user) InstWorkList.push_back(user);
            }
            return true;
        }

        // ---- Forward 路径 ----
        if (Widened.count(V)) return false;

        auto &cur = ValueRanges[V];
        if (newRange == cur) return false;

        int &vCount = EnqueueCount[V];
        vCount++;
        if (vCount >= MAX_ENQUEUE) {
            // 直接 widen 到 Top。曾尝试过"保符号"（非负值 widen 到 [0, INT_MAX]），
            // 但会和 narrowing 阶段的 backward 推导组合成 over-narrowing：
            // 例如循环归纳变量 i 本来有 forward [0, INT_MAX]，narrowing 通过 GEP
            // 索引反推 + SUB backward 会把 i 的 PHI 错误收缩成 [1, 16]，
            // 导致 while(i>0) 这种出口条件被 foldComparisons 恒 true 折叠
            // 保守回到 Top，让 sign 信息靠 contextual
            // 和 narrowing 自行恢复。
            cur = IntRange(); // Top
            Widened.insert(V);
        } else {
            cur = newRange;
        }

        for (auto it = V->getUseList().begin(); it != V->getUseList().end(); ++it) {
            auto *user = dynamic_cast<Instruction *>((*it)->getUser());
            if (user) {
                InstWorkList.push_back(user);
            }
        }
        return true;
    }

    // 浮点版本的 updateRange（forward/narrowing 同样语义）
    bool updateFloatRange(Value *V, const FloatRange &newRange) {
        if (NarrowingMode) {
            auto &cur = FloatValueRanges[V];
            FloatRange intersected = cur.intersectWith(newRange);
            if (intersected.isBottom()) return false;
            if (intersected == cur) return false;
            cur = intersected;
            for (auto it = V->getUseList().begin(); it != V->getUseList().end(); ++it) {
                auto *user = dynamic_cast<Instruction *>((*it)->getUser());
                if (user) InstWorkList.push_back(user);
            }
            return true;
        }

        if (Widened.count(V)) return false;
        auto &cur = FloatValueRanges[V];
        if (cur == newRange) return false;
        int &vCount = EnqueueCount[V];
        vCount++;
        if (vCount >= MAX_ENQUEUE) {
            cur = FloatRange();
            Widened.insert(V);
        } else {
            cur = newRange;
        }
        for (auto it = V->getUseList().begin(); it != V->getUseList().end(); ++it) {
            auto *user = dynamic_cast<Instruction *>((*it)->getUser());
            if (user) InstWorkList.push_back(user);
        }
        return true;
    }

    // 上下文范围更新：赋值语义。
    // 条件分支收窄产生的 per-block 约束，每次分支指令被 visit 时重新计算。
    // 如果分支操作数的范围变宽了，收窄结果也会相应变宽。
    void updateContextual(Value *V, BasicBlock *block, const IntRange &range) {
        auto it = ContextualRanges[V].find(block);

        if (range.isTop()) {
            // 约束退化成 Top：必须删除旧约束，不能残留过窄范围
            if (it == ContextualRanges[V].end()) return; // 本来就没有，不用动
            ContextualRanges[V].erase(it);
            // 通知使用者：约束变宽了
            for (auto uit = V->getUseList().begin(); uit != V->getUseList().end(); ++uit) {
                auto *user = dynamic_cast<Instruction *>((*uit)->getUser());
                if (user) InstWorkList.push_back(user);
            }
            return;
        }

        IntRange &cur = ContextualRanges[V][block];
        if (range == cur) return;
        cur = range;
        for (auto uit = V->getUseList().begin(); uit != V->getUseList().end(); ++uit) {
            auto *user = dynamic_cast<Instruction *>((*uit)->getUser());
            if (user) InstWorkList.push_back(user);
        }
    }

    // 浮点版本的 updateContextual
    void updateFloatContextual(Value *V, BasicBlock *block, const FloatRange &range) {
        auto it = FloatContextualRanges[V].find(block);

        if (range.isTop()) {
            if (it == FloatContextualRanges[V].end()) return;
            FloatContextualRanges[V].erase(it);
            for (auto uit = V->getUseList().begin(); uit != V->getUseList().end(); ++uit) {
                auto *user = dynamic_cast<Instruction *>((*uit)->getUser());
                if (user) InstWorkList.push_back(user);
            }
            return;
        }

        FloatRange &cur = FloatContextualRanges[V][block];
        if (range == cur) return;
        cur = range;
        for (auto uit = V->getUseList().begin(); uit != V->getUseList().end(); ++uit) {
            auto *user = dynamic_cast<Instruction *>((*uit)->getUser());
            if (user) InstWorkList.push_back(user);
        }
    }

    void markBlockExecutable(BasicBlock *BB) {
        if (ExecutableBlocks.insert(BB).second)
            BBWorkList.push_back(BB);
    }

    bool markEdgeFeasible(BasicBlock *From, BasicBlock *To) {
        if (!FeasibleEdges.insert({From, To}).second)
            return false;
        if (ExecutableBlocks.count(To)) {
            // 块已可达，但新边可能带来新 PHI incoming → 重新处理块头的 PHI
            for (Instruction *I : *To) {
                if (!dynamic_cast<PhiInst *>(I)) break;
                InstWorkList.push_back(I);
            }
        } else {
            markBlockExecutable(To);
        }
        return true;
    }

    // --- 指令访问者 ---

    // SCEV 接口占位：不在 visitPhi 中直接覆盖 global range。
    // SCEV 事实只在 solve 尾部作为循环体 contextual range 写入，避免把
    // header/exit 路径也误收窄。
    const std::unordered_map<Value *, LoopInductionInfo> *ScevInductions = nullptr;

    // 根据 SCEV 归纳变量信息推断活动迭代范围
    IntRange rangeFromInduction(const LoopInductionInfo &info) const {
        if (!info.hasUpperBound) return IntRange(); // Top
        if (info.step > 0)
            return IntRange(info.initial, info.upperBound);
        if (info.step < 0)
            return IntRange(info.upperBound, info.initial);
        return IntRange(info.initial);
    }

    void applySCEVContextHints(SCEVResult *SCEV) {
        if (!SCEV) return;

        const auto &inductions = SCEV->getLoopInductions();
        ScevInductions = &inductions;

        for (const auto &entry : inductions) {
            Value *V = entry.first;
            const LoopInductionInfo &info = entry.second;
            if (!V || !info.RecLoop) continue;
            if (!V->getIRType() || V->getIRType()->getEnum() != IR_INT) continue;

            IntRange activeRange = rangeFromInduction(info);
            if (activeRange.isTop() || activeRange.isBottom()) continue;
            if (activeRange.min < IntRange::I32_MIN ||
                activeRange.max > IntRange::I32_MAX)
                continue;

            BasicBlock *header = info.RecLoop->getHeader();
            for (BasicBlock *BB : info.RecLoop->getBlocksIncludingSubLoops()) {
                // Header PHI can also take the first value that leaves the loop
                // through the header condition, so the active-iteration range is
                // intentionally limited to safe, non-exiting body blocks.
                if (!BB || BB == header) continue;
                if (info.RecLoop->isExitingBlock(BB)) continue;

                IntRange cur = IntRange();
                auto it = BlockLocalRanges.find(V);
                if (it != BlockLocalRanges.end()) {
                    auto bit = it->second.find(BB);
                    if (bit != it->second.end())
                        cur = bit->second;
                }
                IntRange narrowed = cur.intersectWith(activeRange);
                if (!narrowed.isBottom())
                    BlockLocalRanges[V][BB] = narrowed;
            }
        }
    }

    void visitPhi(PhiInst *Phi) {
        // 浮点 PHI
        auto phiTy = Phi->getIRType();
        if (phiTy && phiTy->getEnum() == IR_FLOAT) {
            FloatRange fresult = FloatRange::bottom();
            for (unsigned i = 0; i < Phi->getNumIncomingValues(); i++) {
                BasicBlock *pred = Phi->getIncomingBlock(i);
                BasicBlock *phiBB = Phi->getParent();
                if (!FeasibleEdges.count({pred, phiBB})) continue;

                Value *inVal = Phi->getIncomingValue(i);
                FloatRange inRange;
                if (auto *CF = dynamic_cast<ConstFloat *>(inVal))
                    inRange = FloatRange((double)CF->getVal());
                else
                    inRange = FloatValueRanges[inVal];
                fresult = fresult.unionWith(inRange);
            }
            // 浮点 PHI 范围更新（统一走 updateFloatRange）
            updateFloatRange(Phi, fresult);
            return;
        }

        // SCEV 优先：如果外部（未来的 SCEV pass）已经为这个 PHI 推断出
        // 循环归纳变量范围，优先使用，不走 union-of-incomings
        if (ScevInductions) {
            auto it = ScevInductions->find(Phi);
            if (it != ScevInductions->end()) {
                updateRange(Phi, rangeFromInduction(it->second));
                return;
            }
        }

        // 整数 PHI：edge-sensitive union
        // 每个 incoming 取 "inVal 在 pred 块视角" 的 range/KB ——
        // pred 块上如果有条件分支把 inVal 收窄过（pred 的 contextual），
        // 这里就能吃到，而不是只拿 inVal 的 global range。
        IntRange result = IntRange::bottom();
        KnownBits kbResult;
        bool firstKB = true;
        for (unsigned i = 0; i < Phi->getNumIncomingValues(); i++) {
            BasicBlock *pred = Phi->getIncomingBlock(i);
            BasicBlock *phiBB = Phi->getParent();
            if (!FeasibleEdges.count({pred, phiBB})) continue;

            Value *inVal = Phi->getIncomingValue(i);
            IntRange inRange;
            KnownBits inKB;
            if (auto *CI = dynamic_cast<ConstInt *>(inVal)) {
                inRange = IntRange(CI->getVal());
                inKB = KnownBits::fromConst(CI->getVal());
            } else {
                // 暂时退回 global（regression bisect）
                inRange = getRange(inVal);
                inKB = getBits(inVal);
            }
            result = result.unionWith(inRange);
            kbResult = firstKB ? inKB : kbResult.unionWith(inKB);
            firstKB = false;
        }
        updateBits(Phi, kbResult);
        result = tightenWithBits(Phi, result);
        updateRange(Phi, result);
    }

    void visitBinary(BinaryInst *BI) {
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        if (!BI->getOperands(lhs, rhs)) return;

        // 浮点二元运算
        auto resultTy = BI->getIRType();
        if (resultTy && resultTy->getEnum() == IR_FLOAT) {
            FloatRange lf = dynamic_cast<ConstFloat *>(lhs) ?
                FloatRange((double)dynamic_cast<ConstFloat *>(lhs)->getVal()) :
                FloatValueRanges[lhs];
            FloatRange rf = dynamic_cast<ConstFloat *>(rhs) ?
                FloatRange((double)dynamic_cast<ConstFloat *>(rhs)->getVal()) :
                FloatValueRanges[rhs];
            FloatRange fresult;
            switch (BI->getOp()) {
                case BinaryInst::ADD: fresult = lf.add(rf); break;
                case BinaryInst::SUB: fresult = lf.sub(rf); break;
                case BinaryInst::MUL: fresult = lf.mul(rf); break;
                case BinaryInst::DIV: fresult = lf.fdiv(rf); break;
                default: fresult = FloatRange(); break;
            }
            // 浮点范围更新（带 widening / narrowing，由 updateFloatRange 统一处理）
            updateFloatRange(BI, fresult);

            // Backward constraint propagation：浮点 ADD/SUB 同构成立
            // narrowBinaryBackward 内部根据类型分派，这里把整数版本的 dummy 参数 传过去就行
            if (NarrowingMode) {
                narrowBinaryBackward(BI, lhs, rhs, IntRange(), IntRange());
            }
            return;
        }

        // 整数二元运算
        //   操作数走 global range —— 不能用 BB-sensitive contextual：narrowBinaryBackward
        //   会把结果 range.sub(otherOperandRange) 写回操作数的 GLOBAL range，如果
        //   任一侧是 contextual 收窄的结果，backward 推导会把块局部事实传染给全局，
        //   其它不受该 contextual 影响的块立刻 over-narrow → WA。
        //   (PHI 的 incoming 是纯 forward 汇合，没有 backward 写回操作数，安全；
        //    可以用 getRangeInBlock。visitBinary 不行。)
        IntRange lRange = dynamic_cast<ConstInt *>(lhs) ?
            IntRange(dynamic_cast<ConstInt *>(lhs)->getVal()) : getRange(lhs);
        IntRange rRange = dynamic_cast<ConstInt *>(rhs) ?
            IntRange(dynamic_cast<ConstInt *>(rhs)->getVal()) : getRange(rhs);

        IntRange result;
        switch (BI->getOp()) {
            case BinaryInst::ADD:  result = lRange.add(rRange);  break;
            case BinaryInst::SUB:  result = lRange.sub(rRange);  break;
            case BinaryInst::MUL:  result = lRange.mul(rRange);  break;
            case BinaryInst::DIV:  result = lRange.sdiv(rRange); break;
            case BinaryInst::MOD:  result = lRange.srem(rRange); break;
            // 位运算/移位：走扩展的 IntRange 方法
            case BinaryInst::SHL:  result = lRange.shl(rRange);  break;
            case BinaryInst::ASHR: result = lRange.ashr(rRange); break;
            case BinaryInst::LSHR: result = lRange.lshr(rRange); break;
            case BinaryInst::AND:  result = lRange.band(rRange); break;
            case BinaryInst::OR:   result = lRange.bor(rRange);  break;
            // XOR / UDIV / UREM 等当前 SysY 中端不会直接产出，保守 Top
            default: result = IntRange(); break;
        }

        // —— KnownBits 并行推导 —— 操作数同样走 global（原因同上）
        // 移位常量必须是 const 才推位级事实；否则 KB = Top
        KnownBits lKB = getBits(lhs);
        KnownBits rKB = getBits(rhs);
        KnownBits resKB = KnownBits::top();
        auto rhsAsConstK = [&]() -> int {
            if (auto *c = dynamic_cast<ConstInt *>(rhs)) return c->getVal();
            if (rRange.isSingleValue()) return (int)rRange.min;
            return -1;
        };
        switch (BI->getOp()) {
            case BinaryInst::ADD: resKB = lKB.add(rKB); break;
            case BinaryInst::SUB: resKB = lKB.sub(rKB); break;
            case BinaryInst::MUL: resKB = lKB.mul(rKB); break;
            case BinaryInst::AND: resKB = lKB.band(rKB); break;
            case BinaryInst::OR:  resKB = lKB.bor(rKB); break;
            case BinaryInst::XOR: resKB = lKB.bxor(rKB); break;
            case BinaryInst::SHL: {
                int k = rhsAsConstK();
                if (k >= 0 && k < 32) resKB = lKB.shl(k);
                break;
            }
            case BinaryInst::LSHR: {
                int k = rhsAsConstK();
                if (k >= 0 && k < 32) resKB = lKB.lshr(k);
                break;
            }
            case BinaryInst::ASHR: {
                int k = rhsAsConstK();
                if (k >= 0 && k < 32) resKB = lKB.ashr(k);
                break;
            }
            default: break;
        }
        updateBits(BI, resKB);
        // 联动：KB 能推出的 range 和 IntRange 算出的 range 取交
        result = tightenWithBits(BI, result);
        updateRange(BI, result);

        // Backward constraint propagation：只在 narrowing 阶段做
        // 对 ADD/SUB 做 z = x op y ⟹ 反推 x / y 的范围
        // 例：z ∈ [0, 10], y 是常量 3 ⟹ x ∈ [-3, 7]
        if (NarrowingMode) {
            narrowBinaryBackward(BI, lhs, rhs, lRange, rRange);
        }
    }

    // 反推二元运算操作数的范围（仅 narrowing 阶段使用）
    //
    // 完整反推（ADD/SUB）：
    //   z = x + y ⟹ x = z - y,  y = z - x
    //   z = x - y ⟹ x = z + y,  y = x - z
    // 弱反推（MUL/DIV/MOD）：端点反推非单调，只推符号
    //   z = x * y, z 非负, y > 0  ⟹  x 非负
    //   z = x / y, z 非负, y > 0  ⟹  x 非负
    //   z = x % y (srem), z 非负  ⟹  x 非负 （srem 结果符号跟被除数）
    // 浮点：FADD/FSUB 和整数同构成立（假定非 NaN；NaN 已被吸收到 Top）
    void narrowBinaryBackward(BinaryInst *BI, Value *lhs, Value *rhs,
                              const IntRange &lRange, const IntRange &rRange) {
        auto ty = BI->getIRType();
        if (!ty) return;

        // 整数路径
        if (ty->getEnum() == IR_INT) {
            IntRange zR = getRange(BI);
            switch (BI->getOp()) {
                case BinaryInst::ADD: {
                    if (!dynamic_cast<ConstInt *>(lhs))
                        updateRange(lhs, zR.sub(rRange));
                    if (!dynamic_cast<ConstInt *>(rhs))
                        updateRange(rhs, zR.sub(lRange));
                    break;
                }
                case BinaryInst::SUB: {
                    if (!dynamic_cast<ConstInt *>(lhs))
                        updateRange(lhs, zR.add(rRange));
                    if (!dynamic_cast<ConstInt *>(rhs))
                        updateRange(rhs, lRange.sub(zR));
                    break;
                }
                // 弱 backward：z = x * y 且 z 非负、y 非负 ⟹ x 非负
                // 对 SDIV / SREM 同理（SREM 只在除数非零时）
                // 只推符号，不推具体边界（MUL/DIV 的端点反推非单调）
                case BinaryInst::MUL: {
                    if (zR.isNonNegative()) {
                        if (rRange.isPositive() && !dynamic_cast<ConstInt *>(lhs))
                            updateRange(lhs, IntRange::nonNegative());
                        if (lRange.isPositive() && !dynamic_cast<ConstInt *>(rhs))
                            updateRange(rhs, IntRange::nonNegative());
                    }
                    break;
                }
                case BinaryInst::DIV: {
                    if (zR.isNonNegative() && rRange.isPositive() &&
                        !dynamic_cast<ConstInt *>(lhs)) {
                        updateRange(lhs, IntRange::nonNegative());
                    }
                    break;
                }
                case BinaryInst::MOD: {
                    // srem 结果符号跟被除数，z 非负 ⟹ 被除数非负（仅当除数非零时）
                    if (zR.isNonNegative() && !rRange.isTop() &&
                        !dynamic_cast<ConstInt *>(lhs)) {
                        updateRange(lhs, IntRange::nonNegative());
                    }
                    break;
                }
                default: break;
            }
            return;
        }

        // 浮点路径
        if (ty->getEnum() == IR_FLOAT) {
            FloatRange zR = FloatValueRanges[BI];
            FloatRange lF = dynamic_cast<ConstFloat *>(lhs) ?
                FloatRange((double)dynamic_cast<ConstFloat *>(lhs)->getVal()) :
                FloatValueRanges[lhs];
            FloatRange rF = dynamic_cast<ConstFloat *>(rhs) ?
                FloatRange((double)dynamic_cast<ConstFloat *>(rhs)->getVal()) :
                FloatValueRanges[rhs];
            // lRange / rRange 是传入的整数参数，浮点不用；用 lF/rF 替代
            (void)lRange; (void)rRange;

            switch (BI->getOp()) {
                case BinaryInst::ADD: {
                    if (!dynamic_cast<ConstFloat *>(lhs))
                        updateFloatRange(lhs, zR.sub(rF));
                    if (!dynamic_cast<ConstFloat *>(rhs))
                        updateFloatRange(rhs, zR.sub(lF));
                    break;
                }
                case BinaryInst::SUB: {
                    if (!dynamic_cast<ConstFloat *>(lhs))
                        updateFloatRange(lhs, zR.add(rF));
                    if (!dynamic_cast<ConstFloat *>(rhs))
                        updateFloatRange(rhs, lF.sub(zR));
                    break;
                }
                default: break;
            }
            return;
        }
    }

    void visitBranch(BrInst *Br) {
        // 无条件分支
        if (!Br->isConditional()) {
            auto *dest = Br->getTrueSuccessor();
            BasicBlock *fromBB = Br->getParent();
            if (dest) markEdgeFeasible(fromBB, dest);
            return;
        }

        // 条件分支
        Value *cond = Br->getCondition();
        auto *trueBB = Br->getTrueSuccessor();
        auto *falseBB = Br->getFalseSuccessor();
        BasicBlock *fromBB = Br->getParent();

        if (!trueBB || !falseBB) return;

        // 条件分支收窄：条件是 icmp 比较指令时，在 true/false 分支上收窄操作数范围。
        // 安全性约束：只在目标块的唯一前驱就是当前块时才收窄。
        // 如果目标块有多个前驱（典型如 loop header），从其他前驱进入时
        // 这个条件不成立，收窄会变成错误的 over-constrain。
        bool trueSinglePred = (trueBB->getPredecessors().size() == 1);
        bool falseSinglePred = (falseBB->getPredecessors().size() == 1);

        if ((trueSinglePred || falseSinglePred) &&
            (dynamic_cast<BinaryInst *>(cond) != nullptr)) {
            auto *cmpInst = dynamic_cast<BinaryInst *>(cond);
            Value *lhs = nullptr;
            Value *rhs = nullptr;
            if (cmpInst->getOperands(lhs, rhs)) {
                auto op = cmpInst->getOp();

                // 浮点分支收窄：操作数类型 IR_FLOAT 时走浮点路径
                // FloatRange 的 Top 保守含 NaN；非 Top 的操作数我们假定不含 NaN
                // （NaN 已被吸收到 Top），此时 fcmp uXX 等价 olt/ole/... 按端点收窄
                // 和整数路径语义一致，只是不用 ±1（浮点没有"紧邻前一个/后一个"的概念）
                auto lhsTy = lhs->getIRType();
                auto rhsTy = rhs->getIRType();
                if (lhsTy && rhsTy &&
                    lhsTy->getEnum() == IR_FLOAT && rhsTy->getEnum() == IR_FLOAT) {
                    FloatRange lF = dynamic_cast<ConstFloat *>(lhs) ?
                        FloatRange((double)dynamic_cast<ConstFloat *>(lhs)->getVal()) :
                        FloatValueRanges[lhs];
                    FloatRange rF = dynamic_cast<ConstFloat *>(rhs) ?
                        FloatRange((double)dynamic_cast<ConstFloat *>(rhs)->getVal()) :
                        FloatValueRanges[rhs];

                    double NINF = FloatRange::F_MIN_INF();
                    double PINF = FloatRange::F_MAX_INF();

                    // flt: lhs < rhs
                    if (op == BinaryInst::L) {
                        if (trueSinglePred) {
                            updateFloatContextual(lhs, trueBB, FloatRange(NINF, rF.max));
                            updateFloatContextual(rhs, trueBB, FloatRange(lF.min, PINF));
                        }
                        if (falseSinglePred) {
                            updateFloatContextual(lhs, falseBB, FloatRange(rF.min, PINF));
                            updateFloatContextual(rhs, falseBB, FloatRange(NINF, lF.max));
                        }
                    }
                    // fle: lhs <= rhs
                    else if (op == BinaryInst::LE) {
                        if (trueSinglePred) {
                            updateFloatContextual(lhs, trueBB, FloatRange(NINF, rF.max));
                            updateFloatContextual(rhs, trueBB, FloatRange(lF.min, PINF));
                        }
                        if (falseSinglePred) {
                            updateFloatContextual(lhs, falseBB, FloatRange(rF.min, PINF));
                            updateFloatContextual(rhs, falseBB, FloatRange(NINF, lF.max));
                        }
                    }
                    // fgt: lhs > rhs
                    else if (op == BinaryInst::G) {
                        if (trueSinglePred) {
                            updateFloatContextual(lhs, trueBB, FloatRange(rF.min, PINF));
                            updateFloatContextual(rhs, trueBB, FloatRange(NINF, lF.max));
                        }
                        if (falseSinglePred) {
                            updateFloatContextual(lhs, falseBB, FloatRange(NINF, rF.max));
                            updateFloatContextual(rhs, falseBB, FloatRange(lF.min, PINF));
                        }
                    }
                    // fge: lhs >= rhs
                    else if (op == BinaryInst::GE) {
                        if (trueSinglePred) {
                            updateFloatContextual(lhs, trueBB, FloatRange(rF.min, PINF));
                            updateFloatContextual(rhs, trueBB, FloatRange(NINF, lF.max));
                        }
                        if (falseSinglePred) {
                            updateFloatContextual(lhs, falseBB, FloatRange(NINF, rF.max));
                            updateFloatContextual(rhs, falseBB, FloatRange(lF.min, PINF));
                        }
                    }
                    // feq: lhs == rhs
                    else if (op == BinaryInst::E) {
                        if (trueSinglePred) {
                            updateFloatContextual(lhs, trueBB, rF);
                            updateFloatContextual(rhs, trueBB, lF);
                        }
                    }

                    markEdgeFeasible(fromBB, trueBB);
                    markEdgeFeasible(fromBB, falseBB);
                    return;
                }

                // 整数分支收窄
                IntRange lRange = dynamic_cast<ConstInt *>(lhs) ?
                    IntRange(dynamic_cast<ConstInt *>(lhs)->getVal()) : getRange(lhs);
                IntRange rRange = dynamic_cast<ConstInt *>(rhs) ?
                    IntRange(dynamic_cast<ConstInt *>(rhs)->getVal()) : getRange(rhs);

                // slt: lhs < rhs
                if (op == BinaryInst::L) {
                    if (trueSinglePred) {
                        updateContextual(lhs, trueBB,
                            IntRange(IntRange::I32_MIN, rRange.max - 1));
                        updateContextual(rhs, trueBB,
                            IntRange(lRange.min + 1, IntRange::I32_MAX));
                    }
                    if (falseSinglePred) {
                        updateContextual(lhs, falseBB,
                            IntRange(rRange.min, IntRange::I32_MAX));
                        updateContextual(rhs, falseBB,
                            IntRange(IntRange::I32_MIN, lRange.max));
                    }
                }
                // sle: lhs <= rhs
                else if (op == BinaryInst::LE) {
                    if (trueSinglePred) {
                        updateContextual(lhs, trueBB,
                            IntRange(IntRange::I32_MIN, rRange.max));
                        updateContextual(rhs, trueBB,
                            IntRange(lRange.min, IntRange::I32_MAX));
                    }
                    if (falseSinglePred) {
                        updateContextual(lhs, falseBB,
                            IntRange(rRange.min + 1, IntRange::I32_MAX));
                        updateContextual(rhs, falseBB,
                            IntRange(IntRange::I32_MIN, lRange.max - 1));
                    }
                }
                // sgt: lhs > rhs
                else if (op == BinaryInst::G) {
                    if (trueSinglePred) {
                        updateContextual(lhs, trueBB,
                            IntRange(rRange.min + 1, IntRange::I32_MAX));
                        updateContextual(rhs, trueBB,
                            IntRange(IntRange::I32_MIN, lRange.max - 1));
                    }
                    if (falseSinglePred) {
                        updateContextual(lhs, falseBB,
                            IntRange(IntRange::I32_MIN, rRange.max));
                        updateContextual(rhs, falseBB,
                            IntRange(lRange.min, IntRange::I32_MAX));
                    }
                }
                // sge: lhs >= rhs
                else if (op == BinaryInst::GE) {
                    if (trueSinglePred) {
                        updateContextual(lhs, trueBB,
                            IntRange(rRange.min, IntRange::I32_MAX));
                        updateContextual(rhs, trueBB,
                            IntRange(IntRange::I32_MIN, lRange.max));
                    }
                    if (falseSinglePred) {
                        updateContextual(lhs, falseBB,
                            IntRange(IntRange::I32_MIN, rRange.max - 1));
                        updateContextual(rhs, falseBB,
                            IntRange(lRange.min + 1, IntRange::I32_MAX));
                    }
                }
                // eq: lhs == rhs
                else if (op == BinaryInst::E) {
                    if (trueSinglePred) {
                        updateContextual(lhs, trueBB, rRange);
                        updateContextual(rhs, trueBB, lRange);
                    }
                }

                // —— 边敏感 KnownBits：eq/ne 下窄化 ——
                //   x == c  (true 分支):  lhs 的 KB = KB(c)
                //   x == c  (false 分支): 不保留 KB（只排除一个具体值，KB 无损表达）
                //   x != c  (false 分支): 和 "x == c 的 true 分支" 等价
                // 常量/非常量两侧对称处理
                auto applyEqKB = [&](Value *a, Value *b, BasicBlock *succ, bool predOnSucc){
                    if (!predOnSucc) return;
                    // a 要从 b 身上继承 KB
                    KnownBits bKB = getBits(b);
                    if (bKB.isTop()) return;
                    updateBitsContextual(a, succ, bKB);
                };
                if (op == BinaryInst::E) {
                    applyEqKB(lhs, rhs, trueBB, trueSinglePred);
                    applyEqKB(rhs, lhs, trueBB, trueSinglePred);
                } else if (op == BinaryInst::NE) {
                    applyEqKB(lhs, rhs, falseBB, falseSinglePred);
                    applyEqKB(rhs, lhs, falseBB, falseSinglePred);
                }
                // slt / sge 时：lhs < 0 的 true 分支 → 符号位 = 1
                //              lhs < 0 的 false 分支 → 符号位 = 0
                // 同理 lhs <= -1 等。只对 rhs 是 0 常量的这一高频形态生效
                auto applySignBitKB = [&](Value *x, BasicBlock *succ, bool knownNeg,
                                          bool predOnSucc) {
                    if (!predOnSucc) return;
                    KnownBits kb;
                    if (knownNeg) kb.one  |= 0x80000000u;
                    else          kb.zero |= 0x80000000u;
                    updateBitsContextual(x, succ, kb);
                };
                if (auto *rC = dynamic_cast<ConstInt *>(rhs)) {
                    int rv = rC->getVal();
                    if (rv == 0) {
                        // x < 0 / x <= -1: rv=0 只对 < 成立；x >= 0 / x > -1 对 >= 成立
                        if (op == BinaryInst::L) {
                            applySignBitKB(lhs, trueBB,  true,  trueSinglePred);
                            applySignBitKB(lhs, falseBB, false, falseSinglePred);
                        } else if (op == BinaryInst::GE) {
                            applySignBitKB(lhs, trueBB,  false, trueSinglePred);
                            applySignBitKB(lhs, falseBB, true,  falseSinglePred);
                        }
                    }
                }
            }
        }

        markEdgeFeasible(fromBB, trueBB);
        markEdgeFeasible(fromBB, falseBB);
    }

    void visitCall(CallInst *CI) {
        Value *callee = CI->getCallee();
        if (!callee) return;

        // 内置函数范围推断
        if (auto *BIF = dynamic_cast<BuildInFunction *>(callee)) {
            std::string name = BIF->getIdent();
            if (name == "getch") {
                // getch 返回 ASCII 字符，范围 [0, 127]
                updateRange(CI, IntRange(0, 127));
                return;
            }
            if (name == "getint") {
                // getint 可以返回任何 i32
                updateRange(CI, IntRange());
                return;
            }
            // 其他内置函数（putint 等）返回值当 Top
            updateRange(CI, IntRange());
            return;
        }

        // 用户定义函数：查跨函数 return range summary
        // 注意：不做参数 range 传播（参数被 seed 成"所有 call site 实参 union"
        // 后，函数会被用偏紧的参数分析，返回值可能被低估；修这个需要 pipeline
        // 级不动点迭代，超出单次 analysis 的范围）
        if (auto *F = dynamic_cast<Function *>(callee)) {
            InterprocRangeCache &cache = InterprocRangeCache::get();
            auto callTy = CI->getIRType();
            if (callTy && callTy->getEnum() == IR_INT) {
                auto it = cache.RetRanges.find(F);
                if (it != cache.RetRanges.end()) {
                    updateRange(CI, it->second);
                    return;
                }
                updateRange(CI, IntRange());
                return;
            }
            if (callTy && callTy->getEnum() == IR_FLOAT) {
                auto it = cache.FloatRetRanges.find(F);
                if (it != cache.FloatRetRanges.end()) {
                    updateFloatRange(CI, it->second);
                    return;
                }
                updateFloatRange(CI, FloatRange());
                return;
            }
            return;
        }

        // 无法识别的 callee → Top
        updateRange(CI, IntRange());
    }

    void visitGEP(GetElementPtrInst *GEP) {
        if (!GEP || GEP->getNumIndices() == 0) return;
        Value *basePtr = GEP->getBasePointer();
        if (!basePtr || !basePtr->getIRType()) return;
        BasicBlock *BB = GEP->getParent();
        if (!BB) return;

        auto currentType = basePtr->getIRType();

        // 按 updateType 的逻辑走类型链；对每个 ARRAYType 位置的索引加约束
        for (size_t i = 0; i < GEP->getNumIndices(); i++) {
            if (auto arr = std::dynamic_pointer_cast<ARRAYType>(currentType)) {
                int count = arr->getElementCount();
                if (count > 0) {
                    Value *idx = GEP->getIndex(i);
                    if (idx && idx->getIRType() &&
                        idx->getIRType()->getEnum() == IR_INT &&
                        !dynamic_cast<ConstInt *>(idx)) {
                        // 约束 idx ∈ [0, count - 1]
                        IntRange bound(0, count - 1);
                        IntRange cur = ContextualRanges[idx].count(BB) ?
                            ContextualRanges[idx][BB] : IntRange();
                        IntRange narrowed = cur.intersectWith(bound);
                        updateContextual(idx, BB, narrowed);
                    }
                }
                currentType = arr->getElementType();
            } else if (auto ptr = std::dynamic_pointer_cast<POINTERType>(currentType)) {
                // 指针算术，没有天然的数组边界 → 跳过
                currentType = ptr->getPointerType();
            } else {
                break;
            }
        }
    }

    // =====================================================================
    // Select：%r = select %c, %a, %b
    //
    // 语义上是条件三元，整数/浮点均取 a、b 的 union。
    // 不尝试靠 %c 的条件信息收窄（condition 是 boolean，其"真分支蕴含什么"
    // 已经在 visitBranch 做过；select 不在 CFG 上，做不到支配推理）。
    // =====================================================================
    void visitSelect(SelectInst *SI) {
        Value *tv = SI->getTrueValue();
        Value *fv = SI->getFalseValue();
        if (!tv || !fv) return;
        auto ty = SI->getIRType();
        if (!ty) return;

        if (ty->getEnum() == IR_INT) {
            IntRange tr = dynamic_cast<ConstInt *>(tv) ?
                IntRange(dynamic_cast<ConstInt *>(tv)->getVal()) : getRange(tv);
            IntRange fr = dynamic_cast<ConstInt *>(fv) ?
                IntRange(dynamic_cast<ConstInt *>(fv)->getVal()) : getRange(fv);
            updateRange(SI, tr.unionWith(fr));
        } else if (ty->getEnum() == IR_FLOAT) {
            FloatRange tr = dynamic_cast<ConstFloat *>(tv) ?
                FloatRange((double)dynamic_cast<ConstFloat *>(tv)->getVal()) :
                FloatValueRanges[tv];
            FloatRange fr = dynamic_cast<ConstFloat *>(fv) ?
                FloatRange((double)dynamic_cast<ConstFloat *>(fv)->getVal()) :
                FloatValueRanges[fv];
            updateFloatRange(SI, tr.unionWith(fr));
        }
    }

    // =====================================================================
    // Cast 指令的范围推导
    //
    // zext  i1 → i32：结果 ∈ [0, 1]（我们的 ConstBool 也遵循这个语义）
    // sext  i1 → i32：结果 ∈ {-1, 0}，给 [-1, 0]（i1 的 1 符号扩展到 -1）
    // trunc i32 → i1：结果 ∈ [0, 1]
    // sitofp i32 → f32：把整数端点直接转成 float 端点；溢出风险对 i32 下忽略
    // fptosi f32 → i32：浮点区间转 i32，±inf 或超范围 → Top
    // =====================================================================
    void visitCast(Instruction *I) {
        Value *src = I->getUnaryOperand();
        if (!src || !src->getIRType() || !I->getIRType()) return;
        auto srcTy = src->getIRType()->getEnum();
        auto dstTy = I->getIRType()->getEnum();

        if (dynamic_cast<ZextInst *>(I)) {
            // 源是 i1：[0, 1]；若源是 i32 当成身份用（SysY 一般不会走这条）
            if (srcTy == IR_INT) {
                updateRange(I, IntRange(0, 1));
                // KB：高 31 位都是 0，最低位未知（取决于 bool）
                updateBits(I, KnownBits(0xFFFFFFFEu, 0));
            }
            return;
        }
        if (dynamic_cast<SextInst *>(I)) {
            // i1 → i32：1 会被符号扩展到 -1，0 保持 0
            if (srcTy == IR_INT) {
                updateRange(I, IntRange(-1, 0));
                // KB：值只能是 0x00000000 或 0xFFFFFFFF；位型未知都归 Top
                // （因为高位取决于最低位，没法用单一 KB 刻画）
            }
            return;
        }
        if (dynamic_cast<TruncInst *>(I)) {
            // i32 → i1：结果只看最低位，直接归为 [0, 1]
            if (dstTy == IR_INT) {
                updateRange(I, IntRange(0, 1));
                // KB：高 31 位都是 0；bit0 继承源的 bit0
                KnownBits srcKB = getBits(src);
                uint32_t z = 0xFFFFFFFEu;
                uint32_t o = 0;
                if (srcKB.zero & 1u) z |= 1u;
                if (srcKB.one  & 1u) o |= 1u;
                updateBits(I, KnownBits(z, o));
            }
            return;
        }
        if (dynamic_cast<SITOFPInst *>(I)) {
            // int → float：把端点直接转成 double（无精度损失，i32 ⊂ double）
            IntRange r = dynamic_cast<ConstInt *>(src) ?
                IntRange(dynamic_cast<ConstInt *>(src)->getVal()) : getRange(src);
            if (r.isBottom()) { updateFloatRange(I, FloatRange::bottom()); return; }
            updateFloatRange(I, FloatRange((double)r.min, (double)r.max));
            return;
        }
        if (dynamic_cast<FPTOSIInst *>(I)) {
            // float → int：端点向零截断；±inf / NaN 保守退 Top。
            // 越界行为：一旦端点超过 i32 可表示范围，fptosi 的结果按 IR 语义
            // 未定义（poison / undef）。这里不能 clamp 到 I32_MIN/I32_MAX，
            // 因为 RangeAwareSimplify 会把单值区间直接折成 ConstInt —— 把
            // 1e20f 折成 INT32_MAX 就是改程序行为。安全做法：任一端点超
            // 出范围 → Top。
            FloatRange r = dynamic_cast<ConstFloat *>(src) ?
                FloatRange((double)dynamic_cast<ConstFloat *>(src)->getVal()) :
                FloatValueRanges[src];
            if (r.isBottom() || r.isTop()) { updateRange(I, IntRange()); return; }
            double lo = std::trunc(r.min);
            double hi = std::trunc(r.max);
            if (!std::isfinite(lo) || !std::isfinite(hi)) { updateRange(I, IntRange()); return; }
            // 越界直接 Top（而不是夹到边界）
            if (lo < (double)IntRange::I32_MIN || hi > (double)IntRange::I32_MAX) {
                updateRange(I, IntRange());
                return;
            }
            int64_t iLo = (int64_t)lo;
            int64_t iHi = (int64_t)hi;
            if (iLo > iHi) { updateRange(I, IntRange()); return; }
            updateRange(I, IntRange(iLo, iHi));
            return;
        }
        // BitCast 指针层面，不产生整数/浮点范围
    }

    void visitInstruction(Instruction *I) {
        if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
            visitPhi(Phi);
        } else if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
            visitBinary(BI);
        } else if (auto *Br = dynamic_cast<BrInst *>(I)) {
            visitBranch(Br);
        } else if (auto *CI = dynamic_cast<CallInst *>(I)) {
            visitCall(CI);
        } else if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I)) {
            visitGEP(GEP);
        } else if (auto *SI = dynamic_cast<SelectInst *>(I)) {
            visitSelect(SI);
        } else if (dynamic_cast<ZextInst *>(I) || dynamic_cast<SextInst *>(I) ||
                   dynamic_cast<TruncInst *>(I) || dynamic_cast<SITOFPInst *>(I) ||
                   dynamic_cast<FPTOSIInst *>(I)) {
            visitCast(I);
        } else if (dynamic_cast<LoadInst *>(I)) {
            // load 的结果范围未知
            updateRange(I, IntRange());
        }
        // store, ret 等不产生 i32 值的指令不处理
    }

    // 扫描本函数所有 RetInst，把返回值 range 的 union 写入跨函数 cache
    // 下次分析其他调用本函数的函数时能直接查到
    void publishReturnRange(Function &F) {
        InterprocRangeCache &cache = InterprocRangeCache::get();
        IntRange retR = IntRange::bottom();
        FloatRange fretR = FloatRange::bottom();
        bool hasInt = false, hasFloat = false;

        for (BasicBlock *BB : F) {
            if (!ExecutableBlocks.count(BB)) continue;
            for (Instruction *I : *BB) {
                auto *Ret = dynamic_cast<RetInst *>(I);
                if (!Ret) continue;
                Value *rv = Ret->getReturnValue();
                if (!rv) continue;   // ret void
                if (!rv || !rv->getIRType()) continue;
                auto ty = rv->getIRType();

                if (ty->getEnum() == IR_INT) {
                    IntRange r;
                    if (auto *CI = dynamic_cast<ConstInt *>(rv))
                        r = IntRange(CI->getVal());
                    else {
                        auto it = ValueRanges.find(rv);
                        r = (it != ValueRanges.end()) ? it->second : IntRange();
                    }
                    retR = retR.unionWith(r);
                    hasInt = true;
                } else if (ty->getEnum() == IR_FLOAT) {
                    FloatRange r;
                    if (auto *CF = dynamic_cast<ConstFloat *>(rv))
                        r = FloatRange((double)CF->getVal());
                    else {
                        auto it = FloatValueRanges.find(rv);
                        r = (it != FloatValueRanges.end()) ? it->second : FloatRange();
                    }
                    fretR = fretR.unionWith(r);
                    hasFloat = true;
                }
            }
        }

        if (hasInt) {
            // 多次分析同一函数（pipeline 多次调用）可能得到不同 return range
            // 正确做法是 union 保守保留所有可能值，不能 intersect（会漏掉实际返回值）
            auto it = cache.RetRanges.find(&F);
            if (it == cache.RetRanges.end())
                cache.RetRanges[&F] = retR;
            else
                cache.RetRanges[&F] = it->second.unionWith(retR);
        }
        if (hasFloat) {
            auto it = cache.FloatRetRanges.find(&F);
            if (it == cache.FloatRetRanges.end())
                cache.FloatRetRanges[&F] = fretR;
            else
                cache.FloatRetRanges[&F] = it->second.unionWith(fretR);
        }
    }

    // Forward 不动点迭代主循环
    void runForward() {
        while (!BBWorkList.empty() || !InstWorkList.empty()) {
            // 优先处理指令队列
            while (!InstWorkList.empty()) {
                Instruction *I = InstWorkList.back();
                InstWorkList.pop_back();
                if (!ExecutableBlocks.count(
                        I->getParent()))
                    continue;
                visitInstruction(I);
            }

            // 处理块队列
            if (!BBWorkList.empty()) {
                BasicBlock *BB = BBWorkList.back();
                BBWorkList.pop_back();
                for (Instruction *I : *BB) {
                    visitInstruction(I);
                }
            }
        }
    }

    // Narrowing 阶段：backward constraint propagation
    //
    // Forward 收敛后某些值被 widen 到 Top，此阶段用 intersect-only 语义
    // 重跑若干轮，把这些值收回来。重点：
    //   1) updateRange 切到 intersect-only 模式（NarrowingMode=true）
    //   2) 二元运算 visitBinary 会调用 narrowBinaryBackward 做 z=x+y ⟹ x=z-y
    //      这种反推
    //   3) 轮次有上限（NARROW_ROUNDS=3），不追求极限精度，避免过度迭代
    void runNarrowing(Function &F) {
        NarrowingMode = true;
        // 硬预算：narrowing 总访问指令次数上限，避免病态函数上跑飞
        // 经验值：指令数 * 4 足够大多数场景收敛
        size_t instCount = 0;
        for (BasicBlock *BB : F) {
            if (ExecutableBlocks.count(BB)) {
                for (Instruction *I : *BB) { (void)I; instCount++; }
            }
        }
        size_t budget = instCount * 4 + 64;

        // 单次 pass：把所有可达指令压入 worklist 一次，然后驱动收敛
        // worklist 本身的传播会把"narrow 后需要复查"的用户拉回来
        for (BasicBlock *BB : F) {
            if (!ExecutableBlocks.count(BB)) continue;
            for (Instruction *I : *BB) {
                InstWorkList.push_back(I);
            }
        }

        while (!InstWorkList.empty() && budget > 0) {
            Instruction *I = InstWorkList.back();
            InstWorkList.pop_back();
            if (!ExecutableBlocks.count(
                    I->getParent()))
                continue;
            visitInstruction(I);
            budget--;
        }

        // 预算耗尽就清空剩余 worklist，保守放弃
        InstWorkList.clear();
        NarrowingMode = false;
    }

    // --- 主求解入口 ---

    RangeInfo solve(Function &F, SCEVResult *SCEV = nullptr) {
        CurFunc = &F;

        // Step 1: Forward 不动点（widening）
        // 参数保持 Top 初值（不从 cache seed，避免函数被用偏紧参数分析导致
        // 返回值低估）
        markBlockExecutable(F.front());
        runForward();

        // Step 2: 把本函数的返回值 range 写到跨函数 cache
        // 因为 Step 1 用 Top 参数分析，这里得到的 return range 覆盖了所有
        // 可能输入，对任何 caller 都是 safe 的保守估计
        publishReturnRange(F);

        // Step 3: Narrowing（backward constraint propagation）
        runNarrowing(F);

        // Step 4: 用 SCEV 证明出的有限归纳变量范围补充循环体上下文信息。
        // 只写 non-header loop blocks，保留 header/exit 判断的保守性。
        applySCEVContextHints(SCEV);

        // Step 5: 构建结果
        RangeInfo info;
        info.GlobalRanges = std::move(ValueRanges);
        info.ContextualRanges = std::move(ContextualRanges);
        info.BlockLocalRanges = std::move(BlockLocalRanges);
        info.FloatGlobalRanges = std::move(FloatValueRanges);
        info.FloatContextualRanges = std::move(FloatContextualRanges);
        info.GlobalBits = std::move(ValueBits);
        info.ContextualBits = std::move(ContextualBits);
        if (ScevInductions)
            info.LoopInductions = *ScevInductions;
        return info;
    }
};

// =====================================================================
// RangeAnalysis Pass
// =====================================================================

PassID RangeAnalysis::Key;

RangeAnalysis::Result RangeAnalysis::run(Function &F,
                                          FunctionAnalysisPassManager &FAM) const {
    auto &SCEV = FAM.getResult<SCEVAnalysis>(F);
    RangeSolver solver;
    return solver.solve(F, &SCEV);
}
