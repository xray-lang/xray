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

// These handles point to the canonical shared String object, never a second
// native representation. New/retain results own one reference in this root.
// Inputs are borrowed synchronously. Allocation/refcount failures end the root
// with their original value status; an unsuccessful helper never returns NULL.
typedef struct XirString XrtString;
XRAY_API XrtString *xrt_string_new(const char *bytes, size_t length, uint32_t source_id);
XRAY_API XrtString *xrt_string_i64(int64_t value, uint32_t source_id);
XRAY_API XrtString *xrt_string_concat(const XrtString *left, const XrtString *right, uint32_t source_id);
XRAY_API XrtString *xrt_string_retain(const XrtString *string, uint32_t source_id);
// Move requires an empty destination and consumes the actual source owner.
// It allocates nothing. Drop consumes one owner and clears its handle.
XRAY_API void xrt_string_move(XrtString **destination, XrtString **source);
XRAY_API void xrt_string_drop(XrtString **string);
// Release only a reference explicitly transferred out of a root/outcome.
// This operation never edits any active root's registered references.
XRAY_API void xrt_string_owned_drop(XrtString **string);
// Borrow the exact byte span, including NUL, while the String owner is alive.
XRAY_API const char *xrt_string_bytes(const XrtString *string, size_t *length);

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

// Read the selected system clock in nanoseconds under an active synchronous
// execution. Source zero is unknown; nonzero IDs belong to its prepared map.
// Realtime/monotonic read failures and any reading beyond INT64_MAX end the
// current execution as a host-clock failure, independently of output or panic.
// Process CPU time may legitimately be zero. These calls do not allocate or
// suspend. Standalone failure renders once and exits with 1.
XRAY_API int64_t xrt_clock_realtime_nanos(uint32_t source_id);
XRAY_API int64_t xrt_clock_process_cpu_nanos(uint32_t source_id);
XRAY_API int64_t xrt_clock_monotonic_nanos(uint32_t source_id);
// Minutes east of UTC at this exact Unix timestamp in seconds. A timestamp
// outside the host calendar range ends the execution as a host-clock failure.
XRAY_API int64_t xrt_clock_utc_offset_at(int64_t seconds, uint32_t source_id);

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
