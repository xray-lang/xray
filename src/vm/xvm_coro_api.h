/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_coro_api.h - VM coroutine backend creation API
 */

#ifndef XVM_CORO_API_H
#define XVM_CORO_API_H

#include "../coro/xcoroutine.h"
#include "../runtime/value/xtransfer_mode.h"

XR_FUNC XrCoroutine *xr_coro_create_bootstrap(XrVMRuntime *X);
XR_FUNC void xr_coro_setup_main(XrCoroutine *coro, XrVMRuntime *X, XrClosure *closure);
XR_FUNC void xr_coro_reset_for_call(XrCoroutine *coro, XrVMRuntime *X, XrClosure *closure);
XR_FUNC void xr_coro_reset_for_call_args(XrCoroutine *coro, XrVMRuntime *X, XrClosure *closure,
                                         XrValue *args, int nargs);
XR_FUNC bool xr_coro_grow_stack(XrCoroutine *coro, int extra_slots);
XR_FUNC XrCoroutine *xr_coro_create_vm_closure(XrVMRuntime *X, XrClosure *closure, XrValue *args,
                                               const uint8_t *arg_modes, int arg_count,
                                               const char *name, const char *file, int line);
XR_FUNC XrCoroutine *xr_coro_create_vm_closure_owned(XrVMRuntime *X, XrClosure *closure,
                                                     XrValue *args, const uint8_t *arg_modes,
                                                     int arg_count, const char *name,
                                                     const char *file, int line);
/* The returned coroutine owns context on every path, including construction
 * failure, cancellation before first execution, recycling and destruction. */
XR_FUNC XrCoroutine *xr_coro_create_vm_cfunc(XrVMRuntime *X, XrCoroCFuncEntry cfunc,
                                             void *context, XrCoroContextDestroy destroy_context,
                                             const char *name);
XR_FUNC XrCoroRunKind xr_vm_coro_run_to_completion(XrCoroutine *coro, XrValue *out);

/*
** Run `closure` with `args` to completion on the scheduler and return its value.
**
** Unlike xr_vm_call_closure, which runs a single nested dispatch and treats a
** suspension as failure, this entry drives the closure as the isolate's main
** coroutine through xr_main_thread_run, so the scheduler and netpoll pump every
** I/O suspension to completion exactly as a top-level program does. It lets
** synchronous C invoke a suspending stdlib coroutine (http.request and the
** transports beneath it) and collect the result.
**
** Arguments are borrowed and deep-copied into the coroutine, so the caller
** retains ownership of every value it built. The returned value lives in the
** main coroutine heap and stays valid until the next call reuses that
** coroutine; consume it before invoking again. On a thrown or runtime error the
** return is null and, when out_error is non-NULL, the error value is stored
** there.
*/
XR_FUNC XrValue xr_vm_run_closure_blocking(XrVMRuntime *isolate, XrClosure *closure, XrValue *args,
                                           int nargs, XrValue *out_error);

#endif  // XVM_CORO_API_H
