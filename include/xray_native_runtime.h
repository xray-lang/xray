/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_runtime.h - Standalone native output and image-owned locations
 *
 * KEY CONCEPT:
 *   A linked program owns one immutable source map for its entire lifetime.
 *   Output failure ends that standalone process without unwinding guest code.
 */
#ifndef XRAY_NATIVE_RUNTIME_H
#define XRAY_NATIVE_RUNTIME_H

#include "xray_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XrtSourceLocation {
    const char *path;
    size_t path_length;
    uint32_t line, column, end_line, end_column;
} XrtSourceLocation;

typedef struct XrtStandaloneSourceMap {
    const XrtSourceLocation *locations;
    size_t count;
} XrtStandaloneSourceMap;

// Define this strong external symbol once in the standalone program. The
// descriptor, locations and path bytes remain immutable and image-owned.
// Empty maps use {NULL, 0}; nonempty maps have at most UINT32_MAX entries.
XRAY_API const XrtStandaloneSourceMap xrt_standalone_source_map;

// Emit signed decimal followed by exactly one LF. Zero means unknown source;
// ids 1..count select locations[id - 1]. On write or flush failure this does
// not return: the standalone process exits with 74, without exit callbacks.
XRAY_API void xrt_print_i64(int64_t value, uint32_t source_location_id);

#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_RUNTIME_H
