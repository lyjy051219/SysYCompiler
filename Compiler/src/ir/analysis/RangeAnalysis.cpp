#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/SCEV.hpp"
#include "lib/IRUtils.hpp"

#include <queue>
#include <algorithm>
#include <cstdlib>
#include <limits>

// 整数格运算在数学结果可能越过 i32 或无法用单区间表示时返回 Top，绝不使用饱和端点。
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

namespace {

bool isModeledIntegerRangeType(const Value *V) {
    auto Ty = V ? V->getIRType() : nullptr;
    return IRType::hasIntegerBitWidth(Ty, 1) ||
           IRType::hasIntegerBitWidth(Ty, 32);
}

IntRange conservativeIntegerRangeForValue(const Value *V) {
    if (!V || !V->getIRType() || !V->getIRType()->isInt())
        return IntRange();
    if (V->getIRType()->hasIntegerBitWidth(1))
        return IntRange(0, 1);

    return IntRange();
}

}

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

    int64_t v1 = min * rhs.min;
    int64_t v2 = min * rhs.max;
    int64_t v3 = max * rhs.min;
    int64_t v4 = max * rhs.max;
    int64_t lo = std::min({v1, v2, v3, v4});
    int64_t hi = std::max({v1, v2, v3, v4});
    if (lo < I32_MIN || hi > I32_MAX) return IntRange();
    IntRange r(lo, hi);

    int tz = knownTrailingZeros + rhs.knownTrailingZeros;
    r.knownTrailingZeros = std::min(tz, 31);
    return r;
}

IntRange IntRange::sdiv(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();

    if (rhs.min <= 0 && rhs.max >= 0) return IntRange();

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

    int64_t absMax = std::max(std::abs(rhs.min), std::abs(rhs.max)) - 1;
    if (min >= 0) return IntRange(0, std::min(max, absMax));
    if (max <= 0) return IntRange(std::max(min, -absMax), 0);
    return IntRange(-absMax, absMax);
}

IntRange IntRange::shl(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (!rhs.isSingleValue()) return IntRange();
    int64_t k = rhs.min;
    if (k < 0 || k >= 31) return IntRange();

    if (min < 0) return IntRange();

    uint64_t mul = (uint64_t)1 << k;
    uint64_t u1 = (uint64_t)min * mul;
    uint64_t u2 = (uint64_t)max * mul;

    if (u1 > (uint64_t)I32_MAX || u2 > (uint64_t)I32_MAX) return IntRange();
    IntRange r((int64_t)u1, (int64_t)u2);

    int tz = knownTrailingZeros + (int)k;
    r.knownTrailingZeros = std::min(tz, 31);
    return r;
}

IntRange IntRange::ashr(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (!rhs.isSingleValue()) return IntRange();
    int64_t k = rhs.min;
    if (k < 0 || k >= 32) return IntRange();

    int64_t v1 = min >> k;
    int64_t v2 = max >> k;
    return IntRange(std::min(v1, v2), std::max(v1, v2));
}

IntRange IntRange::lshr(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (!rhs.isSingleValue()) return IntRange();
    int64_t k = rhs.min;
    if (k < 0 || k >= 32) return IntRange();

    if (min < 0) return IntRange();
    int64_t v1 = (uint32_t)min >> k;
    int64_t v2 = (uint32_t)max >> k;
    return IntRange(std::min<int64_t>(v1, v2), std::max<int64_t>(v1, v2));
}

IntRange IntRange::band(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();

    int tz = std::max(knownTrailingZeros, rhs.knownTrailingZeros);

    if (min >= 0 && rhs.min >= 0) {
        IntRange r(0, std::min(max, rhs.max));
        r.knownTrailingZeros = std::min(tz, 31);
        return r;
    }

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
    IntRange r;
    r.knownTrailingZeros = std::min(tz, 31);
    return r;
}

// KnownBits 传播只保留逐位可证明事实，冲突表示不可达并在更新阶段被保守忽略。
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
    uint32_t unsignedMin = one;
    uint32_t unsignedMax = one | unknown;

    bool signKnown0 = (zero & 0x80000000u) != 0;
    bool signKnown1 = (one  & 0x80000000u) != 0;

    if (signKnown0) {

        return IntRange((int64_t)unsignedMin, (int64_t)unsignedMax);
    }
    if (signKnown1) {

        int64_t lo = (int32_t)unsignedMin;
        int64_t hi = (int32_t)unsignedMax;

        return IntRange(std::min(lo, hi), std::max(lo, hi));
    }

    IntRange r;
    r.knownTrailingZeros = getTrailingZeros();
    return r;
}

