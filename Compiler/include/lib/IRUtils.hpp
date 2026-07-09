#pragma once

#include <cstdint>

class IRMath {
public:
    static bool isPowerOf2(int64_t Val);
    static bool isUnsignedPowerOf2(uint64_t Val);
    static int log2u(uint64_t Val);
    static int log2i(int64_t Val);
    static bool addI32(int64_t A, int64_t B, int64_t &Out);
    static bool addNoOverflow(int64_t A, int64_t B, int64_t &Out);
    static int64_t ceilDivPositive(int64_t Num, int64_t Den);
};
