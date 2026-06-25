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

#include "xrt_coll.h"
#include "xrt_value.h"
#include "../runtime/xerror_codes.h"
#include "../shared/xr_error_core.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static inline XrValue xrt_exception_message_value(const char *message, size_t len) {
    if (!message)
        return XR_NULL_VAL;
    XrValue s = xrt_str_alloc(len);
    if (len > 0)
        memcpy(xr_str_buf(s), message, len);
    return s;
}

static inline XrValue xrt_exception_new_value(int code, const char *message, size_t len) {
    static const char *const fields[] = {"message", "stack", "cause", "code", "data"};
    XrValue exc = xrt_json_new_named(5, fields);
    xrt_json_set_field(exc, 0, xrt_exception_message_value(message, len));
    xrt_json_set_field(exc, 1, xrt_array_new_len(0));
    xrt_json_set_field(exc, 2, XR_NULL_VAL);
    xrt_json_set_field(exc, 3, XR_FROM_INT(code));
    xrt_json_set_field(exc, 4, XR_NULL_VAL);
    return exc;
}

static inline XrValue xrt_exception_from_message_value(XrValue message) {
    if (XR_IS_STR(message)) {
        const char *data = xr_str_data(message);
        size_t len = (size_t) xr_str_len(message);
        XrErrorCoreMessageView view = xr_error_core_parse_prefixed(data, len);
        return xrt_exception_new_value(view.has_code ? view.code : 0, data, len);
    }
    return xrt_exception_new_value(0, NULL, 0);
}

static inline XrValue xrt_exception_normalize(XrValue exc) {
    if (XR_IS_STR(exc)) {
        const char *data = xr_str_data(exc);
        size_t len = (size_t) xr_str_len(exc);
        XrErrorCoreMessageView view = xr_error_core_parse_prefixed(data, len);
        return xrt_exception_new_value(view.has_code ? view.code : 0, view.message,
                                       view.message_len);
    }
    return exc;
}

static inline XrValue xrt_exception_get_message_value(XrValue exc) {
    if (XR_IS_STR(exc))
        return exc;
    if (exc.tag == XR_TAG_PTR && exc.ptr && exc.heap_type == 0)
        return xrt_json_get_name(exc, "message");
    return XR_NULL_VAL;
}

static inline const char *xrt_exception_message_cstr(XrValue exc) {
    XrValue msg = xrt_exception_get_message_value(exc);
    return XR_IS_STR(msg) ? xr_str_data(msg) : NULL;
}

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
    exc = xrt_exception_normalize(exc);
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
    const char *message = xrt_exception_message_cstr(exc);
    if (message) {
        fprintf(stderr, "Uncaught exception: %s\n", message);
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
    char buf[XR_ERROR_CORE_INDEX_OOB_BUFSZ];
    xr_error_core_format_array_index_oob(buf, sizeof(buf), idx, length);
    xrt_throw_exc(xrt_exception_new_value(XR_ERR_INDEX_OUT_OF_BOUNDS, buf, strlen(buf)));
}

#endif  // XRT_EXCEPTION_H
