/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresume.c - Coroutine resume and unroll mechanism
 *
 * KEY CONCEPT:
 *   Implements coroutine resume with unroll mechanism.
 *   Walks call stack to process continuation functions layer by layer.
 */

#include "xcoroutine.h"
#include "../base/xchecks.h"
#include "xyieldable.h"
#include "../runtime/xvm_call.h"  // XrVMResult, XR_VM_*
#include "../runtime/xray_debug.h"
#include <stdio.h>

// xr_coro_resume_with_unroll - Coroutine resume (with unroll mechanism)
//
// Unroll algorithm:
// 1. Start from current frame, check for continuation functions layer by layer
// 2. If continuation exists, call it
// 3. Based on return: continue unroll, block again, or error
// 4. Until all continuations complete, or encounter block/error
//
// Parameters:
//   X: Isolate pointer
//   coro: coroutine to resume
//   resume_status: resume status (XrResumeStatus)
//
// Returns:
//   XrVMResult: VM_OK (continue), VM_BLOCKED (blocked again), VM_ERROR (error)
XrVMResult xr_coro_resume_with_unroll(XrayIsolate *X, XrCoroutine *coro, int resume_status) {
    if (!X || !coro) {
        return XR_VM_RUNTIME_ERROR;
    }

    // Set coroutine's resume status (continuation can access via coro->resume_status)
    xr_coro_resume_store(coro, resume_status);

    // ========== Unroll mechanism: process continuations in call stack layer by layer ==========

    while (coro->vm_ctx.frame_count > 0) {
        XrBcCallFrame *frame = &coro->vm_ctx.frames[coro->vm_ctx.frame_count - 1];

        // Check if frame has yielded
        if (!(frame->call_status & XR_CALL_YIELDED)) {
            // Frame not yielded, stop unroll
            break;
        }

        // Check if C function frame with continuation
        if ((frame->call_status & XR_CALL_C) && (frame->call_status & XR_CALL_HAS_CONT) &&
            frame->u.c.continuation) {
            // Call continuation (new signature includes result parameter)
            XrContinuation cont = (XrContinuation) frame->u.c.continuation;
            void *user_ctx = frame->u.c.continuation_ctx;
            XrValue cfunc_result = xr_null();

            XrCFuncResult status = cont(X, resume_status, user_ctx, &cfunc_result);

            XR_DBG_CORO("unroll: continuation returned %d, frame_count=%d", status,
                        coro->vm_ctx.frame_count);

            switch (status) {
                case XR_CFUNC_DONE: {
                    // Continuation complete, use VM's startfunc mechanism to store return value
                    // This ensures return value is stored in correct frame context
                    XR_DBG_CORO("unroll DONE: result_slot=%d, base_offset=%d, frame_idx=%d",
                                frame->u.c.result_slot, frame->base_offset,
                                coro->vm_ctx.frame_count - 1);

                    // If this is the coroutine's last frame (frame_count=1),
                    // store result in VM stack[0]; run_on_worker reads it from there.
                    if (coro->vm_ctx.frame_count == 1 && coro->vm_ctx.stack != NULL) {
                        coro->vm_ctx.stack[0] = cfunc_result;
                        XR_DBG_CORO("unroll: coroutine last frame, sync VM stack[0] tag=%u",
                                    cfunc_result.tag);
                    }

                    // Mark return value, let VM store at startfunc
                    // This avoids storing return value in wrong context
                    frame->u.c.cfunc_result = cfunc_result;
                    frame->u.c.has_cfunc_result = true;

                    // Cleanup frame state (C function reuses caller's bytecode frame, don't pop)
                    frame->call_status &= ~(XR_CALL_C | XR_CALL_HAS_CONT | XR_CALL_YIELDED);
                    frame->u.c.continuation = NULL;
                    frame->u.c.continuation_ctx = NULL;

                    // Return OK to continue bytecode execution
                    return XR_VM_OK;
                }

                case XR_CFUNC_BLOCKED:
                    // Continuation blocked again, keep frame state, stop unroll
                    return XR_VM_BLOCKED;

                case XR_CFUNC_YIELD:
                    // Continuation voluntarily yields (yield again)
                    // Note: xr_yield already updated frame's continuation, don't clear!
                    // Just return YIELD status, let scheduler reschedule
                    return XR_VM_YIELD;

                case XR_CFUNC_CALL_CLOSURE:
                    // Continuation called xr_yield_call_closure, closure frame pushed.
                    // Return OK so run_cfunc_coro calls run() to execute the closure.
                    return XR_VM_OK;

                case XR_CFUNC_ERROR:
                    // Continuation error
                    return XR_VM_RUNTIME_ERROR;
            }
        }

        // If bytecode frame (not C function), stop unroll, continue bytecode execution
        if (!(frame->call_status & XR_CALL_C)) {
            // Clear YIELDED flag, prepare to continue execution
            frame->call_status &= ~XR_CALL_YIELDED;
            break;
        }

        // C function frame but no continuation: clear flags and return OK
        // Don't pop frame, C function reuses caller's frame
        frame->call_status &= ~(XR_CALL_C | XR_CALL_YIELDED);
        return XR_VM_OK;
    }

    // ========== Unroll complete, return OK to continue execution ==========

    // Return OK, let VM continue bytecode (if frames empty, VM naturally ends)
    return XR_VM_OK;
}

// xr_coro_set_resume_status / xr_coro_get_resume_status are now
// static inline in xresume.h (trivial wrappers around atomic load/store).
