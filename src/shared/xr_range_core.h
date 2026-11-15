/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_range_core.h - Overflow-free half-open range validation
 */
#ifndef XR_RANGE_CORE_H
#define XR_RANGE_CORE_H
#include <stdbool.h>
#include <stdint.h>

static inline bool xr_range_valid(int64_t start, int64_t end, int64_t length) {
    return start >= 0 && start <= end && end <= length;
}
#endif // XR_RANGE_CORE_H
