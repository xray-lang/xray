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

#include "xrt_arith.h" /* xrt_value_to_string, for the uncaught-error diagnostic */
#include "xrt_coll.h"
#include "xrt_value.h"
#include "../runtime/xerror_codes.h"
#include "../runtime/value/xtype_names.h" /* XrTypeId + xr_typeid_name (shared with VM) */
#include "../base/xentry_plan.h"
#include "../shared/xr_builtin_schema.h"
#include "../shared/xr_error_core.h"
#include "../shared/xr_panic_report.h" /* shared uncaught-panic wording with the VM */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h> /* _isatty/_fileno for the panic report's TTY colour gate */
#if defined(__MINGW32__)
/* The generated-C include path also contains the language's net/io.h.  Some
 * MinGW drivers resolve that project header before the CRT's <io.h>, so keep
 * the one CRT declaration used here explicit and provider-independent. */
int __cdecl _isatty(int fd);
#endif
#else
#include <unistd.h> /* isatty for the panic report's TTY colour gate */
#endif

/* =========================================================================
 * Exception frame (stack-allocated in each try block)
 * ========================================================================= */

typedef struct XrtExcFrame {
    jmp_buf buf;                 // setjmp/longjmp target
    volatile XrValue exception;  // exception value (set before longjmp)
    struct XrtExcFrame *prev;    // previous frame in stack
} XrtExcFrame;

/* =========================================================================
 * Thread-local exception stack.
 *
 * AOT code can run on multiple scheduler workers. Exception stack and pending
 * value-return error state therefore must be per OS thread: a panic/error in
 * one worker must not overwrite another worker's in-flight state.
 * ========================================================================= */

#ifdef XRT_IMPL
XR_THREAD_LOCAL XrtExcFrame *xrt_exc_top = NULL;
XR_THREAD_LOCAL XrValue xrt_pending_error = {.tag = XR_TAG_NULL};
/* Nesting depth of cleanup bodies currently running on this thread, and the
 * xrt_exc_top value captured when the innermost one started. Together they
 * answer "would a panic raised right now escape a cleanup body?": only when we
 * are inside one AND no handler has been pushed since it started. A panic the
 * body catches itself stays ordinary — that is the escape hatch spec 8.3.1
 * rule D2 prescribes. Static cleanup regions maintain both values. */
XR_THREAD_LOCAL int xrt_cleanup_depth = 0;
#define XRT_CLEANUP_NESTING_MAX 256
XR_THREAD_LOCAL XrValue xrt_cleanup_saved_errors[XRT_CLEANUP_NESTING_MAX];
XR_THREAD_LOCAL XrtExcFrame *xrt_cleanup_exc_barriers[XRT_CLEANUP_NESTING_MAX];
#else
extern XR_THREAD_LOCAL XrtExcFrame *xrt_exc_top;
extern XR_THREAD_LOCAL XrValue xrt_pending_error;
extern XR_THREAD_LOCAL int xrt_cleanup_depth;
#define XRT_CLEANUP_NESTING_MAX 256
extern XR_THREAD_LOCAL XrValue xrt_cleanup_saved_errors[XRT_CLEANUP_NESTING_MAX];
extern XR_THREAD_LOCAL XrtExcFrame *xrt_cleanup_exc_barriers[XRT_CLEANUP_NESTING_MAX];
#endif

static inline int xrt_has_pending_error(void) {
    return !XR_IS_NULL(xrt_pending_error);
}

/* Uncaught value-return error diagnostic (spec §8.1.1): print the error value to
 * stderr. `in_go_coroutine` selects the wording for a dropped fire-and-forget
 * `go` whose body threw with nothing left to observe it. Kept byte-identical to
 * the VM's report in run_finalize (src/vm/xvm_coro_backend.c) — the two backends
 * must not drift.
 *
 * Header-only because rendering an AOT value needs the generated program's own
 * formatter; the scheduler runtime reaches it through
 * XrAotValueOps::report_uncaught_error. */
