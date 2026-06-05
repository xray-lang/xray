/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_coro_state.h - VM coroutine backend state layout
 *
 * KEY CONCEPT:
 *   Scheduler-owned coroutine shells keep backend_state opaque. VM and JIT
 *   code include this header when they need the VM-shaped execution payload.
 */

#ifndef XVM_CORO_STATE_H
#define XVM_CORO_STATE_H

#include "../coro/xcoroutine.h"

typedef enum {
    XR_CORO_ENTRY_CLOSURE,
    XR_CORO_ENTRY_NATIVE,
    XR_CORO_ENTRY_CFUNC
} XrCoroEntryType;

typedef union {
    XrClosure *closure;
    struct {
        void (*func)(void *);
        void *arg;
    } native;
    XrCFuncResult (*cfunc)(struct XrayIsolate *, XrValue *, int, XrValue *);
} XrCoroEntry;

struct XrVmCoroState {
    XrJitCoroState *jit_state;
    XrVMContext ctx;
    XrCoroEntryType entry_type;
    XrCoroEntry entry;
    XrValue *args;
    int arg_count;
    XrValue inline_args[4];
};

static inline XrVmCoroState *xr_coro_maybe_vm_state(XrCoroutine *coro) {
    if (!xr_coro_backend_is_vm(coro))
        return NULL;
    return (XrVmCoroState *) coro->backend_state;
}

static inline const XrVmCoroState *xr_coro_maybe_vm_state_const(const XrCoroutine *coro) {
    if (!xr_coro_backend_is_vm(coro))
        return NULL;
    return (const XrVmCoroState *) coro->backend_state;
}

static inline XrVMContext *xr_coro_vm_ctx(XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "coro_vm_ctx: NULL coro");
    XrVmCoroState *state = xr_coro_maybe_vm_state(coro);
    XR_DCHECK(state != NULL, "coro_vm_ctx: missing VM state");
    return &state->ctx;
}

static inline const XrVMContext *xr_coro_vm_ctx_const(const XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "coro_vm_ctx_const: NULL coro");
    const XrVmCoroState *state = xr_coro_maybe_vm_state_const(coro);
    XR_DCHECK(state != NULL, "coro_vm_ctx_const: missing VM state");
    return &state->ctx;
}

static inline XrJitCoroState *xr_coro_peek_jit_state(XrCoroutine *coro) {
    XrVmCoroState *state = xr_coro_maybe_vm_state(coro);
    return state ? state->jit_state : NULL;
}

static inline XrJitCoroState *xr_coro_jit_state(XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "coro_jit_state: NULL coro");
    XrJitCoroState *jit_state = xr_coro_peek_jit_state(coro);
    if (!jit_state || !jit_state->scratch) {
        XrJitCoroState *state = xr_coro_prepare_jit_state(coro);
        XR_DCHECK(state != NULL, "coro_jit_state: missing JIT state");
        return state;
    }
    return jit_state;
}

#endif /* XVM_CORO_STATE_H */
