/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_array_core.h - Runtime-neutral Array method planning helpers.
 */

#ifndef XR_ARRAY_CORE_H
#define XR_ARRAY_CORE_H

#include <stdint.h>

typedef struct XrArrayCoreRange {
    int64_t start;
    int64_t end;
    int64_t count;
} XrArrayCoreRange;

static inline XrArrayCoreRange xr_array_core_slice_range(int64_t length, int64_t start,
                                                         int64_t end) {
    if (length < 0)
        length = 0;

    if (start < 0)
        start += length;
    if (end < 0)
        end += length;

    if (start < 0)
        start = 0;
    if (start > length)
        start = length;

    if (end < 0)
        end = 0;
    if (end > length)
        end = length;

    if (start > end)
        start = end;

    return (XrArrayCoreRange) {start, end, end - start};
}

#endif  // XR_ARRAY_CORE_H
