/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_backend_ops.h - Optional coroutine backend integration hooks
 *
 * KEY CONCEPT:
 *   xcoro_abi.h is the scheduler/backend resume ABI. These hooks are
 *   runtime integration services for VM-shaped entries, C yieldable calls,
 *   JIT scratch state, and coroutine pooling. AOT generated code should not
 *   need this surface.
 */

#ifndef XCORO_BACKEND_OPS_H
#define XCORO_BACKEND_OPS_H

#include <stdbool.h>
#include "xcoro_abi.h"

typedef struct XrCoroutine XrCoroutine;
typedef struct XrWorker XrWorker;
typedef struct XrayIsolate XrayIsolate;
typedef struct XrClosure XrClosure;

typedef struct XrCoroBackendOps {
    bool (*ensure_state)(XrCoroutine *coro);
    bool (*prepare_execution_state)(XrCoroutine *coro, XrayIsolate *X, XrWorker *worker,
                                    bool need_storage, bool is_clean);
    void (*reset_execution_state)(XrCoroutine *coro, XrayIsolate *X);
    void (*clear_entry_state)(XrCoroutine *coro);
    void (*reset_entry_state_no_free)(XrCoroutine *coro);
    bool (*bind_closure_entry)(XrCoroutine *coro, XrayIsolate *X, XrClosure *closure, XrValue *args,
                               int arg_count, bool copy_args);
    bool (*bind_cfunc_entry)(XrCoroutine *coro, XrCoroCFuncEntry cfunc, XrValue *args,
                             int arg_count);
} XrCoroBackendOps;

XR_FUNC const XrCoroBackendOps *xr_coro_vm_backend_ops(void);

#endif /* XCORO_BACKEND_OPS_H */
