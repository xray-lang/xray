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
XR_FUNC bool xr_coro_grow_stack(XrCoroutine *coro, int extra_slots);
XR_FUNC XrCoroutine *xr_coro_create_vm_closure(XrVMRuntime *X, XrClosure *closure, XrValue *args,
                                               const uint8_t *arg_modes, int arg_count,
                                               const char *name, const char *file, int line);
XR_FUNC XrCoroutine *xr_coro_create_vm_cfunc(XrVMRuntime *X, XrCoroCFuncEntry cfunc, XrValue *args,
                                             int argc, const char *name);

#endif  // XVM_CORO_API_H
