/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_abi.h - Backend-neutral coroutine resume ABI
 *
 * KEY CONCEPT:
 *   The scheduler owns coroutine lifecycle and queue state. Execution
 *   backends own how a suspendable body resumes. This ABI is the narrow
 *   contract between the two sides, so workers do not depend on VM-specific
 *   result enums, frame layouts, or VM entry lifecycle hooks.
 */

#ifndef XCORO_ABI_H
#define XCORO_ABI_H

#include <stdbool.h>
#include <stdint.h>
#include "../runtime/value/xvalue.h"

typedef struct XrCoroutine XrCoroutine;
typedef struct XrWorker XrWorker;
typedef struct XrayIsolate XrayIsolate;

#ifndef XR_CFUNC_RESULT_DEFINED
typedef enum {
    XR_CFUNC_DONE = 0,
    XR_CFUNC_YIELD,
    XR_CFUNC_BLOCKED,
    XR_CFUNC_ERROR,
    XR_CFUNC_CALL_CLOSURE,
    XR_CFUNC_WOULD_BLOCK
} XrCFuncResult;
#define XR_CFUNC_RESULT_DEFINED
#endif

typedef XrCFuncResult (*XrCoroCFuncEntry)(XrayIsolate *isolate, XrValue *args, int nargs,
                                          XrValue *result);

/* ========== Backend Identity ========== */

typedef enum {
    XR_CORO_BACKEND_VM,
    XR_CORO_BACKEND_JIT,
    XR_CORO_BACKEND_AOT,
    XR_CORO_BACKEND_CFUNC,
    XR_CORO_BACKEND_NATIVE,
} XrCoroBackendKind;

/* ========== Scheduler-visible Run Result ========== */

typedef enum {
    XR_CORO_RUN_DONE,
    XR_CORO_RUN_BLOCKED,
    XR_CORO_RUN_YIELD,
    XR_CORO_RUN_SPAWN_CHILD,
    XR_CORO_RUN_ERROR,
    XR_CORO_RUN_CANCELLED,
    XR_CORO_RUN_DEBUG_BREAK,
} XrCoroRunKind;

typedef struct XrCoroRunResult {
    XrCoroRunKind kind;
    XrValue value;
    XrValue error;
    XrCoroutine *child;
    bool error_is_value;
} XrCoroRunResult;

/* ========== Resume Event ========== */

typedef enum {
    XR_CORO_EVENT_START,
    XR_CORO_EVENT_RESUME,
    XR_CORO_EVENT_CHANNEL,
    XR_CORO_EVENT_CHANNEL_CLOSED,
    XR_CORO_EVENT_AWAIT,
    XR_CORO_EVENT_TIMEOUT,
    XR_CORO_EVENT_IO_READY,
    XR_CORO_EVENT_CANCEL,
    XR_CORO_EVENT_DEBUG,
} XrCoroEventKind;

typedef struct XrCoroEvent {
    XrCoroEventKind kind;
    XrValue value;
    uint32_t flags;
} XrCoroEvent;

typedef struct XrCoroRunContext {
    XrWorker *worker;
    XrayIsolate *isolate;
} XrCoroRunContext;

typedef struct XrCoroDebugSnapshot {
    const char *backend_name;
    const char *function_name;
    int frame_count;
    int in_c_frame;
} XrCoroDebugSnapshot;

typedef XrCoroRunResult (*XrCoroResumeFn)(XrCoroutine *coro, const XrCoroEvent *event,
                                          const XrCoroRunContext *run_ctx);

typedef struct XrCoroBackendVTable {
    XrCoroBackendKind kind;
    XrCoroResumeFn resume;
    void (*trace_roots)(XrCoroutine *coro, void *visitor);
    bool (*prepare_recycle)(XrCoroutine *coro, XrWorker *worker);
    void (*reset_reusable)(XrCoroutine *coro);
    void (*release)(XrCoroutine *coro);
    void (*destroy)(XrCoroutine *coro);
    const char *(*debug_name)(const XrCoroutine *coro);
    void (*debug_snapshot)(const XrCoroutine *coro, XrCoroDebugSnapshot *snapshot);
} XrCoroBackendVTable;

static inline XrCoroRunResult xr_coro_run_result(XrCoroRunKind kind) {
    XrCoroRunResult result;
    result.kind = kind;
    result.value = XR_NULL_VAL;
    result.error = XR_NULL_VAL;
    result.child = NULL;
    result.error_is_value = false;
    return result;
}

static inline XrCoroRunResult xr_coro_run_done(XrValue value) {
    XrCoroRunResult result = xr_coro_run_result(XR_CORO_RUN_DONE);
    result.value = value;
    return result;
}

static inline XrCoroRunResult xr_coro_run_error(XrValue error, bool error_is_value) {
    XrCoroRunResult result = xr_coro_run_result(XR_CORO_RUN_ERROR);
    result.error = error;
    result.error_is_value = error_is_value;
    return result;
}

static inline XrCoroRunResult xr_coro_run_spawn_child(XrCoroutine *child) {
    XrCoroRunResult result = xr_coro_run_result(XR_CORO_RUN_SPAWN_CHILD);
    result.child = child;
    return result;
}

XR_FUNC const XrCoroBackendVTable *xr_coro_vm_backend_vtable(void);

#endif  // XCORO_ABI_H