static inline XRT_COLD void xrt_report_uncaught_error(XrValue err, bool in_go_coroutine) {
    const char *where = in_go_coroutine ? " in go coroutine" : "";
    XrValue s;
    if (XR_IS_NULL(err))
        return;
    s = xrt_value_to_string(err);
    if (XR_IS_STR(s))
        fprintf(stderr, "\n[Uncaught Error%s] %.*s\n", where, (int) xr_str_len(s), xr_str_data(s));
    else
        fprintf(stderr, "\n[Uncaught Error%s] <error>\n", where);
}

static inline XrValue xrt_exception_message_value(const char *message, size_t len) {
    if (!message)
        return XR_NULL_VAL;
    XrValue s = xrt_str_alloc(len);
    if (len > 0)
        memcpy(xr_str_buf(s), message, len);
    return s;
}

static inline XrValue xrt_exception_new_value(int code, const char *message, size_t len) {
    XrValue exc = xrt_struct_object_new_named(EXCEPTION_FIELD_COUNT, xr_exception_field_names());
    xrt_object_set_field(exc, EXCEPTION_FIELD_MESSAGE, xrt_exception_message_value(message, len));
    xrt_object_set_field(exc, EXCEPTION_FIELD_STACK, xrt_array_new(0));
    xrt_object_set_field(exc, EXCEPTION_FIELD_CAUSE, XR_NULL_VAL);
    xrt_object_set_field(exc, EXCEPTION_FIELD_CODE, XR_FROM_INT(code));
    xrt_object_set_field(exc, EXCEPTION_FIELD_DATA, XR_NULL_VAL);
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
        return xrt_object_get_name(exc, "message");
    return XR_NULL_VAL;
}

static inline const char *xrt_exception_message_cstr(XrValue exc) {
    XrValue msg = xrt_exception_get_message_value(exc);
    return XR_IS_STR(msg) ? xr_str_data(msg) : NULL;
}

static inline _Noreturn void xrt_cleanup_abort(XrValue escaped, XrValue in_flight) {
    xr_error_core_defer_throw_abort(XR_ERR_DEFER_THROW, xrt_exception_message_cstr(escaped),
                                    XR_IS_NULL(in_flight) ? NULL
                                                          : xrt_exception_message_cstr(in_flight));
}

static inline void xrt_cleanup_enter(void) {
    if (xrt_cleanup_depth < 0 || xrt_cleanup_depth >= XRT_CLEANUP_NESTING_MAX)
        xrt_cleanup_abort(XR_NULL_VAL, xrt_pending_error);
    int depth = xrt_cleanup_depth;
    xrt_cleanup_saved_errors[depth] = xrt_pending_error;
    xrt_cleanup_exc_barriers[depth] = xrt_exc_top;
    xrt_pending_error = XR_NULL_VAL;
    xrt_cleanup_depth = depth + 1;
}

static inline void xrt_cleanup_err_check(void) {
    if (xrt_cleanup_depth > 0 && xrt_has_pending_error())
        xrt_cleanup_abort(xrt_pending_error, xrt_cleanup_saved_errors[xrt_cleanup_depth - 1]);
}

static inline void xrt_cleanup_leave(void) {
    if (xrt_cleanup_depth <= 0)
        xrt_cleanup_abort(XR_NULL_VAL, xrt_pending_error);
    xrt_cleanup_err_check();
    int depth = xrt_cleanup_depth - 1;
    xrt_pending_error = xrt_cleanup_saved_errors[depth];
    xrt_cleanup_saved_errors[depth] = XR_NULL_VAL;
    xrt_cleanup_exc_barriers[depth] = NULL;
    xrt_cleanup_depth = depth;
}

/* Fault code carried by a normalized exception object; 0 when absent. A bare
 * string exception still encodes it as an "E0420: " prefix, so parse that too
 * for the pre-normalize path. */
static inline int xrt_exception_get_code(XrValue exc) {
    if (exc.tag == XR_TAG_PTR && exc.ptr && exc.heap_type == 0) {
        XrValue code = xrt_object_get_name(exc, "code");
        if (XR_IS_INT(code))
            return (int) XR_TO_INT(code);
    }
    if (XR_IS_STR(exc)) {
        XrErrorCoreMessageView view =
            xr_error_core_parse_prefixed(xr_str_data(exc), (size_t) xr_str_len(exc));
        if (view.has_code)
            return view.code;
    }
    return 0;
}

