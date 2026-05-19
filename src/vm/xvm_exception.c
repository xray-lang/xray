/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_exception.c - Unified throw/unwind path with full stack trace.
 *
 * KEY CONCEPT:
 *   xray's exception model splits into two responsibilities:
 *
 *     1. Build a textual stack trace describing the call chain at
 *        the moment of the throw. This is what surfaces through
 *        exc.stackTrace at the language level and through the
 *        "Uncaught Exception" diagnostic at the embedder level.
 *
 *     2. Rewind the bytecode interpreter state so dispatch
 *        resumes at the matching catch / finally handler (or
 *        terminates cleanly when nothing catches).
 *
 *   Historically these were two separate APIs (xr_vm_add_stacktrace
 *   + xr_vm_throw_exception), and every throw site had to remember
 *   to call them in the right order. Worse, xr_vm_add_stacktrace
 *   only ever recorded the topmost frame, so the trace lost every
 *   intermediate caller — a thrown exception emerged from a 10-deep
 *   call chain looking like it originated at depth 1.
 *
 *   xr_vm_unwind_with_trace() is the single entry point that does
 *   both jobs correctly:
 *
 *     - Walks XrVMContext.frames from the throw site down to the
 *       handler's enclosing frame (or to the bottom if the throw
 *       is uncaught), appending one "at <fn> (line N)" entry to
 *       exc.stackTrace per frame.
 *     - Delegates to xr_vm_throw_exception for the actual rewind
 *       (handler matching, frame_count / stack_top reset, pc
 *       redirect to catch / finally).
 *
 *   The legacy split APIs remain exported as thin wrappers so
 *   downstream code that hasn't migrated still compiles, but every
 *   throw site inside the VM, the cold paths and the JIT runtime
 *   has been pointed at the unified function.
 */

#include "xvm_internal.h"
#include "../base/xchecks.h"
#include "../runtime/object/xexception.h"
#include "../runtime/value/xchunk.h"

/* ========== Stack-trace recording ========== */

/*
 * Append every active call frame (top → bottom of XrVMContext.frames)
 * to the exception's stackTrace array.
 *
 * The trace is recorded in source-language order: the function that
 * actually executed the throw is appended first, then its caller,
 * then the caller's caller, and so on. xr_exception_print walks the
 * array in that same order, so the user sees the throw site first
 * and the outermost frame last — matching the convention of every
 * mainstream runtime (Python, Java, JavaScript).
 */
static void record_full_trace(XrayIsolate *isolate, XrValue exception) {
    if (!xr_value_is_exception(isolate, exception))
        return;

    XrVMContext *ctx = xr_vm_current_ctx(isolate);
    int top = ctx->frame_count;
    if (top == 0)
        return;

    /* Skip frames that have already been recorded — happens on
     * rethrow inside a catch block, where the catch's catch-handler
     * may invoke this function a second time. We detect prior
     * recording by checking the existing trace length: any non-zero
     * trace means a previous unwind already populated it for the
     * outer frames, and rerecording would duplicate the entries. */
    XrValue stack_val = xr_exception_get_stacktrace(isolate, exception);
    if (XR_IS_ARRAY(stack_val)) {
        XrArray *stack = (XrArray *) XR_TO_PTR(stack_val);
        if (stack->length > 0)
            return;
    }

    for (int fi = top - 1; fi >= 0; fi--) {
        XrBcCallFrame *f = &ctx->frames[fi];
        const char *func_name = "?";
        int line = 0;

        if (f->closure && f->closure->proto) {
            XrProto *proto = f->closure->proto;
            if (proto->name) {
                func_name = proto->name->data;
            }
            /* The interpreter has just stored the post-fetch pc into
             * frame->pc via savepc(); that points one instruction
             * past the throwing op. Step back one when computing the
             * source line so the trace highlights the actual throw
             * site instead of the next instruction. */
            int pc_offset = (int) (f->pc - PROTO_CODE_BASE(proto));
            if (pc_offset > 0)
                pc_offset -= 1;
            size_t line_count = PROTO_LINE_COUNT(proto);
            if (pc_offset >= 0 && (size_t) pc_offset < line_count) {
                line = PROTO_LINE(proto, pc_offset);
            }
        }
        xr_exception_add_frame(isolate, exception, func_name, line);
    }
}

/* ========== Public API ========== */

/*
 * Single-call throw helper. Records the full call chain into
 * exc.stackTrace and then performs the actual stack unwind.
 *
 * Callers that previously did
 *
 *     xr_vm_add_stacktrace(isolate, exc);
 *     xr_vm_throw_exception(isolate, exc);
 *
 * should now write
 *
 *     xr_vm_unwind_with_trace(isolate, exc);
 *
 * and let this function decide what gets recorded.
 */
void xr_vm_unwind_with_trace(XrayIsolate *isolate, XrValue exception) {
    XR_DCHECK(isolate != NULL, "vm_unwind_with_trace: NULL isolate");
    record_full_trace(isolate, exception);
    xr_vm_throw_exception(isolate, exception);
}
