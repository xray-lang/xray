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

static inline double xrt_math_random_f64(void) {
    uint64_t r;
    xr_random_bytes((unsigned char *) &r, sizeof(r));
    return (double) (r >> 11) * (1.0 / 9007199254740992.0);
}

static inline int64_t xrt_math_random_i64(int64_t min_val, int64_t max_val) {
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
        xr_random_bytes((unsigned char *) &r, sizeof(r));
    } else {
        uint64_t threshold = (-range) % range;
        do {
            xr_random_bytes((unsigned char *) &r, sizeof(r));
        } while (r < threshold);
        r %= range;
    }
    return (int64_t) ((uint64_t) min_val + r);
}

static inline XrValue xrt_math_random(void) {
    return XR_FROM_FLOAT(xrt_math_random_f64());
}

static inline XrValue xrt_math_random_int(XrValue min_value, XrValue max_value) {
    return XR_FROM_INT(xrt_math_random_i64(xr_value_to_int64_coerce(min_value),
                                           xr_value_to_int64_coerce(max_value)));
}

#endif  // XRT_MATH_H
