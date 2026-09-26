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

#define XR_XIR_CALL_ABI_VERSION 2u
#define XR_XIR_CALL_STATE_ALIGNMENT 16u
typedef struct XrXirCall XrXirCall;
typedef enum XrXirCallStatus {
    XR_XIR_CALL_READY, XR_XIR_CALL_RETURNED, XR_XIR_CALL_THROWN,
    XR_XIR_CALL_SUSPENDED, XR_XIR_CALL_CANCELLED, XR_XIR_CALL_LIMIT,
    XR_XIR_CALL_OOM, XR_XIR_CALL_BAD_ARGUMENT, XR_XIR_CALL_BAD_ABI,
    XR_XIR_CALL_BAD_STATE, XR_XIR_CALL_BUSY, XR_XIR_CALL_OVERFLOW,
    XR_XIR_CALL_CONSUMED, XR_XIR_CALL_OUTPUT_ERROR
} XrXirCallStatus;
typedef struct XrXirCallResult {
    XrXirCallStatus status;
    XrXirValue value;
    uint64_t wake;
} XrXirCallResult;
typedef enum XrXirActionKind {
    XR_XIR_ACTION_CALL = 1, XR_XIR_ACTION_RETURN, XR_XIR_ACTION_THROW,
    XR_XIR_ACTION_SUSPEND, XR_XIR_ACTION_CONTINUE, XR_XIR_ACTION_FAULT,
    XR_XIR_ACTION_OUTPUT
} XrXirActionKind;
typedef struct XrXirAction {
    XrXirActionKind kind;
    uint32_t callee;
    const XrXirValue *arguments;
    uint32_t argument_count;
    XrXirValue value;
} XrXirAction;
typedef struct XrXirCallView {
    XrXirCall *activation;
    void *instance;
    const void *environment;
    void *state;
    const XrXirValue *arguments;
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

typedef enum XrXirOutputStream { XR_XIR_STDOUT = 1, XR_XIR_STDERR = 2 } XrXirOutputStream;
typedef bool (*XrXirOutputEntry)(void *context, XrXirOutputStream stream, const XrXirValue *value);
typedef struct XrXirOutputProvider {
    XrXirOutputEntry write;
    void *context;
} XrXirOutputProvider;
typedef struct XrXirCallConfig {
    const XrXirCallEntry *entries;
    uint32_t entry_count;
    void *instance;
    uint64_t byte_limit, poll_limit;
    uint32_t depth_limit;
    XrXirCallAccounting *accounting;
    XrXirOutputProvider output;
} XrXirCallConfig;

XR_FUNC XrXirCallStatus xr_xir_call_new(const XrXirCallConfig *config, uint32_t entry,
    const XrXirValue *arguments, uint32_t count, XrXirCall **output);
XR_FUNC XrXirCallResult xr_xir_call_poll(XrXirCall *activation);
XR_FUNC XrXirCallStatus xr_xir_call_take_result(XrXirCall *activation, XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_call_resume(XrXirCall *activation, uint64_t wake);
XR_FUNC XrXirCallStatus xr_xir_call_cancel(XrXirCall *activation);
XR_FUNC XrXirCallStatus xr_xir_call_free(XrXirCall *activation);
#endif // XXIR_CALL_H
