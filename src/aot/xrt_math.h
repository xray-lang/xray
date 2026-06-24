/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_math.h - Freestanding AOT math helpers with system randomness.
 */

#ifndef XRT_MATH_H
#define XRT_MATH_H

#include "xrt_value.h"
#include "../os/os_random.h"
#include "../shared/xr_math_core.h"

static inline void xrt_math_random_bytes(void *ctx, unsigned char *buf, size_t len) {
    (void) ctx;
    xr_random_bytes(buf, len);
}

static inline double xrt_math_random_f64(void) {
    return xr_math_core_random_f64(xrt_math_random_bytes, NULL);
}

static inline int64_t xrt_math_random_i64(int64_t min_val, int64_t max_val) {
    return xr_math_core_random_i64(xrt_math_random_bytes, NULL, min_val, max_val);
}

static inline int64_t xrt_math_int_arg_or(XrValue v, int64_t fallback) {
    bool has_int = XR_IS_INT(v);
    return xr_math_core_int_arg_or(has_int, has_int ? XR_TO_INT(v) : 0, fallback);
}

static inline XrValue xrt_math_random(void) {
    return XR_FROM_FLOAT(xrt_math_random_f64());
}

static inline XrValue xrt_math_random_int(XrValue min_value, XrValue max_value) {
    return XR_FROM_INT(
        xrt_math_random_i64(xrt_math_int_arg_or(min_value, 0), xrt_math_int_arg_or(max_value, 0)));
}

#endif  // XRT_MATH_H
