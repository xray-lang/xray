/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_value_format_core.h - Runtime-neutral value formatting rules.
 */

#ifndef XR_VALUE_FORMAT_CORE_H
#define XR_VALUE_FORMAT_CORE_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#define XR_VALUE_FORMAT_MAX_DEPTH 3
#define XR_VALUE_FORMAT_MAX_ELEMENTS 32

static inline int xr_value_format_depth_exceeded(int depth) {
    return depth > XR_VALUE_FORMAT_MAX_DEPTH;
}

static inline int64_t xr_value_format_limit_count(int64_t count) {
    if (count <= 0)
        return 0;
    return count > XR_VALUE_FORMAT_MAX_ELEMENTS ? XR_VALUE_FORMAT_MAX_ELEMENTS : count;
}

static inline int64_t xr_value_format_remaining_count(int64_t total, int64_t shown) {
    return total > shown ? total - shown : 0;
}

static inline int xr_value_format_more_suffix(char *buf, size_t cap, int64_t total, int64_t shown) {
    int64_t remaining = xr_value_format_remaining_count(total, shown);
    if (remaining <= 0) {
        if (buf && cap > 0)
            buf[0] = '\0';
        return 0;
    }
    return snprintf(buf, cap, ", ...(%" PRId64 " more)", remaining);
}

#endif /* XR_VALUE_FORMAT_CORE_H */
