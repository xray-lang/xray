/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_exception.h - AOT exception handling runtime (setjmp/longjmp)
 *
 * KEY CONCEPT:
 *   Cross-function exception propagation uses setjmp/longjmp.
 *   Same-function try/catch uses goto (structured by Xm builder).
 *
 *   Each try block pushes an XrtExcFrame on the thread-local stack.
 *   xrt_throw() longjmps to the nearest frame. If no frame exists,
 *   prints an error and aborts.
 *
 * USAGE IN GENERATED CODE:
 *   XrtExcFrame _ef0;
 *   _ef0.prev = xrt_exc_top;
 *   xrt_exc_top = &_ef0;
 *   if (setjmp(_ef0.buf) != 0) {
 *       xrt_exception = _ef0.exception;
 *       xrt_exc_top = _ef0.prev;
 *       goto L_catch;
 *   }
 *   // ... try body ...
 *   xrt_exc_top = _ef0.prev;   // pop on normal exit
 */

#ifndef XRT_EXCEPTION_H
#define XRT_EXCEPTION_H

#include "xrt_value.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

/* =========================================================================
 * Exception frame (stack-allocated in each try block)
 * ========================================================================= */

typedef struct XrtExcFrame {
    jmp_buf buf;               // setjmp/longjmp target
    XrValue exception;         // exception value (set before longjmp)
    struct XrtExcFrame *prev;  // previous frame in stack
    void *defer_mark;          // xrt_defer_top at try entry (XrtDeferScope*); see xrt_defer.h
} XrtExcFrame;

/* =========================================================================
 * Thread-local exception stack
 *
 * In single-threaded AOT mode, a plain global suffices.
 * Once concurrency support lands, switch to _Thread_local.
 * ========================================================================= */

#ifdef XRT_IMPL
XrtExcFrame *xrt_exc_top = NULL;
XrValue xrt_pending_error = {.tag = XR_TAG_NULL};
#else
extern XrtExcFrame *xrt_exc_top;
extern XrValue xrt_pending_error;
#endif

static inline int xrt_has_pending_error(void) {
    return !XR_IS_NULL(xrt_pending_error);
}

/* Run pending defers above `mark` before a panic transfers control. Defined in
 * xrt_defer.h (included after this header); forward-declared here because
 * xrt_throw_exc must drain skipped frames' defers before longjmp. */
static inline void xrt_defer_unwind_to(void *mark);

/* =========================================================================
 * xrt_throw - throw an exception value
 *
 * If inside a try block (xrt_exc_top != NULL), stores the exception
 * and longjmps to the nearest frame. Otherwise, prints and aborts.
 *
 * Before transferring control, runs the defers of every frame skipped by the
 * jump (down to the catching try's recorded mark, or all of them when
 * uncaught), so `defer` cleanup runs on the panic path exactly as in the VM.
 * ========================================================================= */

static XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    if (xrt_exc_top) {
        /* Caught panic: run the defers of every frame skipped on the way to the
         * handler (down to its recorded mark), then jump. An uncaught panic
         * aborts WITHOUT running defers, matching the VM. */
        xrt_defer_unwind_to(xrt_exc_top->defer_mark);
        xrt_exc_top->exception = exc;
        longjmp(xrt_exc_top->buf, 1);
    }
    /* Uncaught exception: report and exit with status 1, matching the VM's
     * uncaught-exception behavior (a clean exit(1), not a SIGABRT/134 core
     * dump) so both backends agree on the observable exit code. */
    if (exc.tag == XR_TAG_STR || exc.tag == XR_TAG_STR_ARC) {
        fprintf(stderr, "Uncaught exception: %s\n", xr_str_data(exc));
    } else {
        fprintf(stderr, "Uncaught exception (tag=%d)\n", exc.tag);
    }
    exit(1);
}

/* Array index out of bounds → E0430 (spec §3 expr-index-access), matching the
 * VM's XR_ERR_INDEX_OUT_OF_BOUNDS. Lives here (not in xrt_coll.h) because raising
 * a panic is an exception-layer concern; xrt_coll.h's index helpers and the
 * generated typed/fixed-array reads call it via a forward declaration. */
static XRT_COLD _Noreturn void xrt_index_oob(int64_t idx, int64_t length) {
    char buf[96];
    snprintf(buf, sizeof(buf), "E0430: array index out of range: %lld (length %lld)",
             (long long) idx, (long long) length);
    xrt_throw_exc(xr_box_str(buf));
}

#endif  // XRT_EXCEPTION_H
