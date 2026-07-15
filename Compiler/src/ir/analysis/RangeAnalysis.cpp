#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/SCEV.hpp"
#include "lib/TraverseTools.hpp"

#include <queue>
#include <algorithm>
#include <cstdlib>
#include <limits>

// =====================================================================
// IntRange 算术运算
//
// 所有运算用 int64 检测 i32 溢出。普通 i32 IR 是 wrap 语义；一旦
// 数学区间可能越界，饱和端点并不保守，必须退回 Top。
// =====================================================================

int IntRange::trailingZerosOf(int64_t v) {
    if (v == 0) return 31;
    uint64_t u = static_cast<uint64_t>(v < 0 ? -v : v);
    int n = 0;
    while (((u >> n) & 1ULL) == 0 && n < 31) n++;
    return n;
}

IntRange::IntRange()
    : min(I32_MIN), max(I32_MAX), knownTrailingZeros(0) {}

IntRange::IntRange(int64_t val)
    : min(val), max(val), knownTrailingZeros(trailingZerosOf(val)) {}

IntRange::IntRange(int64_t lo, int64_t hi)
    : min(lo), max(hi), knownTrailingZeros(0) {}

bool IntRange::isTop() const {
    return min == I32_MIN && max == I32_MAX;
}

bool IntRange::isBottom() const {
    return min > max;
}

bool IntRange::isSingleValue() const {
    return min == max && !isBottom();
}

bool IntRange::isNonNegative() const {
    return !isBottom() && min >= 0;
}

bool IntRange::isPositive() const {
    return !isBottom() && min > 0;
}

bool IntRange::isMultipleOf(int k) const {
    return knownTrailingZeros >= k;
}

IntRange IntRange::unionWith(const IntRange &other) const {
    if (isBottom()) return other;
    if (other.isBottom()) return *this;
    IntRange r(std::min(min, other.min), std::max(max, other.max));
    r.knownTrailingZeros =
        std::min(knownTrailingZeros, other.knownTrailingZeros);
    return r;
}

IntRange IntRange::intersectWith(const IntRange &other) const {
    IntRange r(std::max(min, other.min), std::min(max, other.max));
    r.knownTrailingZeros =
        std::max(knownTrailingZeros, other.knownTrailingZeros);
    return r;
}

bool IntRange::operator==(const IntRange &o) const {
    return min == o.min && max == o.max &&
           knownTrailingZeros == o.knownTrailingZeros;
}

bool IntRange::operator!=(const IntRange &o) const {
    return !(*this == o);
}

IntRange IntRange::bottom() {
    return IntRange(1, 0);
}

IntRange IntRange::nonNegative() {
    return IntRange(0, I32_MAX);
}

IntRange IntRange::add(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    int64_t lo = min + rhs.min;
    int64_t hi = max + rhs.max;
    if (lo < I32_MIN || hi > I32_MAX) return IntRange();
    IntRange r(lo, hi);
    // 对齐：a + b 的 trailing zeros ≥ min(a.tz, b.tz)
    r.knownTrailingZeros = std::min(knownTrailingZeros, rhs.knownTrailingZeros);
    return r;
}

