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
 *   Scheduler-owned coroutine shells keep backend_state opaque. VM code
 *   includes this header when it needs the VM-shaped execution payload.
 */

#ifndef XVM_CORO_STATE_H
#define XVM_CORO_STATE_H

#include "../coro/xcoroutine.h"
#include "../runtime/xexec_frame.h"

typedef struct XrVmCoroState XrVmCoroState;

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
    XrVMContext ctx;
    XrCoroEntryType entry_type;
    XrCoroEntry entry;
    /* gc that owns entry.closure (the spawning coroutine's gc, captured at
     * bind). The coroutine holds one reference to its entry closure; the final
     * release must return the block to the owner's per-coroutine heap, not to
     * whatever coroutine happens to drop the last reference. */
    struct XrCoroGC *entry_closure_owner;
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

#endif /* XVM_CORO_STATE_H */