IntRange IntRange::bor(const IntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();

    int tz = std::min(knownTrailingZeros, rhs.knownTrailingZeros);

    if (min >= 0 && rhs.min >= 0) {
        IntRange r(std::max(min, rhs.min), I32_MAX);
        r.knownTrailingZeros = tz;
        return r;
    }
    IntRange r;
    r.knownTrailingZeros = tz;
    return r;
}

// 浮点格以 f32 执行端点运算，再用 double 保存包络，并独立传播 NaN、无穷和零符号。
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

    if (isExactNumericValue() && rhs.isExactNumericValue()) {
        float V = exactF32Value(*this) / exactF32Value(rhs);
        FloatRange Exact(static_cast<double>(V));
        Exact.mayNaN = Exact.mayNaN || MayNaN;
        return Exact;
    }

    bool DenominatorTouchesZero = rhs.hasNumericValues() &&
                                  rhs.min <= 0.0 && rhs.max >= 0.0;
    if (DenominatorTouchesZero) {
        FloatRange R(F_MIN_INF(), F_MAX_INF());
        R.mayNaN = MayNaN;
        return R;
    }

    FloatRange R = evaluateFloatEndpoints(
        *this, rhs, MayNaN, [](float A, float B) { return A / B; });

    bool NumeratorHasFinite = hasNumericValues() &&
                              min < F_MAX_INF() && max > F_MIN_INF();
    if (NumeratorHasFinite && rhs.mayContainInfinity())
        R = R.unionWith(FloatRange(0.0, 0.0));
    return R;
}

LoopInductionInfo::LoopInductionInfo()
    : RecLoop(nullptr), initial(0), step(0), hasUpperBound(false),
      upperBound(0), noWrap(false) {}

void RangeInfo::setLoopInduction(Value *v,
                                 const LoopInductionInfo &info) {
    LoopInductions[v] = info;
}

