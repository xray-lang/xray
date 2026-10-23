/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_runtime.h - Native output under a controlled execution root
 *
 * KEY CONCEPT:
 *   A standalone root borrows its immutable image while invoking typed code.
 *   The shared output leaf ends the active execution on host output failure.
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
    // A missing end uses two zero fields; it does not erase the real start.
    uint32_t line, column, end_line, end_column;
} XrtSourceLocation;

typedef struct XrtSourceMap {
    const XrtSourceLocation *locations;
    size_t count;
} XrtSourceMap;

typedef void (*XrtSynchronousRoot)(void *context);

// Define this strong external symbol once in the standalone program. The
// descriptor, locations and path bytes remain immutable and image-owned.
// Empty maps use {NULL, 0}; nonempty maps have at most UINT32_MAX entries.
XRAY_API const XrtSourceMap xrt_standalone_source_map;

// The compiler-owned standalone root explicitly passes its strong descriptor.
// Map and context are borrowed only for this synchronous invocation; a valid
// static map needs no allocation or path copy. Normal return guarantees exactly
// one completed root call. Invalid map/root violates a host precondition and
// aborts before guest execution, without a specified guest configuration code.
XRAY_API void xrt_run_standalone(const XrtSourceMap *map, XrtSynchronousRoot root, void *context);

// Emit signed decimal followed by exactly one LF. Zero means unknown source;
// ids 1..count select locations[id - 1]. On write or flush failure this does
// not return: it ends the active supported execution without guest cleanup.
// Under xrt_run_standalone the process exits with 74, without exit callbacks;
// a broken pipe emits no diagnostic. Raw unbound leaf calls are unsupported.
XRAY_API void xrt_print_i64(int64_t value, uint32_t source_location_id);

// Borrow bytes for this synchronous call, preserving byte_length bytes exactly
// (including embedded NUL), then emit one LF. No NUL terminator is required.
// NULL is valid only for an empty span. Nonempty spans must be readable.
// Flush/write failures use the same execution-ending protocol as integer output.
XRAY_API void xrt_print_string(const char *bytes, size_t byte_length, uint32_t source_location_id);

// End the active synchronous execution with division (420) or modulo (421)
// by zero. The source id must be zero or in the active image map. Invalid
// bindings, codes or ids violate host preconditions and abort. Embedded roots
// capture facts for the host; standalone roots render once and exit with 1,
// even if the diagnostic sink fails. No guest cleanup or exit callback runs.
#ifdef __cplusplus
#define XRT_NORETURN [[noreturn]]
#else
#define XRT_NORETURN _Noreturn
#endif
XRT_NORETURN XRAY_API void xrt_arithmetic_panic(uint32_t code, uint32_t source_id);
#undef XRT_NORETURN

#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_RUNTIME_H
