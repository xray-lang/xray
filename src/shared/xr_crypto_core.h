/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_crypto_core.h - Shared freestanding crypto utilities
 */

#ifndef XR_CRYPTO_CORE_H
#define XR_CRYPTO_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool xr_crypto_core_timing_safe_equal(const char *a, size_t a_len, const char *b,
                                                    size_t b_len) {
    if ((!a && a_len != 0) || (!b && b_len != 0))
        return false;
    volatile uint8_t diff = (a_len != b_len) ? 1 : 0;
    size_t min_len = a_len < b_len ? a_len : b_len;
    for (size_t i = 0; i < min_len; i++)
        diff |= (uint8_t) (((uint8_t) a[i]) ^ ((uint8_t) b[i]));
    return diff == 0;
}

#endif  // XR_CRYPTO_CORE_H
