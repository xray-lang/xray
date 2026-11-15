/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_values.h - Fixed values under the active execution boundary
 *
 * KEY CONCEPT:
 *   Generated code borrows the execution's stable root. Nonlocal failure never
 *   depends on owners in an abandoned C activation.
 */
#ifndef XRAY_NATIVE_VALUES_H
#define XRAY_NATIVE_VALUES_H
#include "xray_native_managed_root.h"
#include "xray_native_string_value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Requires an active synchronous execution and a source id from its source map.
 * The borrowed root lives until that execution exits. Initialization is lazy;
 * scalar and POD-only code does not call this function or allocate a root.
 * The execution owns all frames before entering fallible generated code. */
XRAY_API XrtManagedRoot *xrt_native_managed_root(uint32_t source_id);
/* Call only after the failing kernel operation has returned and released its
 * exclusive guard. Status must be a non-OK managed status, never a diagnostic
 * enum cast. Recovery aborts pending allocations before committed owners.
 * Allocator, lease and payload-cleanup callbacks must return normally instead. */
#ifdef __cplusplus
[[noreturn]] XRAY_API void xrt_native_managed_failure(XrtManagedStatus, uint32_t source_id);
#else
_Noreturn XRAY_API void xrt_native_managed_failure(XrtManagedStatus, uint32_t source_id);
#endif
/* The frame is an active stable frame in this execution. Success publishes one
 * complete String owner into its empty slot; failure ends this execution with
 * the original status and source id. Bytes are borrowed until return only. */
XRAY_API void xrt_native_string_create(XrtManagedFrame *, size_t slot,
    const char *bytes, size_t length, uint32_t source_id);
/* Inputs are live admitted String borrows, including aliases. The destination
 * slot is empty. Publication preserves both input objects and their owners. */
XRAY_API void xrt_native_string_concat_create(XrtManagedFrame *, size_t slot,
    const XrtManagedHeader *left, const XrtManagedHeader *right, uint32_t source_id);

#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_VALUES_H