// 整数查询流程：常量或不支持位宽先处理，再将全局范围与支配链、精确块提示依次取交。
IntRange RangeInfo::query(Value *val, BasicBlock *ctxBlock,
                          const DominatorTree &DT) const {

    // 1. 常量可直接得到单点范围；未建模的整数位宽退化为该类型的完整范围。
    if (auto *CI = dynamic_cast<ConstInt *>(val))
        return IntRange(CI->getVal());
    if (!isModeledIntegerRangeType(val))
        return conservativeIntegerRangeForValue(val);

    // 2. 以固定点求出的全局范围为基线；没有结果时保持类型完整范围，不凭空制造精度。
    IntRange range = conservativeIntegerRangeForValue(val);
    auto gIt = GlobalRanges.find(val);
    if (gIt != GlobalRanges.end())
        range = gIt->second;

    // 3. 查询点存在时，沿有限深度 IDom 链叠加支配事实，最后再叠加当前块精确提示。
    if (ctxBlock) {
        auto cIt = ContextualRanges.find(val);
        if (cIt != ContextualRanges.end()) {
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

// 边查询以 predecessor 上下文为基线，再叠加最终 CFG 重建出的特定 pred->succ 事实。
IntRange RangeInfo::queryOnEdge(Value *val, BasicBlock *predBB,
                                 BasicBlock *succBB,
                                 const DominatorTree &DT) const {
    if (auto *CI = dynamic_cast<ConstInt *>(val)) return IntRange(CI->getVal());
    if (!isModeledIntegerRangeType(val))
        return conservativeIntegerRangeForValue(val);
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

KnownBits RangeInfo::queryBits(Value *val) const {
    if (!val ||
        !IRType::hasIntegerBitWidth(val->getIRType(), 32))
        return KnownBits::top();
    if (auto *CI = dynamic_cast<ConstInt *>(val))
        return KnownBits::fromConst(CI->getVal());
    auto it = GlobalBits.find(val);
    if (it != GlobalBits.end()) return it->second;
    return KnownBits::top();
}

KnownBits RangeInfo::queryBits(Value *val, BasicBlock *ctxBlock,
                                const DominatorTree &DT) const {
    if (!val ||
        !IRType::hasIntegerBitWidth(val->getIRType(), 32))
        return KnownBits::top();
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
    if (!val ||
        !IRType::hasIntegerBitWidth(val->getIRType(), 32))
        return KnownBits::top();
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

// 求解器内部未见过的值按类型初始化为 Top；i1 使用完整布尔域。
IntRange &RangeSolver::getRange(Value *V) {

    auto It = ValueRanges.find(V);
    if (It == ValueRanges.end())
        It = ValueRanges.emplace(
            V, conservativeIntegerRangeForValue(V)).first;
    return It->second;
}

KnownBits RangeSolver::getBits(Value *V) {
    if (!V ||
        !IRType::hasIntegerBitWidth(V->getIRType(), 32))
        return KnownBits::top();
    if (auto *CI = dynamic_cast<ConstInt *>(V))
        return KnownBits::fromConst(CI->getVal());
    return ValueBits[V];
}

IntRange RangeSolver::getRangeInBlock(Value *V, BasicBlock *BB) {
    if (!isModeledIntegerRangeType(V))
        return conservativeIntegerRangeForValue(V);
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
    if (!V ||
        !IRType::hasIntegerBitWidth(V->getIRType(), 32))
        return KnownBits::top();
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

void RangeSolver::enqueueInstructionUsers(Value *V) {
    if (!V) return;
    for (Instruction *User : V->getInstructionUsers())
        InstWorkList.push_back(User);
}

// KnownBits 更新与区间更新共享用户工作队列，使两个格可以互相收紧并共同收敛。
bool RangeSolver::updateBits(Value *V, const KnownBits &newBits) {
    if (!V ||
        !IRType::hasIntegerBitWidth(V->getIRType(), 32))
        return false;
    auto &cur = ValueBits[V];
    KnownBits merged = NarrowingMode ? cur.intersectWith(newBits) : newBits;
    if (merged.isConflict()) return false;
    if (merged == cur) return false;
    cur = merged;
    enqueueInstructionUsers(V);
    return true;
}

void RangeSolver::updateBitsContextual(Value *V, BasicBlock *block,
                                   const KnownBits &kb) {
    auto it = ContextualBits[V].find(block);

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

IntRange RangeSolver::tightenWithBits(Value *V, const IntRange &newRange) {
    KnownBits kb = getBits(V);
    if (kb.isTop()) return newRange;
    IntRange fromKB = kb.toIntRange();
    if (fromKB.isBottom()) return IntRange::bottom();
    return newRange.intersectWith(fromKB);
}

// 核心更新不变量：前向阶段超过次数预算即锁为 Top，narrowing 阶段只取交且不引入 Bottom。
bool RangeSolver::updateRange(Value *V, const IntRange &newRange) {
    // 1. 未建模位宽只允许保存类型完整范围，防止 i32 规则错误套用到其他整数类型。
    if (!isModeledIntegerRangeType(V)) {
        IntRange Full = conservativeIntegerRangeForValue(V);
        auto It = ValueRanges.find(V);
        if (It != ValueRanges.end() && It->second == Full)
            return false;
        ValueRanges[V] = Full;
        return true;
    }

    // 2. Narrowing 只把旧值与新值取交；冲突产生 Bottom 时拒绝更新，避免不安全的欠近似。
    if (NarrowingMode) {
        auto &cur = getRange(V);
        IntRange intersected = cur.intersectWith(newRange);

        if (intersected.isBottom()) return false;
        if (intersected == cur) return false;
        cur = intersected;
        enqueueInstructionUsers(V);
        return true;
    }

    // 3. 前向阶段先正常覆盖；达到更新预算后一次性扩大为 Top，并锁定该值保证终止。
    if (Widened.count(V)) return false;

    auto &cur = getRange(V);
    if (newRange == cur) return false;

    int &vCount = EnqueueCount[V];
    vCount++;
    if (vCount >= MAX_ENQUEUE) {

        cur = IntRange();
        Widened.insert(V);
    } else {
        cur = newRange;
    }

    enqueueInstructionUsers(V);
    return true;
}

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

void RangeSolver::updateContextual(Value *V, BasicBlock *block,
                               const IntRange &range) {
    auto it = ContextualRanges[V].find(block);

    if (range.isTop()) {

        if (it == ContextualRanges[V].end()) return;
        ContextualRanges[V].erase(it);

        enqueueInstructionUsers(V);
        return;
    }

    IntRange &cur = ContextualRanges[V][block];
    if (range == cur) return;
    cur = range;
    enqueueInstructionUsers(V);
}

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

// 新可行边会激活目标块；若块已执行，则只重排 PHI 以合并新增 incoming。
bool RangeSolver::markEdgeFeasible(BasicBlock *From, BasicBlock *To) {
    if (!FeasibleEdges.insert({From, To}).second)
        return false;
    if (ExecutableBlocks.count(To)) {

        for (Instruction *I : *To) {
            if (!dynamic_cast<PhiInst *>(I)) break;
            InstWorkList.push_back(I);
        }
    } else {
        markBlockExecutable(To);
    }
    return true;
}

IntRange RangeSolver::rangeFromInduction(
const LoopInductionInfo &info) const {
    if (!info.hasUpperBound || !info.noWrap) return IntRange();
    if (info.step > 0)
        return IntRange(info.initial, info.upperBound);
    if (info.step < 0)
        return IntRange(info.upperBound, info.initial);
    return IntRange(info.initial);
}

// SCEV 提示只写入非 Header、非退出的活动循环体块，不能覆盖 PHI 的全局范围。
void RangeSolver::applySCEVContextHints(SCEVResult *SCEV) {
    if (!SCEV) return;

    const auto &inductions = SCEV->getLoopInductions();
    ScevInductions = &inductions;

    for (const auto &entry : inductions) {
        Value *V = entry.first;
        const LoopInductionInfo &info = entry.second;
        if (!V || !info.RecLoop) continue;
        if (!info.noWrap) continue;
        if (!V->getIRType() || !V->getIRType()->isInt()) continue;

        IntRange activeRange = rangeFromInduction(info);
        if (activeRange.isTop() || activeRange.isBottom()) continue;
        if (activeRange.min < IntRange::I32_MIN ||
            activeRange.max > IntRange::I32_MAX)
            continue;

        BasicBlock *header = info.RecLoop->getHeader();
        for (BasicBlock *BB : info.RecLoop->getBlocksIncludingSubLoops()) {

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

// PHI 传递只合并可行入边；浮点取包络，整数同时汇合区间和 KnownBits。
void RangeSolver::visitPhi(PhiInst *Phi) {

    // 1. 浮点 PHI 对可行入边取包络；未建模整数 PHI 直接回退为保守格值。
    auto phiTy = Phi->getIRType();
    if (phiTy && phiTy->isFloat()) {
        FloatRange fresult = FloatRange::bottom();
        BasicBlock *phiBB = Phi->getParent();
        forEachPhiIncoming(
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

        updateFloatRange(Phi, fresult);
        return;
    }
    if (phiTy && phiTy->isInt() &&
        !isModeledIntegerRangeType(Phi)) {
        updateBits(Phi, KnownBits::top());
        updateRange(Phi, conservativeIntegerRangeForValue(Phi));
        return;
    }

    // 2. 若 SCEV 已识别归纳变量，优先使用其初值、步长和边界生成更精确范围。
    if (ScevInductions) {
        auto it = ScevInductions->find(Phi);
        if (it != ScevInductions->end()) {
            updateRange(Phi, rangeFromInduction(it->second));
            return;
        }
    }

    // 3. 普通整数 PHI 只汇合可行入边，同时合并区间与 KnownBits，再用位信息收紧区间。
    IntRange result = IntRange::bottom();
    KnownBits kbResult;
    bool firstKB = true;
    BasicBlock *phiBB = Phi->getParent();
    forEachPhiIncoming(
        Phi, [this, phiBB, &result, &kbResult, &firstKB](
                 unsigned, Value *inVal, BasicBlock *pred) {
        if (!FeasibleEdges.count({pred, phiBB})) return;
        IntRange inRange;
        KnownBits inKB;
        if (auto *CI = dynamic_cast<ConstInt *>(inVal)) {
            inRange = IntRange(CI->getVal());
            inKB = KnownBits::fromConst(CI->getVal());
        } else {

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

// 二元指令先按类型前向计算结果格；整数 KnownBits 并行传播，narrowing 时再反推 ADD/SUB 操作数。
void RangeSolver::visitBinary(BinaryInst *BI) {
    // 1. 解析两个操作数；形态不完整时无法建立可靠传递函数，直接保守退出。
    Value *lhs = nullptr;
    Value *rhs = nullptr;
    if (!BI->getOperands(lhs, rhs)) return;

    // 2. 浮点运算只做前向区间计算；除四则运算外回退为 Top，不参与整数反推。
    auto resultTy = BI->getIRType();
    if (resultTy && resultTy->isFloat()) {
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

        updateFloatRange(BI, fresult);

        return;
    }

    // 3. 比较结果固定在 [0, 1]；不支持位宽则保留完整类型范围和未知位信息。
    if (BinaryInst::isCompareOp(BI->getOp())) {
        updateBits(BI, KnownBits::top());
        updateRange(BI, IntRange(0, 1));
        return;
    }
    if (resultTy && resultTy->isInt() &&
        !isModeledIntegerRangeType(BI)) {
        updateBits(BI, KnownBits::top());
        updateRange(BI, conservativeIntegerRangeForValue(BI));
        return;
    }

    // 4. 使用操作数的全局格值做前向传递，避免把某一分支的上下文事实泄漏到其他路径。
    IntRange lRange = dynamic_cast<ConstInt *>(lhs) ?
        IntRange(dynamic_cast<ConstInt *>(lhs)->getVal()) : getRange(lhs);
    IntRange rRange = dynamic_cast<ConstInt *>(rhs) ?
        IntRange(dynamic_cast<ConstInt *>(rhs)->getVal()) : getRange(rhs);

    const int ResultBitWidth = resultTy && resultTy->isInt()
        ? resultTy->getIntegerBitWidth(0)
        : 0;
    const bool IsModeledI32 = ResultBitWidth == 32;
    auto HasModeledShiftAmount = [&]() {
        if (!rRange.isSingleValue() || rRange.min < 0 ||
            rRange.min >= ResultBitWidth)
            return false;

        return IsModeledI32 || rRange.min == 0;
    };

    IntRange result;
    switch (BI->getOp()) {
        case BinaryInst::ADD:  result = IsModeledI32 ? lRange.add(rRange) : IntRange();  break;
        case BinaryInst::SUB:  result = IsModeledI32 ? lRange.sub(rRange) : IntRange();  break;
        case BinaryInst::MUL:  result = IsModeledI32 ? lRange.mul(rRange) : IntRange();  break;
        case BinaryInst::DIV:  result = IsModeledI32 ? lRange.sdiv(rRange) : IntRange(); break;
        case BinaryInst::MOD:  result = IsModeledI32 ? lRange.srem(rRange) : IntRange(); break;

        case BinaryInst::SHL:  result = HasModeledShiftAmount() ? lRange.shl(rRange) : IntRange();  break;
        case BinaryInst::ASHR: result = HasModeledShiftAmount() ? lRange.ashr(rRange) : IntRange(); break;
        case BinaryInst::LSHR: result = HasModeledShiftAmount() ? lRange.lshr(rRange) : IntRange(); break;
        case BinaryInst::AND:  result = lRange.band(rRange); break;
        case BinaryInst::OR:   result = lRange.bor(rRange);  break;

        default: result = IntRange(); break;
    }

    // 5. KnownBits 与区间并行传播；位传递完成后再把确定的位模式投影回结果区间。
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

    result = tightenWithBits(BI, result);
    updateRange(BI, result);

    // 6. 仅在 Narrowing 且结果为已建模 i32 时反推 ADD/SUB 操作数，其他运算保持保守。
    if (NarrowingMode && IsModeledI32) {
        narrowBinaryBackward(BI, lhs, rhs, lRange, rRange);
    }
}

// 反向收窄仅处理已证明前向运算不回绕的 i32 ADD/SUB；其他算术保持保守。
void RangeSolver::narrowBinaryBackward(
BinaryInst *BI, Value *lhs, Value *rhs, const IntRange &lRange,
const IntRange &rRange) {
    auto ty = BI->getIRType();
    if (!ty) return;

    if (ty->isInt()) {
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

            default: break;
        }
        return;
    }

}

// 分支访问先标记可行边，再为单前驱后继生成临时块约束；多前驱块不得继承某一条边事实。
void RangeSolver::visitBranch(BrInst *Br) {
    // 1. 无条件分支只登记唯一边；条件分支取得两侧目标和当前比较。
    if (!Br->isConditional()) {
        auto *dest = Br->getTrueSuccessor();
        BasicBlock *fromBB = Br->getParent();
        if (dest) markEdgeFeasible(fromBB, dest);
        return;
    }

    Value *cond = Br->getCondition();
    auto *trueBB = Br->getTrueSuccessor();
    auto *falseBB = Br->getFalseSuccessor();
    BasicBlock *fromBB = Br->getParent();

    if (!trueBB || !falseBB) return;

    // 2. 只有唯一入边目标可在求解期写块上下文，浮点含 NaN 时放弃数值收窄。
    bool trueSinglePred = (trueBB->getPredecessors().size() == 1);
    bool falseSinglePred = (falseBB->getPredecessors().size() == 1);

    if ((trueSinglePred || falseSinglePred) &&
        (dynamic_cast<BinaryInst *>(cond) != nullptr)) {
        auto *cmpInst = dynamic_cast<BinaryInst *>(cond);
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        if (cmpInst->getOperands(lhs, rhs)) {
            auto op = cmpInst->getOp();

            auto lhsTy = lhs->getIRType();
            auto rhsTy = rhs->getIRType();
            if (lhsTy && rhsTy &&
                lhsTy->isFloat() && rhsTy->isFloat()) {
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

            // 3. 整数比较生成真假两侧区间，并为等值和符号测试补充 KnownBits。
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

            else if (op == BinaryInst::E) {
                if (trueSinglePred) {
                    updateContextual(lhs, trueBB, rRange);
                    updateContextual(rhs, trueBB, lRange);
                }
            }

            auto applyEqKB = [&](Value *a, Value *b, BasicBlock *succ, bool predOnSucc){
                if (!predOnSucc) return;

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

    // 4. 无论能否收窄，两个条件后继都加入可行图并驱动后续固定点。
    markEdgeFeasible(fromBB, trueBB);
    markEdgeFeasible(fromBB, falseBB);
}

// 调用结果只精确建模已知输入内建；用户函数和未知被调目标因缺少可靠跨函数失效机制返回 Top。
void RangeSolver::visitCall(CallInst *CI) {
    Value *callee = CI->getCallee();
    if (!callee) return;

    auto SetTopForResultType = [&]() {
        auto Ty = CI->getIRType();
        if (!Ty) return;
        if (Ty->isInt()) updateRange(CI, IntRange());
        else if (Ty->isFloat())
            updateFloatRange(CI, FloatRange());
    };

    if (auto *BIF = dynamic_cast<BuildInFunction *>(callee)) {
        std::string name = BIF->getIdent();
        if (name == "getch") {

            updateRange(CI, IntRange(-128, 127));
            return;
        }
        if (name == "getint") {

            updateRange(CI, IntRange());
            return;
        }

        SetTopForResultType();
        return;
    }

    if (dynamic_cast<Function *>(callee)) {
        SetTopForResultType();
        return;
    }

    SetTopForResultType();
}

// Select 不建立 CFG 上下文，只对两臂的整数/浮点结果取并集。
void RangeSolver::visitSelect(SelectInst *SI) {
    Value *tv = SI->getTrueValue();
    Value *fv = SI->getFalseValue();
    if (!tv || !fv) return;
    auto ty = SI->getIRType();
    if (!ty) return;

    if (ty->isInt()) {
        if (!isModeledIntegerRangeType(SI)) {
            updateRange(SI, conservativeIntegerRangeForValue(SI));
            updateBits(SI, KnownBits::top());
            return;
        }
        IntRange tr = dynamic_cast<ConstInt *>(tv) ?
            IntRange(dynamic_cast<ConstInt *>(tv)->getVal()) : getRange(tv);
        IntRange fr = dynamic_cast<ConstInt *>(fv) ?
            IntRange(dynamic_cast<ConstInt *>(fv)->getVal()) : getRange(fv);
        updateRange(SI, tr.unionWith(fr));
    } else if (ty->isFloat()) {
        FloatRange tr = dynamic_cast<ConstFloat *>(tv) ?
            FloatRange((double)dynamic_cast<ConstFloat *>(tv)->getVal()) :
            FloatValueRanges[tv];
        FloatRange fr = dynamic_cast<ConstFloat *>(fv) ?
            FloatRange((double)dynamic_cast<ConstFloat *>(fv)->getVal()) :
            FloatValueRanges[fv];
        updateFloatRange(SI, tr.unionWith(fr));
    }
}

// Cast 按源/目标位宽和浮点异常语义分类；未完整建模或可能越界的转换返回 Top。
void RangeSolver::visitCast(Instruction *I) {
    Value *src = I->getUnaryOperand();
    if (!src || !src->getIRType() || !I->getIRType()) return;
    auto srcTy = src->getIRType()->getTypeSystem();
    auto dstTy = I->getIRType()->getTypeSystem();
    const int SourceBits = src->getIRType()->isInt()
                               ? src->getIRType()->getIntegerBitWidth(0)
                               : 0;
    const int DestinationBits = I->getIRType()->isInt()
                                    ? I->getIRType()
                                          ->getIntegerBitWidth(0)
                                    : 0;

    if (dynamic_cast<ZextInst *>(I)) {

        if (srcTy == IR_INT && SourceBits == 1 &&
            DestinationBits == 32) {
            updateRange(I, IntRange(0, 1));

            updateBits(I, KnownBits(0xFFFFFFFEu, 0));
        } else if (dstTy == IR_INT) {
            updateRange(I, conservativeIntegerRangeForValue(I));
            updateBits(I, KnownBits::top());
        }
        return;
    }
    if (dynamic_cast<SextInst *>(I)) {

        if (srcTy == IR_INT && SourceBits == 1 &&
            DestinationBits == 32) {
            updateRange(I, IntRange(-1, 0));

        } else if (dstTy == IR_INT) {
            updateRange(I, conservativeIntegerRangeForValue(I));
            updateBits(I, KnownBits::top());
        }
        return;
    }
    if (dynamic_cast<TruncInst *>(I)) {

        if (dstTy == IR_INT && DestinationBits == 1) {
            updateRange(I, IntRange(0, 1));

            KnownBits srcKB = getBits(src);
            uint32_t z = 0xFFFFFFFEu;
            uint32_t o = 0;
            if (srcKB.zero & 1u) z |= 1u;
            if (srcKB.one  & 1u) o |= 1u;
            updateBits(I, KnownBits(z, o));
        } else if (dstTy == IR_INT) {
            updateRange(I, conservativeIntegerRangeForValue(I));
            updateBits(I, KnownBits::top());
        }
        return;
    }
    if (dynamic_cast<SITOFPInst *>(I)) {

        IntRange r = dynamic_cast<ConstInt *>(src) ?
            IntRange(dynamic_cast<ConstInt *>(src)->getVal()) : getRange(src);
        if (r.isBottom()) { updateFloatRange(I, FloatRange::bottom()); return; }

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
        if (DestinationBits != 32) {
            updateRange(I, conservativeIntegerRangeForValue(I));
            updateBits(I, KnownBits::top());
            return;
        }

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

        updateRange(I, IntRange());
    }

}

// 1. 前向固定点优先处理值变化产生的指令队列，再扫描首次变为可达的基本块。
void RangeSolver::runForward() {
    while (!BBWorkList.empty() || !InstWorkList.empty()) {

        while (!InstWorkList.empty()) {
            Instruction *I = InstWorkList.back();
            InstWorkList.pop_back();
            if (!ExecutableBlocks.count(
                    I->getParent()))
                continue;
            visitInstruction(I);
        }

        if (!BBWorkList.empty()) {
            BasicBlock *BB = BBWorkList.back();
            BBWorkList.pop_back();
            for (Instruction *I : *BB) {
                visitInstruction(I);
            }
        }
    }
}

// 2. Narrowing 把更新切为只取交模式，并在 O(指令数) 预算内重访所有可达指令。
bool RangeSolver::runNarrowing(Function &F) {
    // 2.1 切换到只取交模式，并按可达指令数设置有限预算，保证第二阶段必然终止。
    NarrowingMode = true;

    size_t instCount = 0;
    for (BasicBlock *BB : F) {
        if (ExecutableBlocks.count(BB)) {
            for (Instruction *I : *BB) { (void)I; instCount++; }
        }
    }
    size_t budget = instCount * 4 + 64;

    // 2.2 将所有可达指令重新入队，使 widening 丢失的精度有机会从最终前向格中恢复。
    for (BasicBlock *BB : F) {
        if (!ExecutableBlocks.count(BB)) continue;
        for (Instruction *I : *BB) {
            InstWorkList.push_back(I);
        }
    }

    // 2.3 驱动队列直到稳定或预算耗尽；未完成会由调用者识别并放弃依赖欠近似的结果。
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

// 3. 清除求解期临时约束，从最终全局格和当前 CFG 单次重建整数块事实与边事实。
void RangeSolver::rebuildIntegerContextualFacts(Function &F) {
    // 3.1 丢弃求解期随可行边增量生成的临时事实，避免残留过时 CFG 约束。
    ContextualRanges.clear();
    ContextualBits.clear();
    EdgeRanges.clear();
    EdgeBits.clear();

    // 3.2 后续闭包统一读取最终全局格，并只记录非 Top、非冲突且可安全落块的事实。
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

    // 3.3 扫描当前 CFG 的条件边，从最终比较操作数重新导出真假边范围与 KnownBits。
    for (BasicBlock *Pred : F) {
        if (!Pred) continue;
        auto *Br = Pred->getBranchTerminator();
        if (!Br || !Br->isConditional()) continue;
        BasicBlock *TrueBB = Br->getTrueSuccessor();
        BasicBlock *FalseBB = Br->getFalseSuccessor();

        if (!TrueBB || !FalseBB || TrueBB == FalseBB) continue;

        auto *Cmp = dynamic_cast<BinaryInst *>(Br->getCondition());
        if (!Cmp) continue;
        Value *LHS = nullptr;
        Value *RHS = nullptr;
        if (!Cmp->getOperands(LHS, RHS) || !LHS || !RHS ||
            !LHS->getIRType() || !RHS->getIRType() ||
            !LHS->getIRType()->isInt() ||
            !RHS->getIRType()->isInt())
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

// RangeAnalysis 主求解流程严格分阶段运行；任何 narrowing 截断都会丢弃部分下近似。
RangeInfo RangeSolver::solve(Function &F, SCEVResult *SCEV) {
    // 1. 从入口启动可达性与前向传播，更新次数预算保证循环最终扩宽收敛。
    CurFunc = &F;

    markBlockExecutable(F.front());
    runForward();

    // 2. 在最终前向格上执行反向收窄；预算耗尽时清空值事实，查询自然退化为 Top。
    const bool NarrowingComplete = runNarrowing(F);

    if (!NarrowingComplete) {
        ValueRanges.clear();
        FloatValueRanges.clear();
        ValueBits.clear();
        BlockLocalRanges.clear();
    }

    // 3. 丢弃求解期上下文，并仅在 narrowing 完成后从最终 CFG 重建路径事实。
    ContextualRanges.clear();
    FloatContextualRanges.clear();
    ContextualBits.clear();
    EdgeRanges.clear();
    EdgeBits.clear();
    if (NarrowingComplete)
        rebuildIntegerContextualFacts(F);

    // 4. 叠加 SCEV 已证明有限且不回绕的循环体局部归纳范围。
    if (NarrowingComplete)
        applySCEVContextHints(SCEV);

    // 5. 移交各格、上下文和归纳摘要，形成只读分析结果。
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

PassID RangeAnalysis::Key;

// Pass 入口取得 SCEV 后创建一次性求解器，并返回封装好的范围信息。
RangeAnalysis::Result RangeAnalysis::run(Function &F,
                                          FunctionAnalysisPassManager &FAM) const {
    // 1. 获取循环次数与归纳范围提示所需的 SCEV 结果。
    auto &SCEV = FAM.getResult<SCEVAnalysis>(F);
    // 2. 创建一次性求解器，运行五阶段流程并返回只读范围结果。
    RangeSolver solver;
    return solver.solve(F, &SCEV);
}
