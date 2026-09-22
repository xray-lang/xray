/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_integer_conversion_core.h - Target-independent exact integer conversion
 */

#ifndef XR_INTEGER_CONVERSION_CORE_H
#define XR_INTEGER_CONVERSION_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* Callers carry exact logical widths; target ABI storage never chooses the
 * interpretation of a source value. Signed results are sign extended so the
 * same bit result works in both exact-width and erased integer carriers. */
static inline uint64_t xr_integer_convert_bits(uint64_t raw, uint8_t source_width,
                                               bool source_signed, uint8_t target_width,
                                               bool target_signed) {
    if (source_width == 0u || source_width > 64u || target_width == 0u || target_width > 64u)
        return raw;
    uint64_t source_mask =
        source_width == 64u ? UINT64_MAX : (UINT64_C(1) << source_width) - UINT64_C(1);
    uint64_t target_mask =
        target_width == 64u ? UINT64_MAX : (UINT64_C(1) << target_width) - UINT64_C(1);
    uint64_t bits = raw & source_mask;
    if (source_signed && (bits & (UINT64_C(1) << (source_width - 1u))) != 0u)
        bits |= ~source_mask;
    bits &= target_mask;
    if (target_signed && (bits & (UINT64_C(1) << (target_width - 1u))) != 0u)
        bits |= ~target_mask;
    return bits;
}

/* Both casts are in range, including the INT64_MIN bit pattern. */
static inline int64_t xr_integer_signed_from_bits(uint64_t bits) {
    return bits <= (uint64_t) INT64_MAX ? (int64_t) bits
                                        : -INT64_C(1) - (int64_t) (UINT64_MAX - bits);
}

#endif  // XR_INTEGER_CONVERSION_CORE_H
