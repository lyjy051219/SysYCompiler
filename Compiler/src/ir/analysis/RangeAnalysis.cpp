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
    // 二补码值直接转成无符号位模式即可统计低位零，避免对 INT64_MIN
    // 求负触发宿主 C++ 的有符号溢出。
    uint64_t u = static_cast<uint64_t>(v);
    int n = 0;
    while (n < 31 && ((u >> n) & 1ULL) == 0) n++;
    return n;
}

IntRange::IntRange()
    : min(I32_MIN), max(I32_MAX), knownTrailingZeros(0) {}

IntRange::IntRange(int64_t val)
    : min(val), max(val), knownTrailingZeros(trailingZerosOf(val)) {}

IntRange::IntRange(int64_t lo, int64_t hi)
    : min(lo), max(hi), knownTrailingZeros(0) {}

static bool isModeledIntegerRangeType(const Value *V) {
    auto Ty = V ? V->getIRType() : nullptr;
    return IRType::hasIntegerBitWidth(Ty, 1) ||
           IRType::hasIntegerBitWidth(Ty, 32);
}

static IntRange conservativeIntegerRangeForValue(const Value *V) {
    if (!V || !V->getIRType() || !V->getIRType()->isInt())
        return IntRange();
    if (V->getIRType()->hasIntegerBitWidth(1))
        return IntRange(0, 1);

    return IntRange();
}

static UIntRange conservativeUnsignedRangeForValue(const Value *V) {
    if (!V || !V->getIRType() || !V->getIRType()->isInt())
        return UIntRange();
    if (V->getIRType()->hasIntegerBitWidth(1))
        return UIntRange(0, 1);
    return UIntRange();
}

static IntRange signedRangeForConstant(const ConstInt *CI) {
    if (!CI || !CI->getIRType()) return IntRange();
    if (IRType::hasIntegerBitWidth(CI->getIRType(), 1))
        return IntRange(CI->getVal() == 0 ? 0 : 1);
    if (IRType::hasIntegerBitWidth(CI->getIRType(), 32))
        return IntRange(static_cast<int32_t>(CI->getVal()));
    // 当前整数抽象域只建模 SysY 的 i1/i32；辅助 ABI 使用的 i64 常量
    // 不能被误投影成 i32 单点。
    return conservativeIntegerRangeForValue(CI);
}

static UIntRange unsignedRangeForConstant(const ConstInt *CI) {
    if (!CI || !CI->getIRType()) return UIntRange();
    if (IRType::hasIntegerBitWidth(CI->getIRType(), 1))
        return UIntRange(CI->getVal() == 0 ? 0 : 1);
    if (IRType::hasIntegerBitWidth(CI->getIRType(), 32))
        return UIntRange(static_cast<uint32_t>(
            static_cast<int32_t>(CI->getVal())));
    return conservativeUnsignedRangeForValue(CI);
}

