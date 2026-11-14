/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * raw.h - Bare C key allocation kept opaque to the workload translation unit
 *
 * KEY CONCEPT:
 *   Both controls actually allocate every interpolation, without volatile fences.
 */
#ifndef XR_MVL_RAW_H
#define XR_MVL_RAW_H

#include "algorithms.h"

typedef struct {
    size_t length;
    uint64_t hash;
    char bytes[];
} XrMvlRawString;

XR_FUNC XrMvlRawString *xr_mvl_raw_string_new(const char *bytes, size_t length);

#endif // XR_MVL_RAW_H
