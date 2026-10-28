/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_stack.h - Entry capacity and coroutine storage growth
 *
 * KEY CONCEPT:
 *   Storage growth preserves the context top on success and failure.
 */
#ifndef XVM_STACK_H
#define XVM_STACK_H

#include "../runtime/xexec_frame.h"

struct XrCoroutine;
XR_FUNC bool xr_coro_grow_stack(struct XrCoroutine *coro, int extra_slots);

/* Reserve an entry frame and its stack window. The context must be the
 * current coroutine's exact VM state to grow; a static context may only
 * use existing capacity. Every outcome retains a valid stack_top. */
XR_FUNC bool xr_vm_prepare_entry(XrVMContext *ctx, int extra_stack);

/* Ephemeral dispatch cursors, never stored beyond the current call. */
typedef struct {
    XrValue *base;
    XrBcCallFrame *frame;
    XrInstruction *pc;
} XrVMStackCursor;

XR_FUNC bool xr_vm_ensure_call_stack(XrVMContext *ctx, struct XrCoroutine *coro,
                                      int needed, XrVMStackCursor *cursor);
XR_FUNC XrBcCallFrame *xr_vm_push_frame(XrVMContext *ctx, struct XrCoroutine *coro,
                                        XrClosure *closure, int base_delta,
                                        XrVMStackCursor *cursor);

#endif // XVM_STACK_H