/* stderr TTY gate for the panic report's colour, matching the VM's
 * XR_COLOR_SUPPORTED (isatty on stderr) so piped output stays plain on both. */
static inline bool xrt_stderr_is_tty(void) {
#if defined(_WIN32)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc);

static inline void xrt_guard_task_spawn(void) {
    static const char message[] = "a deferred action cannot create a task or reach the scheduler";
    if (xrt_cleanup_depth > 0)
        xrt_throw_exc(xrt_exception_new_value(XR_ERR_DEFER_ASYNC, message, sizeof(message) - 1u));
}

static inline void xrt_guard_callable_effects(const XrAotCallableDesc *callable) {
    if (xrt_cleanup_depth == 0 || !callable)
        return;
    if ((callable->effect_bits & (XR_EFFECT_MAY_SUSPEND | XR_EFFECT_MAY_SPAWN)) != 0)
        xrt_guard_task_spawn();
}

static inline int64_t xrt_expect_int_arg(XrValue value) {
    if (XR_UNLIKELY(!XR_IS_INT(value)))
        xrt_throw_exc(xr_box_str("E0404: expected int argument"));
    return XR_TO_INT(value);
}

static inline int64_t xrt_expect_bool_arg(XrValue value) {
    if (XR_UNLIKELY(!XR_IS_BOOL(value)))
        xrt_throw_exc(xr_box_str("E0404: expected bool argument"));
    return XR_TO_INT(value);
}

static inline int64_t xrt_expect_rune_arg(XrValue value) {
    if (XR_UNLIKELY(!XR_IS_RUNE(value)))
        xrt_throw_exc(xr_box_str("E0404: expected rune argument"));
    return (int64_t) XR_TO_RUNE(value);
}
XRT_COLD _Noreturn void xrt_index_oob(int64_t idx, int64_t length);
XRT_COLD _Noreturn void xrt_fixed_index_oob(int64_t idx, int64_t length);
XRT_COLD _Noreturn void xrt_type_no_index(const char *message);
XRT_COLD _Noreturn void xrt_throw_type_mismatch(int64_t expected_tid, int64_t actual_tid);

/* CHECKTYPE preserves the operand's borrowed identity. Keep the check in a
 * typed helper so generated code evaluates the operand once without relying on
 * a provider-specific statement expression. */
static inline XrValue xrt_check_type_borrowed(XrValue value, int64_t expected_tid,
                                              bool allow_null) {
    int64_t actual_tid = xrt_typeof_id(value);
    if (xrt_value_is_type_id(value, expected_tid) ||
        (allow_null && actual_tid == XR_TID_NULL))
        return value;
    xrt_throw_type_mismatch(expected_tid, actual_tid);
    return XR_NULL_VAL;
}

/* A checked cast borrows its operand and therefore must establish a distinct
 * owner when it returns the same runtime object.  Keep this header-inline so
 * every separately compiled AOT module has an exact prototype and body. */
static inline XrValue xrt_as_owned(XrValue value, int64_t expected_tid, bool is_safe) {
    if (xrt_value_is_type_id(value, expected_tid))
        return xrt_retain_identity(value);
    if (is_safe)
        return XR_NULL_VAL;
    xrt_throw_type_mismatch(expected_tid, xrt_typeof_id(value));
    return XR_NULL_VAL;
}

/* XrTypeId -> canonical name. The mapping itself lives in the shared kernel
 * xr_type_names_core.h, which is header-only, so standalone AOT (which links no
 * runtime value layer) resolves the same names the VM does without a second
 * table to keep in step. Ids are runtime XrTypeId values (see xrt_typeof_id). */
static inline const char *xrt_type_name(int64_t tid) {
    return xr_type_name_from_tid((XrTypeId) tid);
}

/* =========================================================================
 * xrt_throw - throw an exception value
 *
 * If inside a try block (xrt_exc_top != NULL), stores the exception
 * and longjmps to the nearest frame. Otherwise, prints and aborts.
 *
 * Cleanup is represented directly in generated control flow. Hidden cleanup
 * handlers run the statically known LIFO frontier before rethrowing.
 * ========================================================================= */

#ifdef XRT_IMPL
XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    exc = xrt_exception_normalize(exc);
    /* Spec 8.3.1 rule D3: a panic that would escape a defer body is a cleanup
     * failure, not a recoverable condition — unwinding past a half-run cleanup
     * would leave the resource state unknown, so not even an enclosing
     * `catch panic` may intercept it. A handler pushed inside the body moves
     * xrt_exc_top past the barrier and keeps the panic ordinary. */
    if (xrt_cleanup_depth > 0 && xrt_exc_top == xrt_cleanup_exc_barriers[xrt_cleanup_depth - 1])
        xr_error_core_defer_throw_abort(
            XR_ERR_DEFER_THROW, xrt_exception_message_cstr(exc),
            XR_IS_NULL(xrt_cleanup_saved_errors[xrt_cleanup_depth - 1])
                ? NULL
                : xrt_exception_message_cstr(xrt_cleanup_saved_errors[xrt_cleanup_depth - 1]));
    if (xrt_exc_top) {
        xrt_exc_top->exception = exc;
        longjmp(xrt_exc_top->buf, 1);
    }
    /* Uncaught panic: emit the shared report and exit(1) — matching the VM's
     * wording (shared/xr_panic_report.h) and its clean exit code (not a
     * SIGABRT/134 core dump). No stack trace: the native path carries no unwind
     * state, and the VM gates its own trace behind XRAY_BACKTRACE precisely so
     * the default output stays identical across backends. */
    const char *message = xrt_exception_message_cstr(exc);
    xr_panic_report_emit(stderr, xrt_exception_get_code(exc), message, xrt_stderr_is_tty());
    exit(1);
}

