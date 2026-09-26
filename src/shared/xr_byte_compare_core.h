/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_byte_compare_core.h - Allocation-free byte comparison kernel
 */
#ifndef XR_BYTE_COMPARE_CORE_H
#define XR_BYTE_COMPARE_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lengths and strides are public. Callers own the backing storage and prove
 * that each indexed byte is within it. Every compared byte contributes to
 * the volatile accumulator, regardless of the position of a mismatch. */
static inline bool xr_crypto_core_timing_safe_equal(const void *a, size_t a_len, size_t a_stride,
                                                    const void *b, size_t b_len, size_t b_stride) {
    if ((a_len && (!a || !a_stride || a_len - 1u > SIZE_MAX / a_stride)) ||
        (b_len && (!b || !b_stride || b_len - 1u > SIZE_MAX / b_stride)))
        return false;
    const unsigned char *a_bytes = (const unsigned char *)a;
    const unsigned char *b_bytes = (const unsigned char *)b;
    volatile uint8_t diff = (a_len != b_len) ? 1 : 0;
    size_t min_len = a_len < b_len ? a_len : b_len;
    for (size_t i = 0; i < min_len; i++)
        diff |= (uint8_t) (a_bytes[i * a_stride] ^ b_bytes[i * b_stride]);
    return diff == 0;
}

#endif  // XR_BYTE_COMPARE_CORE_H
