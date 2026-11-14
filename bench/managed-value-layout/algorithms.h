/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * algorithms.h - Pure arithmetic shared by managed and raw layout controls
 *
 * KEY CONCEPT:
 *   Identical key construction and growth keep representation comparisons fair.
 */
#ifndef XR_MVL_ALGORITHMS_H
#define XR_MVL_ALGORITHMS_H

#include "base/xhash.h"
#include "base/xmalloc.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>

#define XR_MVL_PUSH_COUNT INT64_C(5000000)
#define XR_MVL_RECORD_COUNT INT64_C(3000000)
#define XR_MVL_MAP_COUNT INT64_C(200000)

static inline size_t xr_mvl_key_bytes(uint64_t index, char bytes[32]) {
    char reversed[24];
    size_t digits = 0;
    do {
        reversed[digits++] = (char) ('0' + index % 10);
        index /= 10;
    } while (index);
    bytes[0] = 'k';
    for (size_t i = 0; i < digits; ++i)
        bytes[i + 1] = reversed[digits - i - 1];
    bytes[digits + 1] = '\0';
    return digits + 1;
}

static inline bool xr_mvl_capacity(size_t old, size_t minimum, size_t stride,
                                    size_t *result) {
    size_t capacity = old ? old : 4;
    if (!stride || minimum > SIZE_MAX / stride)
        return false;
    while (capacity < minimum) {
        if (capacity > SIZE_MAX / 2)
            return false;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / stride)
        return false;
    *result = capacity;
    return true;
}

static inline bool xr_mvl_map_room(size_t length, size_t capacity) {
    return length < capacity - capacity / 4;
}

#endif // XR_MVL_ALGORITHMS_H