// ConstBool 与 ConstInt 属于两套驻留常量类型。RangeAnalysis 在入口统一
// 归一化二者，避免布尔常量在 PHI、分支和 Select 中退化成 [0,1]。
static bool getIntegerConstantRanges(const Value *V,
                                     IntRange &Signed,
                                     UIntRange &Unsigned) {
    if (auto *CB = dynamic_cast<const ConstBool *>(V)) {
        uint64_t Bit = CB->getVal() ? 1 : 0;
        Signed = IntRange(static_cast<int64_t>(Bit));
        Unsigned = UIntRange(Bit);
        return true;
    }
    if (auto *CI = dynamic_cast<const ConstInt *>(V)) {
        Signed = signedRangeForConstant(CI);
        Unsigned = unsignedRangeForConstant(CI);
        return true;
    }
    return false;
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

    // i32 的 INT_MIN % -1 在 LLVM/C 整数语义下不能作为普通确定值使用。
    // 只要两个区间仍允许这组操作数，就保守返回 Top，禁止后续优化借此
    // 折叠控制流或利用源程序中的未定义行为。
    if (min <= I32_MIN && max >= I32_MIN &&
        rhs.min <= -1 && rhs.max >= -1)
        return IntRange();

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

// 无符号区间只覆盖 i32 位模式空间；运算结果一旦跨越 0xffffffff->0 的
// 回绕点，就无法由单个闭区间安全表示，因此统一返回 Top。
UIntRange::UIntRange() : min(U32_MIN), max(U32_MAX) {}

UIntRange::UIntRange(uint64_t val)
    : min(val <= U32_MAX ? val : U32_MIN),
      max(val <= U32_MAX ? val : U32_MAX) {}

UIntRange::UIntRange(uint64_t lo, uint64_t hi)
    : min(lo), max(hi) {
    if (lo <= hi && hi > U32_MAX) {
        min = U32_MIN;
        max = U32_MAX;
    }
}

bool UIntRange::isTop() const {
    return min == U32_MIN && max == U32_MAX;
}

bool UIntRange::isBottom() const {
    return min > max;
}

bool UIntRange::isSingleValue() const {
    return min == max && !isBottom();
}

UIntRange UIntRange::unionWith(const UIntRange &other) const {
    if (isBottom()) return other;
    if (other.isBottom()) return *this;
    return UIntRange(std::min(min, other.min), std::max(max, other.max));
}

UIntRange UIntRange::intersectWith(const UIntRange &other) const {
    return UIntRange(std::max(min, other.min), std::min(max, other.max));
}

bool UIntRange::operator==(const UIntRange &o) const {
    return min == o.min && max == o.max;
}

bool UIntRange::operator!=(const UIntRange &o) const {
    return !(*this == o);
}

UIntRange UIntRange::add(const UIntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    uint64_t lo = min + rhs.min;
    uint64_t hi = max + rhs.max;
    if (hi > U32_MAX) return UIntRange();
    return UIntRange(lo, hi);
}

UIntRange UIntRange::sub(const UIntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (min < rhs.max) return UIntRange();
    return UIntRange(min - rhs.max, max - rhs.min);
}

UIntRange UIntRange::mul(const UIntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    uint64_t hi = max * rhs.max;
    if (hi > U32_MAX) return UIntRange();
    return UIntRange(min * rhs.min, hi);
}

UIntRange UIntRange::udiv(const UIntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (rhs.min == 0) return UIntRange();
    return UIntRange(min / rhs.max, max / rhs.min);
}

UIntRange UIntRange::urem(const UIntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (rhs.min == 0) return UIntRange();
    if (max < rhs.min) return *this;
    return UIntRange(0, std::min(max, rhs.max - 1));
}

UIntRange UIntRange::shl(const UIntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (!rhs.isSingleValue() || rhs.min >= 32) return UIntRange();
    unsigned shift = static_cast<unsigned>(rhs.min);
    if (shift == 0) return *this;
    if (max > (U32_MAX >> shift)) return UIntRange();
    return UIntRange(min << shift, max << shift);
}

UIntRange UIntRange::lshr(const UIntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    if (!rhs.isSingleValue() || rhs.min >= 32) return UIntRange();
    unsigned shift = static_cast<unsigned>(rhs.min);
    return UIntRange(min >> shift, max >> shift);
}

UIntRange UIntRange::band(const UIntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    return UIntRange(0, std::min(max, rhs.max));
}

UIntRange UIntRange::bor(const UIntRange &rhs) const {
    if (isBottom() || rhs.isBottom()) return bottom();
    return UIntRange(std::max(min, rhs.min), U32_MAX);
}

IntRange UIntRange::toIntRange() const {
    if (isBottom()) return IntRange::bottom();
    if (max <= static_cast<uint64_t>(IntRange::I32_MAX))
        return IntRange(static_cast<int64_t>(min),
                        static_cast<int64_t>(max));
    if (min >= 0x80000000ULL) {
        int64_t lo = static_cast<int32_t>(static_cast<uint32_t>(min));
        int64_t hi = static_cast<int32_t>(static_cast<uint32_t>(max));
        return IntRange(lo, hi);
    }
    return IntRange();
}

UIntRange UIntRange::fromIntRange(const IntRange &range) {
    if (range.isBottom()) return bottom();
    if (range.min < IntRange::I32_MIN ||
        range.max > IntRange::I32_MAX)
        return UIntRange();
    if (range.min >= 0)
        return UIntRange(static_cast<uint64_t>(range.min),
                         static_cast<uint64_t>(range.max));
    if (range.max < 0)
        return UIntRange(
            static_cast<uint32_t>(static_cast<int32_t>(range.min)),
            static_cast<uint32_t>(static_cast<int32_t>(range.max)));
    return UIntRange();
}

UIntRange UIntRange::bottom() {
    return UIntRange(1, 0);
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

UIntRange KnownBits::toUIntRange() const {
    if (isConflict()) return UIntRange::bottom();
    uint32_t unknown = ~(zero | one);
    return UIntRange(static_cast<uint64_t>(one),
                     static_cast<uint64_t>(one | unknown));
}

KnownBits KnownBits::fromUIntRange(const UIntRange &range) {
    if (range.isBottom() || range.isTop()) return KnownBits::top();
    uint32_t lo = static_cast<uint32_t>(range.min);
    uint32_t hi = static_cast<uint32_t>(range.max);
    uint32_t different = lo ^ hi;
    uint32_t commonMask = 0xFFFFFFFFu;
    if (different != 0) {
        unsigned highestDifferent = 31u;
        while (((different >> highestDifferent) & 1u) == 0u)
            --highestDifferent;
        if (highestDifferent == 31u)
            commonMask = 0;
        else
            commonMask = ~((1u << (highestDifferent + 1u)) - 1u);
    }
    return KnownBits((~lo) & commonMask, lo & commonMask);
}

// 约化积只交换三种域都能无歧义表达的事实。符号断点两侧形成的非凸集合
// 不强行压成单区间；若任意投影产生矛盾，则把数值域标记为不可达。
void reduceIntegerRangeProduct(IntRange &signedRange,
                               UIntRange &unsignedRange,
                               KnownBits &bits) {
    if (signedRange.isBottom() || unsignedRange.isBottom() ||
        bits.isConflict()) {
        signedRange = IntRange::bottom();
        unsignedRange = UIntRange::bottom();
        bits = KnownBits::top();
        return;
    }

    for (int iteration = 0; iteration < 3; ++iteration) {
        IntRange oldSigned = signedRange;
        UIntRange oldUnsigned = unsignedRange;
        KnownBits oldBits = bits;

        IntRange signedFromBits = bits.toIntRange();
        IntRange narrowedSigned =
            signedRange.intersectWith(signedFromBits);
        if (narrowedSigned.isBottom()) {
            signedRange = IntRange::bottom();
            unsignedRange = UIntRange::bottom();
            bits = KnownBits::top();
            return;
        }
        signedRange = narrowedSigned;

        UIntRange unsignedFromBits = bits.toUIntRange();
        UIntRange narrowedUnsigned =
            unsignedRange.intersectWith(unsignedFromBits);
        if (narrowedUnsigned.isBottom()) {
            signedRange = IntRange::bottom();
            unsignedRange = UIntRange::bottom();
            bits = KnownBits::top();
            return;
        }
        unsignedRange = narrowedUnsigned;

        UIntRange unsignedFromSigned =
            UIntRange::fromIntRange(signedRange);
        if (!unsignedFromSigned.isTop()) {
            narrowedUnsigned =
                unsignedRange.intersectWith(unsignedFromSigned);
            if (narrowedUnsigned.isBottom()) {
                signedRange = IntRange::bottom();
                unsignedRange = UIntRange::bottom();
                bits = KnownBits::top();
                return;
            }
            unsignedRange = narrowedUnsigned;
        }

        IntRange signedFromUnsigned = unsignedRange.toIntRange();
        if (!signedFromUnsigned.isTop()) {
            narrowedSigned =
                signedRange.intersectWith(signedFromUnsigned);
            if (narrowedSigned.isBottom()) {
                signedRange = IntRange::bottom();
                unsignedRange = UIntRange::bottom();
                bits = KnownBits::top();
                return;
            }
            signedRange = narrowedSigned;
        }

        KnownBits intervalBits = KnownBits::fromUIntRange(unsignedRange);
        KnownBits combined(bits.zero | intervalBits.zero,
                           bits.one | intervalBits.one);
        if (combined.isConflict()) {
            signedRange = IntRange::bottom();
            unsignedRange = UIntRange::bottom();
            bits = KnownBits::top();
            return;
        }
        bits = combined;

        if (signedRange == oldSigned &&
            unsignedRange == oldUnsigned &&
            bits == oldBits)
            break;
    }
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

// 整数查询流程：常量或不支持位宽先处理，再完整遍历 IDom 链叠加路径事实，
// 最后把 signed、unsigned、KnownBits 三个域做约减。支配深度不再影响语义精度。
IntRange RangeInfo::query(Value *val, BasicBlock *ctxBlock,
                          const DominatorTree &DT) const {

    // 1. 常量可直接得到单点范围；未建模的整数位宽退化为该类型的完整范围。
    IntRange constantSigned;
    UIntRange constantUnsigned;
    if (getIntegerConstantRanges(
            val, constantSigned, constantUnsigned))
        return constantSigned;
    if (!isModeledIntegerRangeType(val))
        return conservativeIntegerRangeForValue(val);

    // 2. 以固定点求出的三个全局域为基线；缺失事实等价于对应类型的 Top。
    IntRange range = conservativeIntegerRangeForValue(val);
    UIntRange unsignedRange = conservativeUnsignedRangeForValue(val);
    KnownBits bits = KnownBits::top();
    auto gIt = GlobalRanges.find(val);
    if (gIt != GlobalRanges.end())
        range = gIt->second;
    auto uIt = GlobalUnsignedRanges.find(val);
    if (uIt != GlobalUnsignedRanges.end())
        unsignedRange = uIt->second;
    auto kIt = GlobalBits.find(val);
    if (kIt != GlobalBits.end())
        bits = kIt->second;

    // 3. 查询点存在时沿完整 IDom 链叠加支配事实。Seen 只防御损坏的支配树，
    // 不设置任意深度上限。
    if (ctxBlock) {
        auto cIt = ContextualRanges.find(val);
        auto cuIt = ContextualUnsignedRanges.find(val);
        auto ckIt = ContextualBits.find(val);
        BasicBlock *runner = ctxBlock;
        std::unordered_set<BasicBlock *> seen;
        while (runner && seen.insert(runner).second) {
            if (cIt != ContextualRanges.end()) {
                const auto &blockMap = cIt->second;
                auto bIt = blockMap.find(runner);
                if (bIt != blockMap.end())
                    range = range.intersectWith(bIt->second);
            }
            if (cuIt != ContextualUnsignedRanges.end()) {
                const auto &blockMap = cuIt->second;
                auto bIt = blockMap.find(runner);
                if (bIt != blockMap.end())
                    unsignedRange =
                        unsignedRange.intersectWith(bIt->second);
            }
            if (ckIt != ContextualBits.end()) {
                const auto &blockMap = ckIt->second;
                auto bIt = blockMap.find(runner);
                if (bIt != blockMap.end())
                    bits = bits.intersectWith(bIt->second);
            }
            runner = DT.getIDom(runner);
        }

        auto lIt = BlockLocalRanges.find(val);
        if (lIt != BlockLocalRanges.end()) {
            auto bIt = lIt->second.find(ctxBlock);
            if (bIt != lIt->second.end())
                range = range.intersectWith(bIt->second);
        }
        auto luIt = BlockLocalUnsignedRanges.find(val);
        if (luIt != BlockLocalUnsignedRanges.end()) {
            auto bIt = luIt->second.find(ctxBlock);
            if (bIt != luIt->second.end())
                unsignedRange =
                    unsignedRange.intersectWith(bIt->second);
        }
    }

    // Select 的全局固定点不读取求解期上下文；查询阶段面对的上下文已经由
    // 最终全局格重建，因此可以安全地重新计算两条虚拟臂并取并集。这样既
    // 隔离临时循环出口事实，又保留 Select 所在块的稳定支配条件精度。
    if (auto *select = dynamic_cast<SelectInst *>(val)) {
        IntRange stableSelectRange = queryOnSelectArm(
            select, select, true, DT).unionWith(
                queryOnSelectArm(select, select, false, DT));
        range = range.intersectWith(stableSelectRange);
    }

    reduceIntegerRangeProduct(range, unsignedRange, bits);
    return range;
}

IntRange RangeInfo::query(Value *val, Instruction *ctxInst,
                          const DominatorTree &DT) const {
    return query(val, ctxInst ? ctxInst->getParent() : nullptr, DT);
}

UIntRange RangeInfo::queryUnsigned(Value *val, BasicBlock *ctxBlock,
                                   const DominatorTree &DT) const {
    IntRange constantSigned;
    UIntRange constantUnsigned;
    if (getIntegerConstantRanges(
            val, constantSigned, constantUnsigned))
        return constantUnsigned;
    if (!isModeledIntegerRangeType(val))
        return conservativeUnsignedRangeForValue(val);

    IntRange signedRange = conservativeIntegerRangeForValue(val);
    UIntRange unsignedRange = conservativeUnsignedRangeForValue(val);
    KnownBits bits = KnownBits::top();

    auto sIt = GlobalRanges.find(val);
    if (sIt != GlobalRanges.end()) signedRange = sIt->second;
    auto uIt = GlobalUnsignedRanges.find(val);
    if (uIt != GlobalUnsignedRanges.end()) unsignedRange = uIt->second;
    auto kIt = GlobalBits.find(val);
    if (kIt != GlobalBits.end()) bits = kIt->second;

    if (ctxBlock) {
        auto csIt = ContextualRanges.find(val);
        auto cuIt = ContextualUnsignedRanges.find(val);
        auto ckIt = ContextualBits.find(val);
        BasicBlock *runner = ctxBlock;
        std::unordered_set<BasicBlock *> seen;
        while (runner && seen.insert(runner).second) {
            if (csIt != ContextualRanges.end()) {
                auto bIt = csIt->second.find(runner);
                if (bIt != csIt->second.end())
                    signedRange =
                        signedRange.intersectWith(bIt->second);
            }
            if (cuIt != ContextualUnsignedRanges.end()) {
                auto bIt = cuIt->second.find(runner);
                if (bIt != cuIt->second.end())
                    unsignedRange =
                        unsignedRange.intersectWith(bIt->second);
            }
            if (ckIt != ContextualBits.end()) {
                auto bIt = ckIt->second.find(runner);
                if (bIt != ckIt->second.end())
                    bits = bits.intersectWith(bIt->second);
            }
            runner = DT.getIDom(runner);
        }

        auto lsIt = BlockLocalRanges.find(val);
        if (lsIt != BlockLocalRanges.end()) {
            auto bIt = lsIt->second.find(ctxBlock);
            if (bIt != lsIt->second.end())
                signedRange =
                    signedRange.intersectWith(bIt->second);
        }
        auto luIt = BlockLocalUnsignedRanges.find(val);
        if (luIt != BlockLocalUnsignedRanges.end()) {
            auto bIt = luIt->second.find(ctxBlock);
            if (bIt != luIt->second.end())
                unsignedRange =
                    unsignedRange.intersectWith(bIt->second);
        }
    }

    // 与 signed 查询保持同一分层：只在求解完成后的只读查询中消费稳定块
    // 上下文，重新汇合 Select 两臂的 unsigned 区间。
    if (auto *select = dynamic_cast<SelectInst *>(val)) {
        UIntRange stableSelectRange = queryUnsignedOnSelectArm(
            select, select, true, DT).unionWith(
                queryUnsignedOnSelectArm(
                    select, select, false, DT));
        unsignedRange =
            unsignedRange.intersectWith(stableSelectRange);
    }

    reduceIntegerRangeProduct(signedRange, unsignedRange, bits);
    return unsignedRange;
}

UIntRange RangeInfo::queryUnsigned(Value *val, Instruction *ctxInst,
                                   const DominatorTree &DT) const {
    return queryUnsigned(
        val, ctxInst ? ctxInst->getParent() : nullptr, DT);
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
            std::unordered_set<BasicBlock *> seen;
            while (runner && seen.insert(runner).second) {
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
    IntRange constantSigned;
    UIntRange constantUnsigned;
    if (getIntegerConstantRanges(
            val, constantSigned, constantUnsigned))
        return constantSigned;
    if (!isModeledIntegerRangeType(val))
        return conservativeIntegerRangeForValue(val);
    IntRange R = query(val, predBB, DT);
    UIntRange U = queryUnsigned(val, predBB, DT);
    KnownBits KB = queryBits(val, predBB, DT);
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
    auto UVIt = EdgeUnsignedRanges.find(val);
    if (UVIt != EdgeUnsignedRanges.end()) {
        auto PIt = UVIt->second.find(predBB);
        if (PIt != UVIt->second.end()) {
            auto SIt = PIt->second.find(succBB);
            if (SIt != PIt->second.end())
                U = U.intersectWith(SIt->second);
        }
    }
    auto KVIt = EdgeBits.find(val);
    if (KVIt != EdgeBits.end()) {
        auto PIt = KVIt->second.find(predBB);
        if (PIt != KVIt->second.end()) {
            auto SIt = PIt->second.find(succBB);
            if (SIt != PIt->second.end())
                KB = KB.intersectWith(SIt->second);
        }
    }
    reduceIntegerRangeProduct(R, U, KB);
    return R;
}

UIntRange RangeInfo::queryUnsignedOnEdge(
    Value *val, BasicBlock *predBB, BasicBlock *succBB,
    const DominatorTree &DT) const {
    IntRange constantSigned;
    UIntRange constantUnsigned;
    if (getIntegerConstantRanges(
            val, constantSigned, constantUnsigned))
        return constantUnsigned;
    if (!isModeledIntegerRangeType(val))
        return conservativeUnsignedRangeForValue(val);

    IntRange R = query(val, predBB, DT);
    UIntRange U = queryUnsigned(val, predBB, DT);
    KnownBits KB = queryBits(val, predBB, DT);
    if (!predBB || !succBB) return U;

    auto SVIt = EdgeRanges.find(val);
    if (SVIt != EdgeRanges.end()) {
        auto PIt = SVIt->second.find(predBB);
        if (PIt != SVIt->second.end()) {
            auto SIt = PIt->second.find(succBB);
            if (SIt != PIt->second.end())
                R = R.intersectWith(SIt->second);
        }
    }
    auto UVIt = EdgeUnsignedRanges.find(val);
    if (UVIt != EdgeUnsignedRanges.end()) {
        auto PIt = UVIt->second.find(predBB);
        if (PIt != UVIt->second.end()) {
            auto SIt = PIt->second.find(succBB);
            if (SIt != PIt->second.end())
                U = U.intersectWith(SIt->second);
        }
    }
    auto KVIt = EdgeBits.find(val);
    if (KVIt != EdgeBits.end()) {
        auto PIt = KVIt->second.find(predBB);
        if (PIt != KVIt->second.end()) {
            auto SIt = PIt->second.find(succBB);
            if (SIt != PIt->second.end())
                KB = KB.intersectWith(SIt->second);
        }
    }
    reduceIntegerRangeProduct(R, U, KB);
    return U;
}

static IntRange constrainSignedRangeByComparison(
    Value *target, BinaryInst *compare, bool conditionIsTrue,
    const IntRange &targetRange, const IntRange &lhsRange,
    const IntRange &rhsRange) {
    if (!target || !compare) return targetRange;
    Value *lhs = nullptr;
    Value *rhs = nullptr;
    if (!compare->getOperands(lhs, rhs)) return targetRange;

    BinaryInst::Operation predicate = conditionIsTrue
        ? compare->getOp()
        : BinaryInst::negatePredicate(compare->getOp());

    // L/LE/G/GE 会被打印为有符号 icmp。i1 的位模式 1 在这种比较中代表
    // -1，而本分析为条件值保存的是逻辑域 [0,1]；两种序不能混用。因此
    // 只有 i32 才应用有符号顺序约束，i1 仍可安全使用 E/NE。
    bool isOrderingPredicate =
        predicate == BinaryInst::L ||
        predicate == BinaryInst::LE ||
        predicate == BinaryInst::G ||
        predicate == BinaryInst::GE;
    if (isOrderingPredicate &&
        (!IRType::hasIntegerBitWidth(lhs->getIRType(), 32) ||
         !IRType::hasIntegerBitWidth(rhs->getIRType(), 32)))
        return targetRange;

    IntRange other;
    if (target == lhs) {
        other = rhsRange;
    } else if (target == rhs) {
        predicate = BinaryInst::reversePredicate(predicate);
        other = lhsRange;
    } else {
        return targetRange;
    }

    IntRange constraint;
    switch (predicate) {
        case BinaryInst::L:
            constraint =
                IntRange(IntRange::I32_MIN, other.max - 1);
            break;
        case BinaryInst::LE:
            constraint =
                IntRange(IntRange::I32_MIN, other.max);
            break;
        case BinaryInst::G:
            constraint =
                IntRange(other.min + 1, IntRange::I32_MAX);
            break;
        case BinaryInst::GE:
            constraint =
                IntRange(other.min, IntRange::I32_MAX);
            break;
        case BinaryInst::E:
            constraint = other;
            break;
        case BinaryInst::NE:
        default:
            return targetRange;
    }
    return targetRange.intersectWith(constraint);
}

static bool selectArmIsExecutable(const RangeInfo &info,
                                  SelectInst *select, bool trueArm,
                                  const DominatorTree &DT) {
    if (!select || !select->getCondition()) return true;
    Value *condition = select->getCondition();
    bool constantTruth = false;
    if (condition->tryGetBoolConst(constantTruth))
        return constantTruth == trueArm;
    IntRange conditionRange =
        info.query(condition, select->getParent(), DT);
    if (!conditionRange.isSingleValue()) return true;
    return (conditionRange.min != 0) == trueArm;
}

// Select 臂查询把比较条件当成一条虚拟边，只在被选中的值或比较操作数上
// 叠加事实；该过程不创建基本块，也不修改任何 IR。
IntRange RangeInfo::queryOnSelectArm(
    Value *val, SelectInst *select, bool trueArm,
    const DominatorTree &DT) const {
    if (!select) return query(val, static_cast<BasicBlock *>(nullptr), DT);
    if (!selectArmIsExecutable(*this, select, trueArm, DT))
        return IntRange::bottom();

    Value *target = val == select
        ? (trueArm ? select->getTrueValue() : select->getFalseValue())
        : val;
    IntRange targetRange = query(target, select->getParent(), DT);
    auto *compare =
        dynamic_cast<BinaryInst *>(select->getCondition());
    if (!compare ||
        !BinaryInst::isCompareOp(compare->getOp()))
        return targetRange;

    Value *lhs = nullptr;
    Value *rhs = nullptr;
    if (!compare->getOperands(lhs, rhs))
        return targetRange;
    IntRange lhsRange = query(lhs, select->getParent(), DT);
    IntRange rhsRange = query(rhs, select->getParent(), DT);
    return constrainSignedRangeByComparison(
        target, compare, trueArm, targetRange, lhsRange, rhsRange);
}

UIntRange RangeInfo::queryUnsignedOnSelectArm(
    Value *val, SelectInst *select, bool trueArm,
    const DominatorTree &DT) const {
    if (!select)
        return queryUnsigned(
            val, static_cast<BasicBlock *>(nullptr), DT);
    if (!selectArmIsExecutable(*this, select, trueArm, DT))
        return UIntRange::bottom();

    Value *target = val == select
        ? (trueArm ? select->getTrueValue() : select->getFalseValue())
        : val;
    IntRange signedRange = query(target, select->getParent(), DT);
    UIntRange unsignedRange =
        queryUnsigned(target, select->getParent(), DT);
    KnownBits bits = queryBits(target, select->getParent(), DT);

    auto *compare =
        dynamic_cast<BinaryInst *>(select->getCondition());
    if (compare && BinaryInst::isCompareOp(compare->getOp())) {
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        if (compare->getOperands(lhs, rhs)) {
            IntRange lhsRange =
                query(lhs, select->getParent(), DT);
            IntRange rhsRange =
                query(rhs, select->getParent(), DT);
            signedRange = constrainSignedRangeByComparison(
                target, compare, trueArm, signedRange,
                lhsRange, rhsRange);
        }
    }

    reduceIntegerRangeProduct(signedRange, unsignedRange, bits);
    return unsignedRange;
}

KnownBits RangeInfo::queryBits(Value *val) const {
    if (!val ||
        !IRType::hasIntegerBitWidth(val->getIRType(), 32))
        return KnownBits::top();
    if (auto *CI = dynamic_cast<ConstInt *>(val))
        return KnownBits::fromConst(CI->getVal());
    auto it = GlobalBits.find(val);
    KnownBits bits =
        it != GlobalBits.end() ? it->second : KnownBits::top();
    auto uIt = GlobalUnsignedRanges.find(val);
    if (uIt == GlobalUnsignedRanges.end()) return bits;
    KnownBits intervalBits =
        KnownBits::fromUIntRange(uIt->second);
    KnownBits combined(bits.zero | intervalBits.zero,
                       bits.one | intervalBits.one);
    return combined.isConflict() ? bits : combined;
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
            std::unordered_set<BasicBlock *> seen;
            while (runner && seen.insert(runner).second) {
                auto bIt = blockMap.find(runner);
                if (bIt != blockMap.end()) {
                    kb = kb.intersectWith(bIt->second);
                }
                runner = DT.getIDom(runner);
            }
        }
    }

    UIntRange U = queryUnsigned(val, ctxBlock, DT);
    KnownBits intervalBits = KnownBits::fromUIntRange(U);
    KnownBits combined(kb.zero | intervalBits.zero,
                       kb.one | intervalBits.one);
    if (!combined.isConflict()) kb = combined;
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
    UIntRange U = queryUnsignedOnEdge(val, predBB, succBB, DT);
    KnownBits intervalBits = KnownBits::fromUIntRange(U);
    KnownBits combined(KB.zero | intervalBits.zero,
                       KB.one | intervalBits.one);
    if (!combined.isConflict()) KB = combined;
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
    if (It == ValueRanges.end()) {
        IntRange constantSigned;
        UIntRange constantUnsigned;
        IntRange initial = getIntegerConstantRanges(
            V, constantSigned, constantUnsigned)
            ? constantSigned
            : conservativeIntegerRangeForValue(V);
        It = ValueRanges.emplace(V, initial).first;
    }
    return It->second;
}

UIntRange RangeSolver::getUnsignedRange(Value *V) {
    IntRange constantSigned;
    UIntRange constantUnsigned;
    if (getIntegerConstantRanges(
            V, constantSigned, constantUnsigned))
        return constantUnsigned;
    auto It = UnsignedValueRanges.find(V);
    if (It == UnsignedValueRanges.end())
        return conservativeUnsignedRangeForValue(V);
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
    IntRange constantSigned;
    UIntRange constantUnsigned;
    IntRange r = getIntegerConstantRanges(
        V, constantSigned, constantUnsigned)
        ? constantSigned : getRange(V);
    UIntRange u = getUnsignedRange(V);
    KnownBits kb = getBits(V);
    if (!BB) {
        reduceIntegerRangeProduct(r, u, kb);
        return r;
    }

    auto sIt = ContextualRanges.find(V);
    auto uIt = ContextualUnsignedRanges.find(V);
    auto kIt = ContextualBits.find(V);
    BasicBlock *runner = BB;
    std::unordered_set<BasicBlock *> seen;
    while (runner && seen.insert(runner).second) {
        if (sIt != ContextualRanges.end()) {
            auto it = sIt->second.find(runner);
            if (it != sIt->second.end())
                r = r.intersectWith(it->second);
        }
        if (uIt != ContextualUnsignedRanges.end()) {
            auto it = uIt->second.find(runner);
            if (it != uIt->second.end())
                u = u.intersectWith(it->second);
        }
        if (kIt != ContextualBits.end()) {
            auto it = kIt->second.find(runner);
            if (it != kIt->second.end())
                kb = kb.intersectWith(it->second);
        }
        if (!DomTree) break;
        runner = DomTree->getIDom(runner);
    }
    reduceIntegerRangeProduct(r, u, kb);
    return r;
}

UIntRange RangeSolver::getUnsignedRangeInBlock(
    Value *V, BasicBlock *BB) {
    if (!isModeledIntegerRangeType(V))
        return conservativeUnsignedRangeForValue(V);
    IntRange constantSigned;
    UIntRange constantUnsigned;
    bool isConstant = getIntegerConstantRanges(
        V, constantSigned, constantUnsigned);
    IntRange r = isConstant ? constantSigned : getRange(V);
    UIntRange u = isConstant
        ? constantUnsigned : getUnsignedRange(V);
    KnownBits kb = getBits(V);

    auto sIt = ContextualRanges.find(V);
    auto uIt = ContextualUnsignedRanges.find(V);
    auto kIt = ContextualBits.find(V);
    BasicBlock *runner = BB;
    std::unordered_set<BasicBlock *> seen;
    while (runner && seen.insert(runner).second) {
        if (sIt != ContextualRanges.end()) {
            auto it = sIt->second.find(runner);
            if (it != sIt->second.end())
                r = r.intersectWith(it->second);
        }
        if (uIt != ContextualUnsignedRanges.end()) {
            auto it = uIt->second.find(runner);
            if (it != uIt->second.end())
                u = u.intersectWith(it->second);
        }
        if (kIt != ContextualBits.end()) {
            auto it = kIt->second.find(runner);
            if (it != kIt->second.end())
                kb = kb.intersectWith(it->second);
        }
        if (!DomTree) break;
        runner = DomTree->getIDom(runner);
    }
    reduceIntegerRangeProduct(r, u, kb);
    return u;
}

KnownBits RangeSolver::getBitsInBlock(Value *V, BasicBlock *BB) {
    if (!V ||
        !IRType::hasIntegerBitWidth(V->getIRType(), 32))
        return KnownBits::top();
    if (auto *CI = dynamic_cast<ConstInt *>(V))
        return KnownBits::fromConst(CI->getVal());
    IntRange r = getRange(V);
    UIntRange u = getUnsignedRange(V);
    KnownBits kb = ValueBits[V];
    auto sIt = ContextualRanges.find(V);
    auto uIt = ContextualUnsignedRanges.find(V);
    auto kIt = ContextualBits.find(V);
    BasicBlock *runner = BB;
    std::unordered_set<BasicBlock *> seen;
    while (runner && seen.insert(runner).second) {
        if (sIt != ContextualRanges.end()) {
            auto it = sIt->second.find(runner);
            if (it != sIt->second.end())
                r = r.intersectWith(it->second);
        }
        if (uIt != ContextualUnsignedRanges.end()) {
            auto it = uIt->second.find(runner);
            if (it != uIt->second.end())
                u = u.intersectWith(it->second);
        }
        if (kIt != ContextualBits.end()) {
            auto it = kIt->second.find(runner);
            if (it != kIt->second.end())
                kb = kb.intersectWith(it->second);
        }
        if (!DomTree) break;
        runner = DomTree->getIDom(runner);
    }
    reduceIntegerRangeProduct(r, u, kb);
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

bool RangeSolver::updateUnsignedRange(
    Value *V, const UIntRange &newRange) {
    if (!isModeledIntegerRangeType(V)) {
        UIntRange full = conservativeUnsignedRangeForValue(V);
        auto it = UnsignedValueRanges.find(V);
        if (it != UnsignedValueRanges.end() && it->second == full)
            return false;
        UnsignedValueRanges[V] = full;
        return true;
    }

    if (NarrowingMode) {
        UIntRange &current = UnsignedValueRanges[V];
        UIntRange intersected = current.intersectWith(newRange);
        if (intersected.isBottom() || intersected == current)
            return false;
        current = intersected;
        enqueueInstructionUsers(V);
        return true;
    }

    if (UnsignedWidened.count(V)) return false;
    UIntRange &current = UnsignedValueRanges[V];
    if (current == newRange) return false;

    int &count = UnsignedEnqueueCount[V];
    ++count;
    if (count >= MAX_ENQUEUE) {
        current = UIntRange();
        UnsignedWidened.insert(V);
    } else {
        current = newRange;
    }
    enqueueInstructionUsers(V);
    return true;
}

bool RangeSolver::updateIntegerFacts(
    Value *V, const IntRange &newSignedRange,
    const UIntRange &newUnsignedRange,
    const KnownBits &newBits) {
    IntRange signedRange = newSignedRange;
    UIntRange unsignedRange = newUnsignedRange;
    KnownBits bits = newBits;
    reduceIntegerRangeProduct(signedRange, unsignedRange, bits);

    bool changed = false;
    changed = updateBits(V, bits) || changed;
    changed = updateUnsignedRange(V, unsignedRange) || changed;
    changed = updateRange(V, signedRange) || changed;
    return changed;
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

        if (it != ContextualRanges[V].end()) {
            ContextualRanges[V].erase(it);
            enqueueInstructionUsers(V);
        }
        updateUnsignedContextual(V, block, UIntRange());
        return;
    }

    IntRange &cur = ContextualRanges[V][block];
    if (range == cur) {
        updateUnsignedContextual(
            V, block, UIntRange::fromIntRange(range));
        return;
    }
    cur = range;
    updateUnsignedContextual(
        V, block, UIntRange::fromIntRange(range));
    enqueueInstructionUsers(V);
}

void RangeSolver::updateUnsignedContextual(
    Value *V, BasicBlock *block, const UIntRange &range) {
    auto it = ContextualUnsignedRanges[V].find(block);
    if (range.isTop()) {
        if (it == ContextualUnsignedRanges[V].end()) return;
        ContextualUnsignedRanges[V].erase(it);
        enqueueInstructionUsers(V);
        return;
    }

    UIntRange &current = ContextualUnsignedRanges[V][block];
    if (range == current) return;
    current = range;
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
            if (!narrowed.isBottom()) {
                BlockLocalRanges[V][BB] = narrowed;
                UIntRange unsignedHint =
                    UIntRange::fromIntRange(narrowed);
                if (!unsignedHint.isTop())
                    BlockLocalUnsignedRanges[V][BB] =
                        unsignedHint;
            }
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
        updateIntegerFacts(
            Phi, conservativeIntegerRangeForValue(Phi),
            conservativeUnsignedRangeForValue(Phi),
            KnownBits::top());
        return;
    }

    // 2. 若 SCEV 已识别归纳变量，优先使用其初值、步长和边界生成更精确范围。
    if (ScevInductions) {
        auto it = ScevInductions->find(Phi);
        if (it != ScevInductions->end()) {
            IntRange inductionRange =
                rangeFromInduction(it->second);
            UIntRange unsignedInduction =
                UIntRange::fromIntRange(inductionRange);
            KnownBits inductionBits =
                KnownBits::fromUIntRange(unsignedInduction);
            updateIntegerFacts(Phi, inductionRange,
                               unsignedInduction, inductionBits);
            return;
        }
    }

    // 3. 普通整数 PHI 只汇合可行入边，同时合并区间与 KnownBits，再用位信息收紧区间。
    IntRange result = IntRange::bottom();
    UIntRange unsignedResult = UIntRange::bottom();
    KnownBits kbResult;
    bool firstKB = true;
    BasicBlock *phiBB = Phi->getParent();
    forEachPhiIncoming(
        Phi, [this, phiBB, &result, &unsignedResult,
              &kbResult, &firstKB](
                 unsigned, Value *inVal, BasicBlock *pred) {
        if (!FeasibleEdges.count({pred, phiBB})) return;
        IntRange inRange;
        KnownBits inKB;
        if (auto *CI = dynamic_cast<ConstInt *>(inVal)) {
            inRange = signedRangeForConstant(CI);
            inKB = KnownBits::fromConst(CI->getVal());
            unsignedResult = unsignedResult.unionWith(
                unsignedRangeForConstant(CI));
        } else {

            inRange = getRange(inVal);
            inKB = getBits(inVal);
            unsignedResult =
                unsignedResult.unionWith(getUnsignedRange(inVal));
        }
        result = result.unionWith(inRange);
        kbResult = firstKB ? inKB : kbResult.unionWith(inKB);
        firstKB = false;
        });
    reduceIntegerRangeProduct(result, unsignedResult, kbResult);
    updateIntegerFacts(Phi, result, unsignedResult, kbResult);
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
        updateIntegerFacts(
            BI, IntRange(0, 1), UIntRange(0, 1),
            KnownBits::top());
        return;
    }
    if (resultTy && resultTy->isInt() &&
        !isModeledIntegerRangeType(BI)) {
        updateIntegerFacts(
            BI, conservativeIntegerRangeForValue(BI),
            conservativeUnsignedRangeForValue(BI),
            KnownBits::top());
        return;
    }

    // 4. 使用操作数的全局格值做前向传递，避免把某一分支的上下文事实泄漏到其他路径。
    IntRange lRange = dynamic_cast<ConstInt *>(lhs) ?
        signedRangeForConstant(dynamic_cast<ConstInt *>(lhs)) : getRange(lhs);
    IntRange rRange = dynamic_cast<ConstInt *>(rhs) ?
        signedRangeForConstant(dynamic_cast<ConstInt *>(rhs)) : getRange(rhs);
    UIntRange lUnsigned = getUnsignedRange(lhs);
    UIntRange rUnsigned = getUnsignedRange(rhs);

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
    UIntRange unsignedResult;
    switch (BI->getOp()) {
        case BinaryInst::ADD:
            result = IsModeledI32
                ? lRange.add(rRange) : IntRange();
            unsignedResult = IsModeledI32
                ? lUnsigned.add(rUnsigned) : UIntRange();
            break;
        case BinaryInst::SUB:
            result = IsModeledI32
                ? lRange.sub(rRange) : IntRange();
            unsignedResult = IsModeledI32
                ? lUnsigned.sub(rUnsigned) : UIntRange();
            break;
        case BinaryInst::MUL:
            result = IsModeledI32
                ? lRange.mul(rRange) : IntRange();
            unsignedResult = IsModeledI32
                ? lUnsigned.mul(rUnsigned) : UIntRange();
            break;
        case BinaryInst::DIV:
            result = IsModeledI32
                ? lRange.sdiv(rRange) : IntRange();
            unsignedResult = UIntRange::fromIntRange(result);
            break;
        case BinaryInst::UDIV:
            unsignedResult = IsModeledI32
                ? lUnsigned.udiv(rUnsigned) : UIntRange();
            result = unsignedResult.toIntRange();
            break;
        case BinaryInst::MOD:
            result = IsModeledI32
                ? lRange.srem(rRange) : IntRange();
            unsignedResult = UIntRange::fromIntRange(result);
            break;
        case BinaryInst::UREM:
            unsignedResult = IsModeledI32
                ? lUnsigned.urem(rUnsigned) : UIntRange();
            result = unsignedResult.toIntRange();
            break;
        case BinaryInst::SHL:
            result = HasModeledShiftAmount()
                ? lRange.shl(rRange) : IntRange();
            unsignedResult = HasModeledShiftAmount()
                ? lUnsigned.shl(rUnsigned) : UIntRange();
            break;
        case BinaryInst::ASHR:
            result = HasModeledShiftAmount()
                ? lRange.ashr(rRange) : IntRange();
            unsignedResult = UIntRange::fromIntRange(result);
            break;
        case BinaryInst::LSHR:
            unsignedResult = HasModeledShiftAmount()
                ? lUnsigned.lshr(rUnsigned) : UIntRange();
            result = unsignedResult.toIntRange();
            break;
        case BinaryInst::AND:
            result = lRange.band(rRange);
            unsignedResult = lUnsigned.band(rUnsigned);
            break;
        case BinaryInst::OR:
            result = lRange.bor(rRange);
            unsignedResult = lUnsigned.bor(rUnsigned);
            break;
        case BinaryInst::XOR:
            result = IntRange();
            unsignedResult = UIntRange();
            break;
        default:
            result = IntRange();
            unsignedResult = UIntRange();
            break;
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
    reduceIntegerRangeProduct(result, unsignedResult, resKB);
    updateIntegerFacts(BI, result, unsignedResult, resKB);

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
        auto narrowSignedValue =
            [this](Value *value, const IntRange &candidate) {
                UIntRange unsignedRange =
                    getUnsignedRange(value);
                KnownBits bits = getBits(value);
                updateIntegerFacts(
                    value, candidate, unsignedRange, bits);
            };
        switch (BI->getOp()) {
            case BinaryInst::ADD: {
                if (lRange.add(rRange).isTop()) break;
                if (!dynamic_cast<ConstInt *>(lhs))
                    narrowSignedValue(lhs, zR.sub(rRange));
                if (!dynamic_cast<ConstInt *>(rhs))
                    narrowSignedValue(rhs, zR.sub(lRange));
                break;
            }
            case BinaryInst::SUB: {
                if (lRange.sub(rRange).isTop()) break;
                if (!dynamic_cast<ConstInt *>(lhs))
                    narrowSignedValue(lhs, zR.add(rRange));
                if (!dynamic_cast<ConstInt *>(rhs))
                    narrowSignedValue(rhs, lRange.sub(zR));
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

    // BaseManager 统一识别 ConstBool/ConstInt 常量。常量条件只激活真实后继，
    // 防止不可执行分支中的 PHI incoming 污染全局范围。
    bool constantTruth = false;
    if (cond && cond->tryGetBoolConst(constantTruth)) {
        markEdgeFeasible(
            fromBB, constantTruth ? trueBB : falseBB);
        return;
    }

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
            IntRange lRange = getRange(lhs);
            IntRange rRange = getRange(rhs);
            bool hasI32SignedOrdering =
                IRType::hasIntegerBitWidth(lhs->getIRType(), 32) &&
                IRType::hasIntegerBitWidth(rhs->getIRType(), 32);

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

            if (hasI32SignedOrdering &&
                op == BinaryInst::L) {
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

            else if (hasI32SignedOrdering &&
                     op == BinaryInst::LE) {
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

            else if (hasI32SignedOrdering &&
                     op == BinaryInst::G) {
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

            else if (hasI32SignedOrdering &&
                     op == BinaryInst::GE) {
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
            if (hasI32SignedOrdering) {
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
    }

    // 4. 无论能否收窄，两个条件后继都加入可行图并驱动后续固定点。
    markEdgeFeasible(fromBB, trueBB);
    markEdgeFeasible(fromBB, falseBB);
}

// 调用结果只依据 BuildInFunction 身份和官方 SysY ABI 建模。普通用户函数即使
// 名字相同也不参与匹配；getarray/getfarray 的库契约不检查容量，返回值不得
// 按实参数组大小收窄。
void RangeSolver::visitCall(CallInst *CI) {
    Value *callee = CI->getCallee();
    if (!callee) return;

    auto SetTopForResultType = [&]() {
        auto Ty = CI->getIRType();
        if (!Ty) return;
        if (Ty->isInt())
            updateIntegerFacts(
                CI, conservativeIntegerRangeForValue(CI),
                conservativeUnsignedRangeForValue(CI),
                KnownBits::top());
        else if (Ty->isFloat())
            updateFloatRange(CI, FloatRange());
    };

    if (auto *BIF = CI->getBuiltinCallee()) {
        const std::string &name = BIF->getIdent();
        if (name == "getch") {
            // 仓库主机 runtime 把 char 以 sext i8 返回，而 RISC-V ABI 的
            // plain char 可为 unsigned。未发布显式 ABI signedness 前取两类
            // 扩展字节实现的并集；其无符号位模式是非凸集合，只能保持 Top。
            updateIntegerFacts(
                CI, IntRange(-128, 255), UIntRange(),
                KnownBits::top());
            return;
        }
        if (name == "getint" ||
            BuildInFunction::isArrayInputName(name)) {
            // getint 是任意 i32；数组输入返回输入流给出的元素个数，规范明确
            // 不按目标数组容量检查，因此二者均保持完整 i32。
            SetTopForResultType();
            return;
        }
        // getfloat、输出、计时和内存内建均按签名返回类型保守初始化。
        SetTopForResultType();
        return;
    }

    if (dynamic_cast<Function *>(callee)) {
        SetTopForResultType();
        return;
    }

    SetTopForResultType();
}

// Select 把条件比较视为两条“虚拟臂”：分别在真假条件下收窄直接参与比较的
// 候选值，再汇合结果。全局传递函数只读取已经进入全局格的事实，不能消费
// 求解过程中尚会扩宽的临时块上下文；稳定的块/分臂事实由 RangeInfo 查询接口
// 在求解完成后叠加。否则循环出口早期的窄事实会被固化进 Select 的全局结果，
// 而 narrowing 阶段只能取交、无法再把错误单点扩回正确包络。
void RangeSolver::visitSelect(SelectInst *SI) {
    Value *tv = SI->getTrueValue();
    Value *fv = SI->getFalseValue();
    if (!tv || !fv) return;
    auto ty = SI->getIRType();
    if (!ty) return;

    bool trueExecutable = true;
    bool falseExecutable = true;
    Value *condition = SI->getCondition();
    bool constantTruth = false;
    if (condition && condition->tryGetBoolConst(constantTruth)) {
        trueExecutable = constantTruth;
        falseExecutable = !trueExecutable;
    } else if (condition &&
               isModeledIntegerRangeType(condition)) {
        IntRange conditionRange = getRange(condition);
        if (conditionRange.isSingleValue()) {
            trueExecutable = conditionRange.min != 0;
            falseExecutable = !trueExecutable;
        }
    }

    if (ty->isInt()) {
        if (!isModeledIntegerRangeType(SI)) {
            updateIntegerFacts(
                SI, conservativeIntegerRangeForValue(SI),
                conservativeUnsignedRangeForValue(SI),
                KnownBits::top());
            return;
        }

        auto *compare = dynamic_cast<BinaryInst *>(condition);
        Value *lhs = nullptr;
        Value *rhs = nullptr;
        bool hasIntegerCompare =
            compare && BinaryInst::isCompareOp(compare->getOp()) &&
            compare->getOperands(lhs, rhs) && lhs && rhs &&
            lhs->getIRType() && rhs->getIRType() &&
            lhs->getIRType()->isInt() &&
            rhs->getIRType()->isInt();
        IntRange lhsRange;
        IntRange rhsRange;
        if (hasIntegerCompare) {
            lhsRange = getRange(lhs);
            rhsRange = getRange(rhs);
        }

        auto collectArm =
            [&](Value *value, bool trueArm,
                IntRange &signedRange, UIntRange &unsignedRange,
                KnownBits &bits) {
                signedRange = getRange(value);
                unsignedRange = getUnsignedRange(value);
                bits = getBits(value);
                if (hasIntegerCompare) {
                    signedRange = constrainSignedRangeByComparison(
                        value, compare, trueArm, signedRange,
                        lhsRange, rhsRange);
                }
                reduceIntegerRangeProduct(
                    signedRange, unsignedRange, bits);
            };

        IntRange result = IntRange::bottom();
        UIntRange unsignedResult = UIntRange::bottom();
        KnownBits resultBits;
        bool firstBits = true;
        if (trueExecutable) {
            IntRange armRange;
            UIntRange armUnsigned;
            KnownBits armBits;
            collectArm(tv, true, armRange, armUnsigned, armBits);
            result = result.unionWith(armRange);
            unsignedResult =
                unsignedResult.unionWith(armUnsigned);
            resultBits = armBits;
            firstBits = false;
        }
        if (falseExecutable) {
            IntRange armRange;
            UIntRange armUnsigned;
            KnownBits armBits;
            collectArm(fv, false, armRange, armUnsigned, armBits);
            result = result.unionWith(armRange);
            unsignedResult =
                unsignedResult.unionWith(armUnsigned);
            resultBits = firstBits
                ? armBits : resultBits.unionWith(armBits);
        }
        updateIntegerFacts(
            SI, result, unsignedResult, resultBits);
    } else if (ty->isFloat()) {
        FloatRange tr = dynamic_cast<ConstFloat *>(tv) ?
            FloatRange((double)dynamic_cast<ConstFloat *>(tv)->getVal()) :
            FloatValueRanges[tv];
        FloatRange fr = dynamic_cast<ConstFloat *>(fv) ?
            FloatRange((double)dynamic_cast<ConstFloat *>(fv)->getVal()) :
            FloatValueRanges[fv];
        if (trueExecutable && falseExecutable)
            updateFloatRange(SI, tr.unionWith(fr));
        else if (trueExecutable)
            updateFloatRange(SI, tr);
        else if (falseExecutable)
            updateFloatRange(SI, fr);
        else
            updateFloatRange(SI, FloatRange::bottom());
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
            updateIntegerFacts(
                I, IntRange(0, 1), UIntRange(0, 1),
                KnownBits(0xFFFFFFFEu, 0));
        } else if (dstTy == IR_INT) {
            updateIntegerFacts(
                I, conservativeIntegerRangeForValue(I),
                conservativeUnsignedRangeForValue(I),
                KnownBits::top());
        }
        return;
    }
    if (dynamic_cast<SextInst *>(I)) {

        if (srcTy == IR_INT && SourceBits == 1 &&
            DestinationBits == 32) {
            updateIntegerFacts(
                I, IntRange(-1, 0), UIntRange(),
                KnownBits::top());
        } else if (dstTy == IR_INT) {
            updateIntegerFacts(
                I, conservativeIntegerRangeForValue(I),
                conservativeUnsignedRangeForValue(I),
                KnownBits::top());
        }
        return;
    }
    if (dynamic_cast<TruncInst *>(I)) {

        if (dstTy == IR_INT && DestinationBits == 1) {
            KnownBits srcKB = getBits(src);
            uint32_t z = 0xFFFFFFFEu;
            uint32_t o = 0;
            if (srcKB.zero & 1u) z |= 1u;
            if (srcKB.one  & 1u) o |= 1u;
            updateIntegerFacts(
                I, IntRange(0, 1), UIntRange(0, 1),
                KnownBits(z, o));
        } else if (dstTy == IR_INT) {
            updateIntegerFacts(
                I, conservativeIntegerRangeForValue(I),
                conservativeUnsignedRangeForValue(I),
                KnownBits::top());
        }
        return;
    }
    if (dynamic_cast<SITOFPInst *>(I)) {

        IntRange r = dynamic_cast<ConstInt *>(src) ?
            signedRangeForConstant(dynamic_cast<ConstInt *>(src)) : getRange(src);
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
            updateIntegerFacts(
                I, conservativeIntegerRangeForValue(I),
                conservativeUnsignedRangeForValue(I),
                KnownBits::top());
            return;
        }

        FloatRange r = dynamic_cast<ConstFloat *>(src) ?
            FloatRange((double)dynamic_cast<ConstFloat *>(src)->getVal()) :
            FloatValueRanges[src];
        if (r.isBottom() || !r.hasNumericValues() || r.mayContainNaN()) {
            updateIntegerFacts(
                I, IntRange(), UIntRange(),
                KnownBits::top());
            return;
        }
        double lo = std::trunc(r.min);
        double hi = std::trunc(r.max);
        if (!std::isfinite(lo) || !std::isfinite(hi)) {
            updateIntegerFacts(
                I, IntRange(), UIntRange(),
                KnownBits::top());
            return;
        }

        if (lo < (double)IntRange::I32_MIN || hi > (double)IntRange::I32_MAX) {
            updateIntegerFacts(
                I, IntRange(), UIntRange(),
                KnownBits::top());
            return;
        }
        int64_t iLo = (int64_t)lo;
        int64_t iHi = (int64_t)hi;
        if (iLo > iHi) {
            updateIntegerFacts(
                I, IntRange(), UIntRange(),
                KnownBits::top());
            return;
        }
        IntRange result(iLo, iHi);
        UIntRange unsignedResult =
            UIntRange::fromIntRange(result);
        updateIntegerFacts(
            I, result, unsignedResult,
            KnownBits::fromUIntRange(unsignedResult));
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

        if (I->getIRType() && I->getIRType()->isInt())
            updateIntegerFacts(
                I, conservativeIntegerRangeForValue(I),
                conservativeUnsignedRangeForValue(I),
                KnownBits::top());
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
    ContextualUnsignedRanges.clear();
    ContextualBits.clear();
    EdgeRanges.clear();
    EdgeUnsignedRanges.clear();
    EdgeBits.clear();

    // 3.2 后续闭包统一读取最终全局格，并只记录非 Top、非冲突且可安全落块的事实。
    auto FinalRange = [&](Value *V) -> IntRange {
        IntRange constantSigned;
        UIntRange constantUnsigned;
        if (getIntegerConstantRanges(
                V, constantSigned, constantUnsigned))
            return constantSigned;
        auto It = ValueRanges.find(V);
        return It == ValueRanges.end() ? IntRange() : It->second;
    };
    auto FinalBits = [&](Value *V) -> KnownBits {
        if (auto *CI = dynamic_cast<ConstInt *>(V))
            return KnownBits::fromConst(CI->getVal());
        auto It = ValueBits.find(V);
        return It == ValueBits.end() ? KnownBits::top() : It->second;
    };
    auto FinalUnsigned = [&](Value *V) -> UIntRange {
        IntRange constantSigned;
        UIntRange constantUnsigned;
        if (getIntegerConstantRanges(
                V, constantSigned, constantUnsigned))
            return constantUnsigned;
        auto It = UnsignedValueRanges.find(V);
        return It == UnsignedValueRanges.end()
            ? UIntRange() : It->second;
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

        UIntRange UnsignedFact =
            UIntRange::fromIntRange(Fact);
        if (UnsignedFact.isTop() ||
            UnsignedFact.isBottom())
            return;
        UnsignedFact =
            FinalUnsigned(V).intersectWith(UnsignedFact);
        if (UnsignedFact.isTop() ||
            UnsignedFact.isBottom())
            return;
        auto &UnsignedEdgeMap =
            EdgeUnsignedRanges[V][Pred];
        auto UExisting = UnsignedEdgeMap.find(Succ);
        if (UExisting != UnsignedEdgeMap.end()) {
            UnsignedFact =
                UExisting->second.intersectWith(UnsignedFact);
            if (UnsignedFact.isBottom()) {
                UnsignedEdgeMap.erase(UExisting);
                if (HasUniqueIncomingEdge(Pred, Succ)) {
                    auto CIt =
                        ContextualUnsignedRanges.find(V);
                    if (CIt !=
                        ContextualUnsignedRanges.end())
                        CIt->second.erase(Succ);
                }
                return;
            }
        }
        UnsignedEdgeMap[Succ] = UnsignedFact;
        if (HasUniqueIncomingEdge(Pred, Succ)) {
            auto &BlockMap =
                ContextualUnsignedRanges[V];
            auto BIt = BlockMap.find(Succ);
            if (BIt == BlockMap.end()) {
                BlockMap[Succ] = UnsignedFact;
            } else {
                UIntRange Combined =
                    BIt->second.intersectWith(UnsignedFact);
                if (Combined.isBottom())
                    BlockMap.erase(BIt);
                else
                    BlockMap[Succ] = Combined;
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
        bool HasI32SignedOrdering =
            IRType::hasIntegerBitWidth(
                LHS->getIRType(), 32) &&
            IRType::hasIntegerBitWidth(
                RHS->getIRType(), 32);

        switch (Cmp->getOp()) {
            case BinaryInst::L:
                if (!HasI32SignedOrdering) break;
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
                if (!HasI32SignedOrdering) break;
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
                if (!HasI32SignedOrdering) break;
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
                if (!HasI32SignedOrdering) break;
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

        if (HasI32SignedOrdering) {
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
}

// RangeAnalysis 主求解流程严格分阶段运行；任何 narrowing 截断都会丢弃部分下近似。
RangeInfo RangeSolver::solve(Function &F, SCEVResult *SCEV,
                             const DominatorTree *DT) {
    // 1. 从入口启动可达性与前向传播，更新次数预算保证循环最终扩宽收敛。
    CurFunc = &F;
    DomTree = DT;

    markBlockExecutable(F.front());
    runForward();

    // 2. 在最终前向格上执行反向收窄；预算耗尽时清空值事实，查询自然退化为 Top。
    const bool NarrowingComplete = runNarrowing(F);

    if (!NarrowingComplete) {
        ValueRanges.clear();
        UnsignedValueRanges.clear();
        FloatValueRanges.clear();
        ValueBits.clear();
        BlockLocalRanges.clear();
        BlockLocalUnsignedRanges.clear();
    }

    // 3. 丢弃求解期上下文，并仅在 narrowing 完成后从最终 CFG 重建路径事实。
    ContextualRanges.clear();
    ContextualUnsignedRanges.clear();
    FloatContextualRanges.clear();
    ContextualBits.clear();
    EdgeRanges.clear();
    EdgeUnsignedRanges.clear();
    EdgeBits.clear();
    if (NarrowingComplete)
        rebuildIntegerContextualFacts(F);

    // 4. 叠加 SCEV 已证明有限且不回绕的循环体局部归纳范围。
    if (NarrowingComplete)
        applySCEVContextHints(SCEV);

    // 5. 移交各格、上下文和归纳摘要，形成只读分析结果。
    RangeInfo info;
    info.GlobalRanges = std::move(ValueRanges);
    info.GlobalUnsignedRanges =
        std::move(UnsignedValueRanges);
    info.ContextualRanges = std::move(ContextualRanges);
    info.ContextualUnsignedRanges =
        std::move(ContextualUnsignedRanges);
    info.EdgeRanges = std::move(EdgeRanges);
    info.EdgeUnsignedRanges =
        std::move(EdgeUnsignedRanges);
    info.BlockLocalRanges = std::move(BlockLocalRanges);
    info.BlockLocalUnsignedRanges =
        std::move(BlockLocalUnsignedRanges);
    info.FloatGlobalRanges = std::move(FloatValueRanges);
    info.FloatContextualRanges = std::move(FloatContextualRanges);
    info.GlobalBits = std::move(ValueBits);
    info.ContextualBits = std::move(ContextualBits);
    info.EdgeBits = std::move(EdgeBits);
    if (ScevInductions)
        info.LoopInductions = *ScevInductions;
    return info;
}

// Pass 入口取得 SCEV 后创建一次性求解器，并返回封装好的范围信息。
RangeAnalysis::Result RangeAnalysis::run(Function &F,
                                          FunctionAnalysisPassManager &FAM) const {
    // 1. 获取循环次数提示和完整支配上下文；依赖关系由新 PassManager 自动记录。
    auto &SCEV = FAM.getResult<SCEVAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    // 2. 创建一次性求解器，运行五阶段流程并返回只读范围结果。
    RangeSolver solver;
    return solver.solve(F, &SCEV, &DT);
}
