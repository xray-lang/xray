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

/* =========================================================================
 * xrt_throw - throw an exception value
 *
 * If inside a try block (xrt_exc_top != NULL), stores the exception
 * and longjmps to the nearest frame. Otherwise, prints and aborts.
 * ========================================================================= */

static XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    if (xrt_exc_top) {
        xrt_exc_top->exception = exc;
        longjmp(xrt_exc_top->buf, 1);
    }
    /* Uncaught exception */
    if (exc.tag == XR_TAG_STR || exc.tag == XR_TAG_STR_ARC) {
        fprintf(stderr, "Uncaught exception: %s\n", xr_str_data(exc));
    } else {
        fprintf(stderr, "Uncaught exception (tag=%d)\n", exc.tag);
    }
    abort();
}

#endif  // XRT_EXCEPTION_H
