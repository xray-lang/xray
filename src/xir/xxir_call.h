/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call.h - Typed resumable frames and trampoline boundary
 *
 * KEY CONCEPT:
 *   Calls and suspensions leave the native stack before another entry runs.
 */
#ifndef XXIR_CALL_H
#define XXIR_CALL_H
#include "xxir_scalar.h"

#define XR_XIR_CALL_ABI_VERSION 1u
#define XR_XIR_CALL_STATE_ALIGNMENT 16u
typedef struct XrXirCall XrXirCall;
typedef enum XrXirCallStatus {
    XR_XIR_CALL_READY, XR_XIR_CALL_RETURNED, XR_XIR_CALL_THROWN,
    XR_XIR_CALL_SUSPENDED, XR_XIR_CALL_CANCELLED, XR_XIR_CALL_LIMIT,
    XR_XIR_CALL_OOM, XR_XIR_CALL_BAD_ARGUMENT, XR_XIR_CALL_BAD_ABI,
    XR_XIR_CALL_BAD_STATE, XR_XIR_CALL_BUSY, XR_XIR_CALL_OVERFLOW
} XrXirCallStatus;
typedef struct XrXirCallResult {
    XrXirCallStatus status;
    XrXirScalar value;
    uint64_t wake;
} XrXirCallResult;
typedef enum XrXirActionKind {
    XR_XIR_ACTION_CALL = 1, XR_XIR_ACTION_RETURN, XR_XIR_ACTION_THROW,
    XR_XIR_ACTION_SUSPEND, XR_XIR_ACTION_CONTINUE, XR_XIR_ACTION_FAULT
} XrXirActionKind;
typedef struct XrXirAction {
    XrXirActionKind kind;
    uint32_t callee;
    const XrXirScalar *arguments;
    uint32_t argument_count;
    XrXirScalar value;
} XrXirAction;
typedef struct XrXirCallView {
    XrXirCall *activation;
    void *instance;
    const void *environment;
    void *state;
    const XrXirScalar *arguments;
    uint32_t argument_count;
    XrXirCallResult inbox;
} XrXirCallView;
typedef XrXirAction (*XrXirResumeEntry)(XrXirCallView *view);
typedef void (*XrXirCleanupEntry)(XrXirCallView *view, XrXirCallStatus reason);
typedef struct XrXirCallEntry {
    uint32_t abi_version;
    const XrXirType *parameters;
    uint32_t parameter_count;
    XrXirType result;
    uint32_t state_bytes;
    XrXirResumeEntry resume;
    XrXirCleanupEntry cleanup;
    const void *environment;
} XrXirCallEntry;
typedef struct XrXirCallAccounting {
    uint64_t live_bytes, peak_bytes, allocations, frees;
    uint64_t polls;
    uint32_t depth, peak_depth;
} XrXirCallAccounting;
typedef struct XrXirCallConfig {
    const XrXirCallEntry *entries;
    uint32_t entry_count;
    void *instance;
    uint64_t byte_limit, poll_limit;
    uint32_t depth_limit;
    XrXirCallAccounting *accounting;
} XrXirCallConfig;

XR_FUNC XrXirCallStatus xr_xir_call_new(const XrXirCallConfig *config, uint32_t entry,
    const XrXirScalar *arguments, uint32_t count, XrXirCall **output);
XR_FUNC XrXirCallResult xr_xir_call_poll(XrXirCall *activation);
XR_FUNC XrXirCallStatus xr_xir_call_resume(XrXirCall *activation, uint64_t wake);
XR_FUNC XrXirCallStatus xr_xir_call_cancel(XrXirCall *activation);
XR_FUNC XrXirCallStatus xr_xir_call_free(XrXirCall *activation);
#endif // XXIR_CALL_H
