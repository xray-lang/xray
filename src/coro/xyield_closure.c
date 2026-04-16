/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xyield_closure.c - Call user closures from C yieldable functions
 *
 * KEY CONCEPT:
 *   xr_yield_call_closure() allows C-layer code to invoke a user closure
 *   that may yield (channel/await/sleep). The closure is pushed as a new
 *   call frame on the coroutine's VM stack. When the closure returns,
 *   the VM detects CLOSURE_PENDING on the caller frame and invokes the
 *   continuation with XR_RESUME_CLOSURE_DONE.
 *
 * WHY THIS DESIGN:
 *   - Eliminates .xr script bridge layer for stdlib modules (http, ws)
 *   - Enables third-party C libraries to use xray coroutines fully
 *   - Single code path: all closure calls go through VM execution
 */

#include "xyieldable.h"
#include "xcoroutine.h"
#include "xworker.h"
#include "../base/xchecks.h"
#include "../vm/xvm_state_frame.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/xisolate_internal.h"
#include <string.h>

// Get current coroutine via per-worker TLS (thread-safe for multi-worker)
static XrCoroutine *get_current_coro(XrayIsolate *X) {
    (void)X;
    XrWorker *worker = xr_current_worker();
    if (worker && worker->m && worker->m->current_coro)
        return worker->m->current_coro;
    return NULL;
}

XrCFuncResult xr_yield_call_closure(
    XrayIsolate *X,
    XrClosure *closure,
    XrValue *args, int nargs,
    XrContinuation on_complete,
    void *user_ctx,
    XrValue *result)
{
    XR_DCHECK(X != NULL, "yield_call_closure: NULL isolate");
    XR_DCHECK(closure != NULL, "yield_call_closure: NULL closure");
    XR_DCHECK(closure->proto != NULL, "yield_call_closure: NULL proto");
    XR_DCHECK(on_complete != NULL, "yield_call_closure: NULL continuation");

    XrCoroutine *coro = get_current_coro(X);
    XR_DCHECK(coro != NULL, "yield_call_closure: no current coroutine");
    if (!coro) return XR_CFUNC_ERROR;

    XrVMContext *ctx = &coro->vm_ctx;
    XrProto *proto = closure->proto;

    XR_DCHECK(ctx->frame_count > 0, "yield_call_closure: no active frame");
    XrBcCallFrame *caller = &ctx->frames[ctx->frame_count - 1];

    /* Calculate where the closure frame's registers start.
     * For bytecode caller: after caller's register file.
     * For cfunc coro frame-0: at current stack_top. */
    int closure_base_offset;
    if (caller->closure && caller->closure->proto) {
        closure_base_offset = caller->base_offset + caller->closure->proto->maxstacksize;
    } else {
        closure_base_offset = (int)(ctx->stack_top - ctx->stack);
    }

    // Ensure stack and frame capacity
    int needed = closure_base_offset + proto->maxstacksize + 1;
    if (needed > ctx->stack_capacity || ctx->frame_count + 1 >= ctx->frame_capacity) {
        int extra = needed - ctx->stack_capacity + 64;
        if (extra < 64) extra = 64;
        bool ok = xr_coro_gc_grow_stack(coro, extra);
        if (!ok) return XR_CFUNC_ERROR;
        // Re-read after potential realloc
        caller = &ctx->frames[ctx->frame_count - 1];
    }

    // OP_RETURN writes return value to base_offset - 1
    int return_slot_offset = closure_base_offset - 1;
    if (return_slot_offset >= 0 && return_slot_offset < ctx->stack_capacity) {
        ctx->stack[return_slot_offset] = xr_null();
    }

    // Mark caller frame: continuation will be called when closure returns
    caller->call_status |= XR_CALL_CLOSURE_PENDING | XR_CALL_HAS_CONT | XR_CALL_C;
    caller->u.c.continuation = (void *)on_complete;
    caller->u.c.continuation_ctx = user_ctx;
    caller->u.c.has_cfunc_result = false;
    caller->u.c.result_slot = (int16_t)(return_slot_offset - caller->base_offset);

    // Push closure call frame
    XrBcCallFrame *frame = &ctx->frames[ctx->frame_count++];
    memset(frame, 0, sizeof(XrBcCallFrame));
    frame->closure = closure;
    frame->pc = PROTO_CODE_BASE(proto);
    frame->base_offset = closure_base_offset;

    // Copy arguments into closure's register file
    XrValue *closure_base = ctx->stack + closure_base_offset;
    int copy_count = nargs < proto->numparams ? nargs : proto->numparams;
    for (int i = 0; i < copy_count; i++) {
        closure_base[i] = args[i];
    }
    for (int i = copy_count; i < proto->maxstacksize; i++) {
        closure_base[i] = xr_null();
    }

    // Update stack top
    ctx->stack_top = ctx->stack + closure_base_offset + proto->maxstacksize;

    (void)result;
    return XR_CFUNC_CALL_CLOSURE;
}

// xr_stackful_call_closure removed — fully stackless now

XrValue xr_get_closure_result(XrayIsolate *X) {
    XrCoroutine *coro = get_current_coro(X);
    if (coro) {
        return coro->pending_closure_result;
    }
    return xr_null();
}
