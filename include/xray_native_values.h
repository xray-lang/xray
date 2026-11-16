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
#include "xray_native_function.h"
#include "xray_native_value_image.h"

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
/* Format the admitted scalar with the shared integer text kernel and publish
 * one complete String in the true-empty stable destination slot. */
XRAY_API void xrt_native_string_i64_create(XrtManagedFrame *, size_t slot,
    int64_t value, uint32_t source_id);
/* Format the exact binary64 payload with the shared integer-only decimal
 * kernel. No floating-point arithmetic or host formatting is performed. */
XRAY_API void xrt_native_string_f64_create(XrtManagedFrame *, size_t slot,
    uint64_t bits, uint32_t source_id);
/* Inputs are live admitted String borrows, including aliases. The destination
 * slot is empty. Publication preserves both input objects and their owners. */
XRAY_API void xrt_native_string_concat_create(XrtManagedFrame *, size_t slot,
    const XrtManagedHeader *left, const XrtManagedHeader *right, uint32_t source_id);
/* Consume the admitted MOVE input into the result with no extra retain. All
 * rejecting work precedes publication; callbacks observe complete owner fields
 * and cannot retire the participating Frame while this transaction is active. */
XRAY_API void xrt_native_string_append_owned(XrtManagedFrame *,
    const XrtStringAppendOwned *, uint32_t source_id);

typedef struct XrtModuleState XrtModuleState;
typedef struct XrtNativeEnvironmentInput {
    const XrtValueImage *image;
    XrtModuleState *state;
    XrtManagedLease code;
    const void *entry, *captures;
    uint32_t type;
} XrtNativeEnvironmentInput;
/* Entry is the admitted immutable backend entry object for this exact Env row,
 * not a function-pointer cast. Code pins that object and the raw image through
 * this execution; static executable data already has permanent lifetime. With
 * State, type belongs to its bound image and no second image is bound here.
 * The caller holds Code or State and every input owner throughout this whole
 * synchronous call, including callbacks. Captures are complete admitted values
 * borrowed until return. Success puts
 * one Env owner in the true-empty frame slot and returns a full Function borrow
 * of that owner. Only the slot owns it; copying the result does not retain.
 * Frame environments do not call this factory. Cold setup is execution-owned,
 * while every produced Env independently keeps its Plan and code alive. */
XRAY_API XrtFunction xrt_native_environment_create(XrtManagedFrame *, size_t slot,
    const XrtNativeEnvironmentInput *, uint32_t source_id);

#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_VALUES_H