/* Array index out of bounds → E0430 (spec §3 expr-index-access), matching the
 * VM's XR_ERR_INDEX_OUT_OF_BOUNDS. Lives here (not in xrt_coll.h) because raising
 * a panic is an exception-layer concern; xrt_coll.h's index helpers and the
 * generated typed/fixed-array reads call it via a forward declaration. */
XRT_COLD _Noreturn void xrt_index_oob(int64_t idx, int64_t length) {
    char buf[XR_ERROR_CORE_INDEX_OOB_BUFSZ];
    xr_error_core_format_array_index_oob(buf, sizeof(buf), idx, length);
    xrt_throw_exc(xrt_exception_new_value(XR_ERR_INDEX_OUT_OF_BOUNDS, buf, strlen(buf)));
}

/* Fixed (stack) array index OOB → E0430, same catchable panic + message shape as
 * the VM (a fixed array OOB must not abort the process). */
XRT_COLD _Noreturn void xrt_fixed_index_oob(int64_t idx, int64_t length) {
    char buf[XR_ERROR_CORE_INDEX_OOB_BUFSZ];
    xr_error_core_format_fixed_array_index_oob(buf, sizeof(buf), idx, length);
    xrt_throw_exc(xrt_exception_new_value(XR_ERR_INDEX_OUT_OF_BOUNDS, buf, strlen(buf)));
}

/* Unsupported index operation -> E0402, matching the VM generic OP_INDEX_GET /
 * OP_INDEX_SET fallback. */
XRT_COLD _Noreturn void xrt_type_no_index(const char *message) {
    if (!message)
        message = "type does not support indexing";
    xrt_throw_exc(xrt_exception_new_value(XR_ERR_TYPE_NO_INDEX, message, strlen(message)));
}

/* Type mismatch (unsafe `as`, dynamic→concrete checktype) → E0404, identical
 * message + code to the VM OP_CHECKTYPE path. Type ids are runtime XrTypeId
 * values (as produced by xrt_typeof_id), so xr_typeid_name maps them exactly as
 * the VM does. */
XRT_COLD _Noreturn void xrt_throw_type_mismatch(int64_t expected_tid, int64_t actual_tid) {
    char buf[XR_ERROR_CORE_TYPE_MISMATCH_BUFSZ];
    xr_error_core_format_type_mismatch(buf, sizeof(buf), xrt_type_name(expected_tid),
                                       xrt_type_name(actual_tid));
    xrt_throw_exc(xrt_exception_new_value(XR_ERR_TYPE_MISMATCH, buf, strlen(buf)));
}

#endif

#endif  // XRT_EXCEPTION_H
