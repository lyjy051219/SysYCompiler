#include "IRUtils.hpp"

#include <cstdint>
#include <limits>

bool IRMath::isPowerOf2(int64_t Val) {
    if (Val <= 0) return false;
    auto U = static_cast<uint64_t>(Val);
    return (U & (U - 1)) == 0;
}

bool IRMath::isUnsignedPowerOf2(uint64_t Val) {
    return Val != 0 && ((Val & (Val - 1)) == 0);
}

int IRMath::log2u(uint64_t Val) {
    int N = 0;
    while (Val > 1) {
        Val >>= 1;
        ++N;
    }
    return N;
}

int IRMath::log2i(int64_t Val) {
    return log2u(static_cast<uint64_t>(Val));
}

bool IRMath::addI32(int64_t A, int64_t B, int64_t &Out) {
    Out = A + B;
    return Out >= std::numeric_limits<int32_t>::min() &&
           Out <= std::numeric_limits<int32_t>::max();
}

bool IRMath::addNoOverflow(int64_t A, int64_t B, int64_t &Out) {
    if ((B > 0 && A > std::numeric_limits<int64_t>::max() - B) ||
        (B < 0 && A < std::numeric_limits<int64_t>::min() - B)) {
        return false;
    }
    Out = A + B;
    return true;
}

int64_t IRMath::ceilDivPositive(int64_t Num, int64_t Den) {
    if (Num <= 0 || Den <= 0) return 0;
    return (Num + Den - 1) / Den;
}
