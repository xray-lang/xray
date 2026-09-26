/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_integer_bitwise_core.h - Exact-width integer bit operations
 */

#ifndef XR_INTEGER_BITWISE_CORE_H
#define XR_INTEGER_BITWISE_CORE_H

#ifndef XR_INTEGER_CONVERSION_CORE_H
#include "xr_integer_conversion_core.h"
#endif

/* Validated callers prove width and mode. Counts use modulo 64 for every
 * integer width; only the result is truncated to the left operand's width. */
static inline uint64_t xr_integer_bitwise_bits(uint64_t left, uint64_t right,
                                              uint8_t width, bool is_signed,
                                              uint32_t mode) {
    uint64_t bits = xr_integer_convert_bits(left, width, is_signed, width, is_signed);
    uint32_t count = (uint32_t)(right & UINT64_C(63));
    switch (mode) {
        case 0u: bits &= right; break;
        case 1u: bits |= right; break;
        case 2u: bits ^= right; break;
        case 3u: bits = ~bits; break;
        case 4u: bits <<= count; break;
        case 5u:
            if (count != 0u && is_signed && (bits & (UINT64_C(1) << 63u)))
                bits = (bits >> count) | (UINT64_MAX << (64u - count));
            else
                bits >>= count;
            break;
        default: return 0u;
    }
    return xr_integer_convert_bits(bits, width, is_signed, width, is_signed);
}

#endif  // XR_INTEGER_BITWISE_CORE_H