IntRange IntRange::sub(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    int64_t lo = min - rhs.max;
    int64_t hi = max - rhs.min;
    if (lo < I32_MIN || hi > I32_MAX) return IntRange();
    IntRange r(lo, hi);
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
    if (lo < I32_MIN || hi > I32_MAX) return IntRange();
    IntRange r(lo, hi);
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
    if (lo < I32_MIN || hi > I32_MAX) return IntRange();
    return IntRange(lo, hi);
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
KnownBits::KnownBits() = default;

KnownBits::KnownBits(uint32_t z, uint32_t o) : zero(z), one(o) {}

KnownBits KnownBits::top() {
    return {};
}

KnownBits KnownBits::fromConst(int32_t v) {
    return {static_cast<uint32_t>(~v), static_cast<uint32_t>(v)};
}

bool KnownBits::isConflict() const {
    return (zero & one) != 0;
}

bool KnownBits::isTop() const {
    return zero == 0 && one == 0;
}

bool KnownBits::isFullyKnown() const {
    return !isConflict() && (zero | one) == 0xFFFFFFFFu;
}

int32_t KnownBits::getConstant() const {
    return static_cast<int32_t>(one);
}

int KnownBits::getTrailingZeros() const {
    int n = 0;
    while (n < 32 && ((zero >> n) & 1u)) n++;
    return n;
}

int KnownBits::getTrailingOnes() const {
    int n = 0;
    while (n < 32 && ((one >> n) & 1u)) n++;
    return n;
}

bool KnownBits::knownNonNegative() const {
    return (zero & 0x80000000u) != 0;
}

bool KnownBits::knownNegative() const {
    return (one & 0x80000000u) != 0;
}

KnownBits KnownBits::unionWith(const KnownBits &o) const {
    return {zero & o.zero, one & o.one};
}

KnownBits KnownBits::intersectWith(const KnownBits &o) const {
    uint32_t z = zero | o.zero;
    uint32_t w = one | o.one;
    uint32_t conflict = z & w;
    return {z & ~conflict, w & ~conflict};
}

bool KnownBits::operator==(const KnownBits &o) const {
    return zero == o.zero && one == o.one;
}

bool KnownBits::operator!=(const KnownBits &o) const {
    return !(*this == o);
}

KnownBits KnownBits::shl(int k) const {
    if (k == 0) return *this;
    if (k < 0 || k >= 32) return top();
    uint32_t lowZero = (1u << k) - 1;
    return {(zero << k) | lowZero, one << k};
}

KnownBits KnownBits::lshr(int k) const {
    if (k == 0) return *this;
    if (k < 0 || k >= 32) return top();
    uint32_t hiZero = ~0u << (32 - k);
    return {(zero >> k) | hiZero, one >> k};
}

KnownBits KnownBits::ashr(int k) const {
    if (k == 0) return *this;
    if (k < 0 || k >= 32) return top();
    uint32_t hiMask = ~0u << (32 - k);
    uint32_t signZero = (zero >> 31) & 1u;
    uint32_t signOne = (one >> 31) & 1u;
    return {(zero >> k) | (signZero ? hiMask : 0u),
            (one >> k) | (signOne ? hiMask : 0u)};
}

KnownBits KnownBits::band(const KnownBits &o) const {
    return {zero | o.zero, one & o.one};
}

KnownBits KnownBits::bor(const KnownBits &o) const {
    return {zero & o.zero, one | o.one};
}

KnownBits KnownBits::bxor(const KnownBits &o) const {
    uint32_t knownA = zero | one;
    uint32_t knownB = o.zero | o.one;
    uint32_t both = knownA & knownB;
    uint32_t resOne = (one ^ o.one) & both;
    uint32_t resZero = (~resOne) & both;
    return {resZero, resOne};
}

KnownBits KnownBits::add(const KnownBits &o) const {
    int tz = std::min(getTrailingZeros(), o.getTrailingZeros());
    if (tz == 0) return top();
    if (tz >= 32) return {~0u, 0};
    return {static_cast<uint32_t>((1u << tz) - 1), 0};
}

KnownBits KnownBits::sub(const KnownBits &o) const {
    int tz = std::min(getTrailingZeros(), o.getTrailingZeros());
    if (tz == 0) return top();
    if (tz >= 32) return {~0u, 0};
    return {static_cast<uint32_t>((1u << tz) - 1), 0};
}

KnownBits KnownBits::mul(const KnownBits &o) const {
    int tz = getTrailingZeros() + o.getTrailingZeros();
    if (tz >= 32) return {~0u, 0};
    if (tz == 0) return top();
    return {static_cast<uint32_t>((1u << tz) - 1), 0};
}

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
// 用 float 完成端点运算，再提升为 double 存储。如果直接用
// double 运算，FLT_MAX + FLT_MAX 会被误认为有限数，而真实 f32
// 结果是 +Inf。NaN 和 +/-0 与数值区间分开传播。
// =====================================================================

double FloatRange::F_MIN_INF() {
    return -std::numeric_limits<double>::infinity();
}

double FloatRange::F_MAX_INF() {
    return std::numeric_limits<double>::infinity();
}

FloatRange::FloatRange()
    : min(F_MIN_INF()), max(F_MAX_INF()), mayNaN(true),
      mayPositiveZero(true), mayNegativeZero(true) {}

FloatRange::FloatRange(double val)
    : min(val), max(val), mayNaN(std::isnan(val)), mayPositiveZero(false),
      mayNegativeZero(false) {
    if (mayNaN) {
        min = 1.0;
        max = 0.0;
    } else if (val == 0.0) {
        mayNegativeZero = std::signbit(val);
        mayPositiveZero = !mayNegativeZero;
    }
}

FloatRange::FloatRange(double lo, double hi) {
    if (std::isnan(lo) || std::isnan(hi)) {
        min = F_MIN_INF();
        max = F_MAX_INF();
        mayNaN = true;
        mayPositiveZero = true;
        mayNegativeZero = true;
    } else {
        min = lo;
        max = hi;
        mayNaN = false;
        bool ContainsZero = lo <= 0.0 && hi >= 0.0;
        mayPositiveZero = ContainsZero;
        mayNegativeZero = ContainsZero;
    }
}

bool FloatRange::hasNumericValues() const {
    return min <= max;
}

bool FloatRange::mayContainNaN() const {
    return mayNaN;
}

bool FloatRange::containsZero() const {
    return hasNumericValues() && min <= 0.0 && max >= 0.0 &&
           (mayPositiveZero || mayNegativeZero);
}

bool FloatRange::mayContainInfinity() const {
    return hasNumericValues() && (std::isinf(min) || std::isinf(max));
}

bool FloatRange::isTop() const {
    return min == F_MIN_INF() && max == F_MAX_INF() && mayNaN &&
           mayPositiveZero && mayNegativeZero;
}

bool FloatRange::isBottom() const {
    return !hasNumericValues() && !mayNaN;
}

bool FloatRange::isNaNOnly() const {
    return !hasNumericValues() && mayNaN;
}

bool FloatRange::isExactNumericValue() const {
    if (mayNaN || !hasNumericValues() || min != max) return false;
    if (min != 0.0) return true;
    return mayPositiveZero != mayNegativeZero;
}

bool FloatRange::isSingleValue() const {
    return isExactNumericValue() && !std::isinf(min);
}

bool FloatRange::isNonNegative() const {
    return !mayNaN && hasNumericValues() && min >= 0.0;
}

bool FloatRange::operator==(const FloatRange &o) const {
    return min == o.min && max == o.max && mayNaN == o.mayNaN &&
           mayPositiveZero == o.mayPositiveZero &&
           mayNegativeZero == o.mayNegativeZero;
}

bool FloatRange::operator!=(const FloatRange &o) const {
    return !(*this == o);
}

bool FloatRange::containsPosInf(const FloatRange &R) {
    return R.hasNumericValues() && R.max == FloatRange::F_MAX_INF();
}

bool FloatRange::containsNegInf(const FloatRange &R) {
    return R.hasNumericValues() && R.min == FloatRange::F_MIN_INF();
}

float FloatRange::exactF32Value(const FloatRange &R) {
    if (R.min == 0.0 && R.mayNegativeZero && !R.mayPositiveZero)
        return -0.0f;
    return static_cast<float>(R.min);
}

bool FloatRange::eitherNaNPropagates(const FloatRange &L,
                                     const FloatRange &R) {
    bool LHasAny = L.hasNumericValues() || L.mayNaN;
    bool RHasAny = R.hasNumericValues() || R.mayNaN;
    return (L.mayNaN && RHasAny) || (R.mayNaN && LHasAny);
}

FloatRange FloatRange::bottom() {
    FloatRange R(1.0, 0.0);
    R.mayNaN = false;
    R.mayPositiveZero = false;
    R.mayNegativeZero = false;
    return R;
}

FloatRange FloatRange::sanitize(double lo, double hi, bool MayNaN) {
    if (std::isnan(lo) || std::isnan(hi)) return FloatRange();
    FloatRange R = (lo > hi) ? FloatRange::bottom() : FloatRange(lo, hi);
    R.mayNaN = MayNaN;
    return R;
}

FloatRange FloatRange::unionWith(const FloatRange &other) const {
    FloatRange R = FloatRange::bottom();
    R.mayNaN = mayNaN || other.mayNaN;

    if (hasNumericValues() && other.hasNumericValues()) {
        R.min = std::min(min, other.min);
        R.max = std::max(max, other.max);
    } else if (hasNumericValues()) {
        R.min = min;
        R.max = max;
    } else if (other.hasNumericValues()) {
        R.min = other.min;
        R.max = other.max;
    }

    if (R.hasNumericValues() && R.min <= 0.0 && R.max >= 0.0) {
        R.mayPositiveZero = mayPositiveZero || other.mayPositiveZero;
        R.mayNegativeZero = mayNegativeZero || other.mayNegativeZero;
    }
    return R;
}

FloatRange FloatRange::intersectWith(const FloatRange &other) const {
    FloatRange R = FloatRange::bottom();
    R.mayNaN = mayNaN && other.mayNaN;
    if (hasNumericValues() && other.hasNumericValues()) {
        R.min = std::max(min, other.min);
        R.max = std::min(max, other.max);
        if (R.hasNumericValues() && R.min <= 0.0 && R.max >= 0.0) {
            R.mayPositiveZero = mayPositiveZero && other.mayPositiveZero;
            R.mayNegativeZero = mayNegativeZero && other.mayNegativeZero;
            // If zero was the only numerical intersection but neither sign
            // is present, the numerical intersection is empty.
            if (R.min == 0.0 && R.max == 0.0 &&
                !R.mayPositiveZero && !R.mayNegativeZero) {
                R.min = 1.0;
                R.max = 0.0;
            }
        }
    }
    return R;
}

FloatRange FloatRange::add(const FloatRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    bool MayNaN = eitherNaNPropagates(*this, rhs) ||
                  (containsPosInf(*this) && containsNegInf(rhs)) ||
                  (containsNegInf(*this) && containsPosInf(rhs));
    return evaluateFloatEndpoints(*this, rhs, MayNaN,
                                  [](float A, float B) { return A + B; });
}

FloatRange FloatRange::sub(const FloatRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    bool MayNaN = eitherNaNPropagates(*this, rhs) ||
                  (containsPosInf(*this) && containsPosInf(rhs)) ||
                  (containsNegInf(*this) && containsNegInf(rhs));
    return evaluateFloatEndpoints(*this, rhs, MayNaN,
                                  [](float A, float B) { return A - B; });
}

FloatRange FloatRange::mul(const FloatRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    bool MayNaN = eitherNaNPropagates(*this, rhs) ||
                  (containsZero() && rhs.mayContainInfinity()) ||
                  (rhs.containsZero() && mayContainInfinity());
    FloatRange R = evaluateFloatEndpoints(
        *this, rhs, MayNaN, [](float A, float B) { return A * B; });
    auto HasFiniteNumeric = [](const FloatRange &X) {
        return X.hasNumericValues() &&
               X.min < FloatRange::F_MAX_INF() &&
               X.max > FloatRange::F_MIN_INF();
    };
    // Endpoint-only evaluation of [-inf,+inf] * 0 sees NaN at both
    // endpoints, but finite interior values also produce signed zero.
    if ((containsZero() && HasFiniteNumeric(rhs)) ||
        (rhs.containsZero() && HasFiniteNumeric(*this)))
        R = R.unionWith(FloatRange(0.0, 0.0));
    return R;
}

FloatRange FloatRange::fdiv(const FloatRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    bool MayNaN = eitherNaNPropagates(*this, rhs) ||
                  (containsZero() && rhs.containsZero()) ||
                  (mayContainInfinity() && rhs.mayContainInfinity());

    // Preserve exact 0/0, inf/inf and signed-zero behaviour before the
    // discontinuous interval fallback below.
    if (isExactNumericValue() && rhs.isExactNumericValue()) {
        float V = exactF32Value(*this) / exactF32Value(rhs);
        FloatRange Exact(static_cast<double>(V));
        Exact.mayNaN = Exact.mayNaN || MayNaN;
        return Exact;
    }

    // A denominator envelope touching/crossing numerical zero is
    // discontinuous even when the zero-sign set says exact zero itself is
    // absent (for example a PHI of {-1,+1}).  The interval abstraction does
    // not track a minimum nonzero magnitude, so only the full numerical
    // envelope is sound.  MayNaN separately records whether exact 0/0 exists.
    bool DenominatorTouchesZero = rhs.hasNumericValues() &&
                                  rhs.min <= 0.0 && rhs.max >= 0.0;
    if (DenominatorTouchesZero) {
        FloatRange R(F_MIN_INF(), F_MAX_INF());
        R.mayNaN = MayNaN;
        return R;
    }

    FloatRange R = evaluateFloatEndpoints(
        *this, rhs, MayNaN, [](float A, float B) { return A / B; });
    // As above, an unbounded numerator divided by exact infinity has NaN at
    // its infinite endpoints but signed-zero results for finite interiors.
    bool NumeratorHasFinite = hasNumericValues() &&
                              min < F_MAX_INF() && max > F_MIN_INF();
    if (NumeratorHasFinite && rhs.mayContainInfinity())
        R = R.unionWith(FloatRange(0.0, 0.0));
    return R;
}

LoopInductionInfo::LoopInductionInfo()
    : RecLoop(nullptr), initial(0), step(0), hasUpperBound(false),
      upperBound(0), noWrap(false) {}

// =====================================================================
// RangeInfo 查询接口
// =====================================================================

void RangeInfo::setLoopInduction(Value *v,
                                 const LoopInductionInfo &info) {
    LoopInductions[v] = info;
}

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
// 边敏感查询（沿 predBB → succBB 边进入 succBB 时的值）。
// 先取 pred 处的支配上下文，再和最终 CFG rebuild 产生的
// edge fact 取交集。这也适用于 loop header 等多前驱块，
// 不会把某一条入边的条件泄漏到其他入边。
// =====================================================================
IntRange RangeInfo::queryOnEdge(Value *val, BasicBlock *predBB,
                                 BasicBlock *succBB,
                                 const DominatorTree &DT) const {
    if (auto *CI = dynamic_cast<ConstInt *>(val)) return IntRange(CI->getVal());
    IntRange R = query(val, predBB, DT);
    if (!predBB || !succBB) return R;
    auto VIt = EdgeRanges.find(val);
    if (VIt != EdgeRanges.end()) {
        auto PIt = VIt->second.find(predBB);
        if (PIt != VIt->second.end()) {
            auto SIt = PIt->second.find(succBB);
            if (SIt != PIt->second.end())
                R = R.intersectWith(SIt->second);
        }
    }
    return R;
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
    KnownBits KB = queryBits(val, predBB, DT);
    if (!predBB || !succBB) return KB;
    auto VIt = EdgeBits.find(val);
    if (VIt != EdgeBits.end()) {
        auto PIt = VIt->second.find(predBB);
        if (PIt != VIt->second.end()) {
            auto SIt = PIt->second.find(succBB);
            if (SIt != PIt->second.end())
                KB = KB.intersectWith(SIt->second);
        }
    }
    return KB;
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

IntRange &RangeSolver::getRange(Value *V) {
    // 全局范围默认初始化为 Top（全范围）。
    // 每条指令首次 visit 时会被 visitXxx 赋予计算出的范围。
    return ValueRanges[V];
}

// KnownBits 取值：常量直接返回精确 KB，其他值查表
KnownBits RangeSolver::getBits(Value *V) {
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
IntRange RangeSolver::getRangeInBlock(Value *V, BasicBlock *BB) {
    IntRange r = getRange(V);
    if (!BB) return r;
    auto it = ContextualRanges.find(V);
    if (it != ContextualRanges.end()) {
        auto it2 = it->second.find(BB);
        if (it2 != it->second.end()) r = r.intersectWith(it2->second);
    }
    return r;
}

KnownBits RangeSolver::getBitsInBlock(Value *V, BasicBlock *BB) {
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

// 统一把某个值的指令用户重新加入求解队列，并保留 use-list 的顺序和重复项。
void RangeSolver::enqueueInstructionUsers(Value *V) {
    if (!V) return;
    for (Instruction *User : V->getInstructionUsers())
        InstWorkList.push_back(User);
}

// KnownBits 更新。forward 覆盖；narrowing 走 intersect。
// 任何一方 bit 变化都把 users 放回 worklist，让 IntRange 有机会通过
// KB.toIntRange() 被重新拉紧。
bool RangeSolver::updateBits(Value *V, const KnownBits &newBits) {
    auto &cur = ValueBits[V];
    KnownBits merged = NarrowingMode ? cur.intersectWith(newBits) : newBits;
    if (merged.isConflict()) return false; // 不可达路径保守忽略
    if (merged == cur) return false;
    cur = merged;
    enqueueInstructionUsers(V);
    return true;
}

void RangeSolver::updateBitsContextual(Value *V, BasicBlock *block,
                                   const KnownBits &kb) {
    auto it = ContextualBits[V].find(block);
    // Top 约束等于没约束，删除旧的（和 updateContextual 对齐）
    if (kb.isTop()) {
        if (it == ContextualBits[V].end()) return;
        ContextualBits[V].erase(it);
        enqueueInstructionUsers(V);
        return;
    }
    KnownBits &cur = ContextualBits[V][block];
    if (kb == cur) return;
    cur = kb;
    enqueueInstructionUsers(V);
}

// 联动：从 KnownBits 反推一条 IntRange，和 newRange intersect 作为最终写入
IntRange RangeSolver::tightenWithBits(Value *V, const IntRange &newRange) {
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
bool RangeSolver::updateRange(Value *V, const IntRange &newRange) {
    // ---- Narrowing 路径 ----
    if (NarrowingMode) {
        auto &cur = ValueRanges[V];
        IntRange intersected = cur.intersectWith(newRange);
        // 不引入 bottom（空区间意味着不可达，narrowing 阶段保守忽略）
        if (intersected.isBottom()) return false;
        if (intersected == cur) return false;
        cur = intersected;
        enqueueInstructionUsers(V);
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

    enqueueInstructionUsers(V);
    return true;
}

// 浮点版本的 updateRange（forward/narrowing 同样语义）
bool RangeSolver::updateFloatRange(Value *V, const FloatRange &newRange) {
    if (NarrowingMode) {
        auto &cur = FloatValueRanges[V];
        FloatRange intersected = cur.intersectWith(newRange);
        if (intersected.isBottom()) return false;
        if (intersected == cur) return false;
        cur = intersected;
        enqueueInstructionUsers(V);
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
    enqueueInstructionUsers(V);
    return true;
}

// 上下文范围更新：赋值语义。
// 条件分支收窄产生的 per-block 约束，每次分支指令被 visit 时重新计算。
// 如果分支操作数的范围变宽了，收窄结果也会相应变宽。
void RangeSolver::updateContextual(Value *V, BasicBlock *block,
                               const IntRange &range) {
    auto it = ContextualRanges[V].find(block);

    if (range.isTop()) {
        // 约束退化成 Top：必须删除旧约束，不能残留过窄范围
        if (it == ContextualRanges[V].end()) return; // 本来就没有，不用动
        ContextualRanges[V].erase(it);
        // 通知使用者：约束变宽了
        enqueueInstructionUsers(V);
        return;
    }

    IntRange &cur = ContextualRanges[V][block];
    if (range == cur) return;
    cur = range;
    enqueueInstructionUsers(V);
}

// 浮点版本的 updateContextual
void RangeSolver::updateFloatContextual(Value *V, BasicBlock *block,
                                    const FloatRange &range) {
    auto it = FloatContextualRanges[V].find(block);

    if (range.isTop()) {
        if (it == FloatContextualRanges[V].end()) return;
        FloatContextualRanges[V].erase(it);
        enqueueInstructionUsers(V);
        return;
    }

    FloatRange &cur = FloatContextualRanges[V][block];
    if (range == cur) return;
    cur = range;
    enqueueInstructionUsers(V);
}

void RangeSolver::markBlockExecutable(BasicBlock *BB) {
    if (ExecutableBlocks.insert(BB).second)
        BBWorkList.push_back(BB);
}

bool RangeSolver::markEdgeFeasible(BasicBlock *From, BasicBlock *To) {
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
// 根据 SCEV 归纳变量信息推断活动迭代范围
IntRange RangeSolver::rangeFromInduction(
const LoopInductionInfo &info) const {
    if (!info.hasUpperBound || !info.noWrap) return IntRange(); // Top
    if (info.step > 0)
        return IntRange(info.initial, info.upperBound);
    if (info.step < 0)
        return IntRange(info.upperBound, info.initial);
    return IntRange(info.initial);
}

void RangeSolver::applySCEVContextHints(SCEVResult *SCEV) {
    if (!SCEV) return;

    const auto &inductions = SCEV->getLoopInductions();
    ScevInductions = &inductions;

    for (const auto &entry : inductions) {
        Value *V = entry.first;
        const LoopInductionInfo &info = entry.second;
        if (!V || !info.RecLoop) continue;
        if (!info.noWrap) continue;
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

void RangeSolver::visitPhi(PhiInst *Phi) {
    // 浮点 PHI
    auto phiTy = Phi->getIRType();
    if (phiTy && phiTy->getEnum() == IR_FLOAT) {
        FloatRange fresult = FloatRange::bottom();
        BasicBlock *phiBB = Phi->getParent();
        TraverseTools::forEachPhiIncoming(
            Phi, [this, phiBB, &fresult](unsigned, Value *inVal,
                                         BasicBlock *pred) {
            if (!FeasibleEdges.count({pred, phiBB})) return;
            FloatRange inRange;
            if (auto *CF = dynamic_cast<ConstFloat *>(inVal))
                inRange = FloatRange((double)CF->getVal());
            else
                inRange = FloatValueRanges[inVal];
            fresult = fresult.unionWith(inRange);
            });
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
    BasicBlock *phiBB = Phi->getParent();
    TraverseTools::forEachPhiIncoming(
        Phi, [this, phiBB, &result, &kbResult, &firstKB](
                 unsigned, Value *inVal, BasicBlock *pred) {
        if (!FeasibleEdges.count({pred, phiBB})) return;
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
        });
    updateBits(Phi, kbResult);
    result = tightenWithBits(Phi, result);
    updateRange(Phi, result);
}

void RangeSolver::visitBinary(BinaryInst *BI) {
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


        // Do not backward-narrow floating ADD/SUB: f32 rounding means the
        // real-number inverse interval is not a sound proof for operands.
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

    const int ResultBitWidth = resultTy && resultTy->isInt()
        ? (resultTy->isBool() ? 1 : resultTy->getIntegerBitWidth(0))
        : 0;
    const bool IsModeledI32 = ResultBitWidth == 32;
    auto HasModeledShiftAmount = [&]() {
        if (!rRange.isSingleValue() || rRange.min < 0 ||
            rRange.min >= ResultBitWidth)
            return false;
        // IntRange/KnownBits model 32-bit arithmetic.  A zero shift is an
        // identity at every width; other narrow-width shifts conservatively
        // stay Top until the lattices become width-parametric.
        return IsModeledI32 || rRange.min == 0;
    };

    IntRange result;
    switch (BI->getOp()) {
        case BinaryInst::ADD:  result = IsModeledI32 ? lRange.add(rRange) : IntRange();  break;
        case BinaryInst::SUB:  result = IsModeledI32 ? lRange.sub(rRange) : IntRange();  break;
        case BinaryInst::MUL:  result = IsModeledI32 ? lRange.mul(rRange) : IntRange();  break;
        case BinaryInst::DIV:  result = IsModeledI32 ? lRange.sdiv(rRange) : IntRange(); break;
        case BinaryInst::MOD:  result = IsModeledI32 ? lRange.srem(rRange) : IntRange(); break;
        // 位运算/移位：走扩展的 IntRange 方法
        case BinaryInst::SHL:  result = HasModeledShiftAmount() ? lRange.shl(rRange) : IntRange();  break;
        case BinaryInst::ASHR: result = HasModeledShiftAmount() ? lRange.ashr(rRange) : IntRange(); break;
        case BinaryInst::LSHR: result = HasModeledShiftAmount() ? lRange.lshr(rRange) : IntRange(); break;
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
        case BinaryInst::ADD: if (IsModeledI32) resKB = lKB.add(rKB); break;
        case BinaryInst::SUB: if (IsModeledI32) resKB = lKB.sub(rKB); break;
        case BinaryInst::MUL: if (IsModeledI32) resKB = lKB.mul(rKB); break;
        case BinaryInst::AND: resKB = lKB.band(rKB); break;
        case BinaryInst::OR:  resKB = lKB.bor(rKB); break;
        case BinaryInst::XOR: resKB = lKB.bxor(rKB); break;
        case BinaryInst::SHL: {
            int k = rhsAsConstK();
            if (HasModeledShiftAmount() && k == rRange.min)
                resKB = lKB.shl(k);
            break;
        }
        case BinaryInst::LSHR: {
            int k = rhsAsConstK();
            if (HasModeledShiftAmount() && k == rRange.min)
                resKB = lKB.lshr(k);
            break;
        }
        case BinaryInst::ASHR: {
            int k = rhsAsConstK();
            if (HasModeledShiftAmount() && k == rRange.min)
                resKB = lKB.ashr(k);
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
    if (NarrowingMode && IsModeledI32) {
        narrowBinaryBackward(BI, lhs, rhs, lRange, rRange);
    }
}

// 反推二元运算操作数的范围（仅 narrowing 阶段使用）
//
// 完整反推（ADD/SUB）：
//   z = x + y ⟹ x = z - y,  y = z - x
//   z = x - y ⟹ x = z + y,  y = x - z
// MUL/DIV/MOD 以及浮点运算不做反推：wrap、除法溢出和 f32 舍入都
// 需要比当前区间 lattice 更强的证明。
void RangeSolver::narrowBinaryBackward(
BinaryInst *BI, Value *lhs, Value *rhs, const IntRange &lRange,
const IntRange &rRange) {
    auto ty = BI->getIRType();
    if (!ty) return;

    // 整数路径
    if (ty->getEnum() == IR_INT) {
        IntRange zR = getRange(BI);
        switch (BI->getOp()) {
            case BinaryInst::ADD: {
                if (lRange.add(rRange).isTop()) break;
                if (!dynamic_cast<ConstInt *>(lhs))
                    updateRange(lhs, zR.sub(rRange));
                if (!dynamic_cast<ConstInt *>(rhs))
                    updateRange(rhs, zR.sub(lRange));
                break;
            }
            case BinaryInst::SUB: {
                if (lRange.sub(rRange).isTop()) break;
                if (!dynamic_cast<ConstInt *>(lhs))
                    updateRange(lhs, zR.add(rRange));
                if (!dynamic_cast<ConstInt *>(rhs))
                    updateRange(rhs, lRange.sub(zR));
                break;
            }
            // MUL/DIV/MOD backward sign inference is disabled.  MUL may
            // wrap, and DIV/MOD need stronger nonzero/overflow facts than
            // this interval lattice currently records.
            default: break;
        }
        return;
    }

    // Floating-point backward narrowing is intentionally disabled because
    // f32 arithmetic is not an exact real-number equation.
}

void RangeSolver::visitBranch(BrInst *Br) {
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

            // 浮点分支收窄：操作数类型 IR_FLOAT 时走浮点路径。
            // 比较遵循 C/IEEE ordered 关系（!= 对 NaN 为真）。当前
            // 实现只消费“两侧全局范围都排除 NaN”的强证明；若任一侧
            // 可能为 NaN 就保守保留 Top，不把某条边错误解释为纯数值
            // 关系。
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

                if (!lF.hasNumericValues() || !rF.hasNumericValues() ||
                    lF.mayContainNaN() || rF.mayContainNaN()) {
                    if (trueSinglePred) {
                        updateFloatContextual(lhs, trueBB, FloatRange());
                        updateFloatContextual(rhs, trueBB, FloatRange());
                    }
                    if (falseSinglePred) {
                        updateFloatContextual(lhs, falseBB, FloatRange());
                        updateFloatContextual(rhs, falseBB, FloatRange());
                    }
                    markEdgeFeasible(fromBB, trueBB);
                    markEdgeFeasible(fromBB, falseBB);
                    return;
                }

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

            if (lRange.isBottom() || rRange.isBottom()) {
                if (trueSinglePred) {
                    updateContextual(lhs, trueBB, IntRange());
                    updateContextual(rhs, trueBB, IntRange());
                    updateBitsContextual(lhs, trueBB, KnownBits::top());
                    updateBitsContextual(rhs, trueBB, KnownBits::top());
                }
                if (falseSinglePred) {
                    updateContextual(lhs, falseBB, IntRange());
                    updateContextual(rhs, falseBB, IntRange());
                    updateBitsContextual(lhs, falseBB, KnownBits::top());
                    updateBitsContextual(rhs, falseBB, KnownBits::top());
                }
                markEdgeFeasible(fromBB, trueBB);
                markEdgeFeasible(fromBB, falseBB);
                return;
            }

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

void RangeSolver::visitCall(CallInst *CI) {
    Value *callee = CI->getCallee();
    if (!callee) return;

    auto SetTopForResultType = [&]() {
        auto Ty = CI->getIRType();
        if (!Ty) return;
        if (Ty->getEnum() == IR_INT) updateRange(CI, IntRange());
        else if (Ty->getEnum() == IR_FLOAT)
            updateFloatRange(CI, FloatRange());
    };

    // 内置函数范围推断
    if (auto *BIF = dynamic_cast<BuildInFunction *>(callee)) {
        std::string name = BIF->getIdent();
        if (name == "getch") {
            // Runtime loads an i8 and sign-extends it to i32.  Bytes above
            // 0x7f therefore produce negative values; ASCII-only [0,127]
            // is not a valid proof for arbitrary input bytes.
            updateRange(CI, IntRange(-128, 127));
            return;
        }
        if (name == "getint") {
            // getint 可以返回任何 i32
            updateRange(CI, IntRange());
            return;
        }
        // 其他内置函数返回值当 Top
        SetTopForResultType();
        return;
    }

    // 用户函数 return summary 在没有 module/call-graph invalidation
    // proxy 时可能过期。不消费 process-static cache，一律 Top。
    if (dynamic_cast<Function *>(callee)) {
        SetTopForResultType();
        return;
    }

    // 无法识别的 callee → Top
    SetTopForResultType();
}

// =====================================================================
// Select：%r = select %c, %a, %b
//
// 语义上是条件三元，整数/浮点均取 a、b 的 union。
// 不尝试靠 %c 的条件信息收窄（condition 是 boolean，其"真分支蕴含什么"
// 已经在 visitBranch 做过；select 不在 CFG 上，做不到支配推理）。
// =====================================================================
void RangeSolver::visitSelect(SelectInst *SI) {
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
void RangeSolver::visitCast(Instruction *I) {
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
        // sitofp rounds to f32.  Store the actual rounded endpoints rather
        // than the exact integer endpoints represented in double.
        if (r.isSingleValue()) {
            updateFloatRange(I, FloatRange(
                static_cast<double>(static_cast<float>(r.min))));
        } else {
            updateFloatRange(I, FloatRange(
                static_cast<double>(static_cast<float>(r.min)),
                static_cast<double>(static_cast<float>(r.max))));
        }
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
        if (r.isBottom() || !r.hasNumericValues() || r.mayContainNaN()) {
            updateRange(I, IntRange());
            return;
        }
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

void RangeSolver::visitInstruction(Instruction *I) {
    if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
        visitPhi(Phi);
    } else if (auto *BI = dynamic_cast<BinaryInst *>(I)) {
        visitBinary(BI);
    } else if (auto *Br = dynamic_cast<BrInst *>(I)) {
        visitBranch(Br);
    } else if (auto *CI = dynamic_cast<CallInst *>(I)) {
        visitCall(CI);
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

// Forward 不动点迭代主循环
void RangeSolver::runForward() {
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
bool RangeSolver::runNarrowing(Function &F) {
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

    const bool Complete = InstWorkList.empty();
    InstWorkList.clear();
    NarrowingMode = false;
    return Complete;
}

// Rebuild transform-visible integer path facts from the *final* value
// lattice and the *current* CFG.  Forward solving deliberately uses a
// mutable contextual map to accelerate convergence; exposing that map
// after widening/narrowing can retain a fact derived from an earlier,
// narrower operand state.  A single final scan is both sound and O(CFG).
void RangeSolver::rebuildIntegerContextualFacts(Function &F) {
    ContextualRanges.clear();
    ContextualBits.clear();
    EdgeRanges.clear();
    EdgeBits.clear();

    auto FinalRange = [&](Value *V) -> IntRange {
        if (auto *CI = dynamic_cast<ConstInt *>(V))
            return IntRange(CI->getVal());
        auto It = ValueRanges.find(V);
        return It == ValueRanges.end() ? IntRange() : It->second;
    };
    auto FinalBits = [&](Value *V) -> KnownBits {
        if (auto *CI = dynamic_cast<ConstInt *>(V))
            return KnownBits::fromConst(CI->getVal());
        auto It = ValueBits.find(V);
        return It == ValueBits.end() ? KnownBits::top() : It->second;
    };
    auto HasUniqueIncomingEdge = [](BasicBlock *Pred, BasicBlock *Succ) {
        if (!Pred || !Succ) return false;
        auto Preds = Succ->getPredecessors();
        return Preds.size() == 1 && Preds.front() == Pred &&
               Pred->countEdgesToSucc(Succ) == 1;
    };

    auto RecordRange = [&](Value *V, BasicBlock *Pred, BasicBlock *Succ,
                           const IntRange &Constraint) {
        if (!V || !Pred || !Succ || Constraint.isTop() ||
            Constraint.isBottom())
            return;
        IntRange Fact = FinalRange(V).intersectWith(Constraint);
        if (Fact.isTop() || Fact.isBottom()) return;

        auto &EdgeMap = EdgeRanges[V][Pred];
        auto Existing = EdgeMap.find(Succ);
        if (Existing != EdgeMap.end()) {
            Fact = Existing->second.intersectWith(Fact);
            if (Fact.isBottom()) {
                // The edge is infeasible, but this analysis does not hand
                // an explicit reachability proof to transforms.  Dropping
                // the fact is safer than exposing Bottom as a value range.
                EdgeMap.erase(Existing);
                if (HasUniqueIncomingEdge(Pred, Succ)) {
                    auto CIt = ContextualRanges.find(V);
                    if (CIt != ContextualRanges.end())
                        CIt->second.erase(Succ);
                }
                return;
            }
        }
        EdgeMap[Succ] = Fact;

        if (HasUniqueIncomingEdge(Pred, Succ)) {
            auto &BlockMap = ContextualRanges[V];
            auto BIt = BlockMap.find(Succ);
            if (BIt == BlockMap.end()) {
                BlockMap[Succ] = Fact;
            } else {
                IntRange Combined = BIt->second.intersectWith(Fact);
                if (Combined.isBottom()) BlockMap.erase(BIt);
                else BlockMap[Succ] = Combined;
            }
        }
    };

    auto RecordBits = [&](Value *V, BasicBlock *Pred, BasicBlock *Succ,
                          const KnownBits &Constraint) {
        if (!V || !Pred || !Succ || Constraint.isTop()) return;
        KnownBits Fact = FinalBits(V).intersectWith(Constraint);
        if (Fact.isTop() || Fact.isConflict()) return;

        auto &Slot = EdgeBits[V][Pred][Succ];
        Slot = Slot.intersectWith(Fact);
        if (HasUniqueIncomingEdge(Pred, Succ))
            ContextualBits[V][Succ] =
                ContextualBits[V][Succ].intersectWith(Fact);
    };

    for (BasicBlock *Pred : F) {
        if (!Pred) continue;
        auto *Br = Pred->getBranchTerminator();
        if (!Br || !Br->isConditional()) continue;
        BasicBlock *TrueBB = Br->getTrueSuccessor();
        BasicBlock *FalseBB = Br->getFalseSuccessor();
        // If both branch labels are the same there is no distinguishable
        // true/false edge in this IR's (pred,succ) edge key.
        if (!TrueBB || !FalseBB || TrueBB == FalseBB) continue;

        auto *Cmp = dynamic_cast<BinaryInst *>(Br->getCondition());
        if (!Cmp) continue;
        Value *LHS = nullptr;
        Value *RHS = nullptr;
        if (!Cmp->getOperands(LHS, RHS) || !LHS || !RHS ||
            !LHS->getIRType() || !RHS->getIRType() ||
            LHS->getIRType()->getEnum() != IR_INT ||
            RHS->getIRType()->getEnum() != IR_INT)
            continue;

        IntRange L = FinalRange(LHS);
        IntRange R = FinalRange(RHS);
        if (L.isBottom() || R.isBottom()) continue;

        switch (Cmp->getOp()) {
            case BinaryInst::L:
                RecordRange(LHS, Pred, TrueBB,
                            IntRange(IntRange::I32_MIN, R.max - 1));
                RecordRange(RHS, Pred, TrueBB,
                            IntRange(L.min + 1, IntRange::I32_MAX));
                RecordRange(LHS, Pred, FalseBB,
                            IntRange(R.min, IntRange::I32_MAX));
                RecordRange(RHS, Pred, FalseBB,
                            IntRange(IntRange::I32_MIN, L.max));
                break;
            case BinaryInst::LE:
                RecordRange(LHS, Pred, TrueBB,
                            IntRange(IntRange::I32_MIN, R.max));
                RecordRange(RHS, Pred, TrueBB,
                            IntRange(L.min, IntRange::I32_MAX));
                RecordRange(LHS, Pred, FalseBB,
                            IntRange(R.min + 1, IntRange::I32_MAX));
                RecordRange(RHS, Pred, FalseBB,
                            IntRange(IntRange::I32_MIN, L.max - 1));
                break;
            case BinaryInst::G:
                RecordRange(LHS, Pred, TrueBB,
                            IntRange(R.min + 1, IntRange::I32_MAX));
                RecordRange(RHS, Pred, TrueBB,
                            IntRange(IntRange::I32_MIN, L.max - 1));
                RecordRange(LHS, Pred, FalseBB,
                            IntRange(IntRange::I32_MIN, R.max));
                RecordRange(RHS, Pred, FalseBB,
                            IntRange(L.min, IntRange::I32_MAX));
                break;
            case BinaryInst::GE:
                RecordRange(LHS, Pred, TrueBB,
                            IntRange(R.min, IntRange::I32_MAX));
                RecordRange(RHS, Pred, TrueBB,
                            IntRange(IntRange::I32_MIN, L.max));
                RecordRange(LHS, Pred, FalseBB,
                            IntRange(IntRange::I32_MIN, R.max - 1));
                RecordRange(RHS, Pred, FalseBB,
                            IntRange(L.min + 1, IntRange::I32_MAX));
                break;
            case BinaryInst::E:
                RecordRange(LHS, Pred, TrueBB, R);
                RecordRange(RHS, Pred, TrueBB, L);
                RecordBits(LHS, Pred, TrueBB, FinalBits(RHS));
                RecordBits(RHS, Pred, TrueBB, FinalBits(LHS));
                break;
            case BinaryInst::NE:
                RecordRange(LHS, Pred, FalseBB, R);
                RecordRange(RHS, Pred, FalseBB, L);
                RecordBits(LHS, Pred, FalseBB, FinalBits(RHS));
                RecordBits(RHS, Pred, FalseBB, FinalBits(LHS));
                break;
            default:
                break;
        }

        // Preserve the common sign-bit facts without deriving them from
        // any source-level UB or array bound assumption.
        if (auto *RC = dynamic_cast<ConstInt *>(RHS)) {
            if (RC->getVal() == 0) {
                if (Cmp->getOp() == BinaryInst::L) {
                    RecordBits(LHS, Pred, TrueBB,
                               KnownBits(0, 0x80000000u));
                    RecordBits(LHS, Pred, FalseBB,
                               KnownBits(0x80000000u, 0));
                } else if (Cmp->getOp() == BinaryInst::GE) {
                    RecordBits(LHS, Pred, TrueBB,
                               KnownBits(0x80000000u, 0));
                    RecordBits(LHS, Pred, FalseBB,
                               KnownBits(0, 0x80000000u));
                }
            }
        }
    }
}

// --- 主求解入口 ---

RangeInfo RangeSolver::solve(Function &F, SCEVResult *SCEV) {
    CurFunc = &F;

    // Step 1: Forward 不动点（widening）
    // 参数保持 Top 初值（不从 cache seed，避免函数被用偏紧参数分析导致
    // 返回值低估）
    markBlockExecutable(F.front());
    runForward();

    // Step 2: Narrowing（backward constraint propagation）。
    // 跨函数 return cache 当前禁用，不发布顺序敏感 summary。
    const bool NarrowingComplete = runNarrowing(F);

    // A budget cutoff leaves a partially intersected (under-approximating)
    // lattice.  Do not expose it to transforms: dropping all facts yields
    // Top on query and is conservative, without copying every analysis map
    // on the normal completed path.
    if (!NarrowingComplete) {
        ValueRanges.clear();
        FloatValueRanges.clear();
        ValueBits.clear();
        BlockLocalRanges.clear();
    }

    // Branch/GEP contextual facts derived while the lattice was changing
    // are never exposed.  Rebuild integer branch facts once from the
    // current CFG and final global lattice.  This naturally includes newly
    // cloned/peeled blocks whenever RangeAnalysis is recomputed after the
    // CFG-transform invalidation.
    ContextualRanges.clear();
    FloatContextualRanges.clear();
    ContextualBits.clear();
    EdgeRanges.clear();
    EdgeBits.clear();
    if (NarrowingComplete)
        rebuildIntegerContextualFacts(F);

    // Step 4: 用 SCEV 证明出的有限归纳变量范围补充循环体上下文信息。
    // 只写 non-header loop blocks，保留 header/exit 判断的保守性。
    if (NarrowingComplete)
        applySCEVContextHints(SCEV);

    // Step 5: 构建结果
    RangeInfo info;
    info.GlobalRanges = std::move(ValueRanges);
    info.ContextualRanges = std::move(ContextualRanges);
    info.EdgeRanges = std::move(EdgeRanges);
    info.BlockLocalRanges = std::move(BlockLocalRanges);
    info.FloatGlobalRanges = std::move(FloatValueRanges);
    info.FloatContextualRanges = std::move(FloatContextualRanges);
    info.GlobalBits = std::move(ValueBits);
    info.ContextualBits = std::move(ContextualBits);
    info.EdgeBits = std::move(EdgeBits);
    if (ScevInductions)
        info.LoopInductions = *ScevInductions;
    return info;
}

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
