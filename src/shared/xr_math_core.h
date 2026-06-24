/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_math_core.h - Runtime-neutral math stdlib core helpers.
 */

#ifndef XRAY_SHARED_XR_MATH_CORE_H
#define XRAY_SHARED_XR_MATH_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*XrMathCoreRandomBytesFn)(void *ctx, unsigned char *buf, size_t len);

static inline int64_t xr_math_core_int_arg_or(bool has_int, int64_t value, int64_t fallback) {
    return has_int ? value : fallback;
}

static inline uint64_t xr_math_core_random_u64(XrMathCoreRandomBytesFn fill, void *ctx) {
    uint64_t r = 0;
    if (fill)
        fill(ctx, (unsigned char *) &r, sizeof(r));
    return r;
}

static inline double xr_math_core_random_f64(XrMathCoreRandomBytesFn fill, void *ctx) {
    uint64_t r = xr_math_core_random_u64(fill, ctx);
    return (double) (r >> 11) * (1.0 / 9007199254740992.0);
}

static inline int64_t xr_math_core_random_i64(XrMathCoreRandomBytesFn fill, void *ctx,
                                              int64_t min_val, int64_t max_val) {
    if (min_val > max_val) {
        int64_t tmp = min_val;
        min_val = max_val;
        max_val = tmp;
    }
    if (min_val == max_val)
        return min_val;

    uint64_t range = (uint64_t) max_val - (uint64_t) min_val + UINT64_C(1);
    uint64_t r;
    if (range == 0) {
        r = xr_math_core_random_u64(fill, ctx);
    } else {
        uint64_t threshold = (-range) % range;
        do {
            r = xr_math_core_random_u64(fill, ctx);
        } while (r < threshold);
        r %= range;
    }
    return (int64_t) ((uint64_t) min_val + r);
}

#endif /* XRAY_SHARED_XR_MATH_CORE_H */
