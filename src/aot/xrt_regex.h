/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_regex.h - Freestanding AOT regex helpers
 */

#ifndef XRT_REGEX_H
#define XRT_REGEX_H

#include <stdbool.h>
#include <stdint.h>

#include "../shared/xr_regex_core.h"
#include "xrt_arc.h"
#include "xrt_value.h"

bool xrt_regex_is_valid_core(const char *data, int64_t len);

static inline XrValue xrt_regex_escape(const char *data, int64_t len) {
    if (!data && len != 0)
        return XR_NULL_VAL;
    size_t n = len < 0 ? 0 : (size_t) len;
    size_t out_len = xr_regex_core_escape_len(data, n);
    if (out_len == (size_t) -1)
        return XR_NULL_VAL;

    XrValue result = xrt_str_alloc(out_len);
    if (xr_regex_core_escape(data, n, xr_str_buf(result), out_len + 1) < 0) {
        xrt_release(result);
        return XR_NULL_VAL;
    }
    return result;
}

static inline XrValue xrt_regex_is_valid(const char *data, int64_t len) {
    return xrt_regex_is_valid_core(data, len) ? XR_TRUE_VAL : XR_FALSE_VAL;
}

#endif  // XRT_REGEX_H
