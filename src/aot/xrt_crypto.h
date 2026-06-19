/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_crypto.h - Freestanding AOT crypto helpers
 */

#ifndef XRT_CRYPTO_H
#define XRT_CRYPTO_H

#include "../shared/xr_crypto_core.h"
#include "xrt_value.h"

static inline XrValue xrt_crypto_timing_safe_equal(const char *a, int64_t a_len, const char *b,
                                                   int64_t b_len) {
    if (a_len < 0 || b_len < 0)
        return XR_FALSE_VAL;
    return XR_FROM_BOOL(xr_crypto_core_timing_safe_equal(a, (size_t) a_len, b, (size_t) b_len));
}

#endif  // XRT_CRYPTO_H
