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
#include "../runtime/value/xtype_names.h" /* XrTypeId + xr_typeid_name (shared with VM) */
#include "../shared/xr_builtin_schema.h"
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
    void *defer_scope_mark;    // xrt_defer_top at try entry (XrtDeferScope*); see xrt_defer.h
    int defer_count_mark;      // pending count inside defer_scope_mark at try entry
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
static inline void xrt_defer_unwind_to_mark(void *scope_mark, int count_mark);

static inline XrValue xrt_exception_message_value(const char *message, size_t len) {
    if (!message)
        return XR_NULL_VAL;
    XrValue s = xrt_str_alloc(len);
    if (len > 0)
        memcpy(xr_str_buf(s), message, len);
    return s;
}

static inline XrValue xrt_exception_new_value(int code, const char *message, size_t len) {
    XrValue exc = xrt_json_new_named(EXCEPTION_FIELD_COUNT, xr_exception_field_names());
    xrt_json_set_field(exc, EXCEPTION_FIELD_MESSAGE, xrt_exception_message_value(message, len));
    xrt_json_set_field(exc, EXCEPTION_FIELD_STACK, xrt_array_new(0));
    xrt_json_set_field(exc, EXCEPTION_FIELD_CAUSE, XR_NULL_VAL);
    xrt_json_set_field(exc, EXCEPTION_FIELD_CODE, XR_FROM_INT(code));
    xrt_json_set_field(exc, EXCEPTION_FIELD_DATA, XR_NULL_VAL);
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

XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc);
XRT_COLD _Noreturn void xrt_index_oob(int64_t idx, int64_t length);
XRT_COLD _Noreturn void xrt_throw_type_mismatch(int64_t expected_tid, int64_t actual_tid);

/* Header-only XrTypeId -> canonical name, mirroring the VM's xr_typeid_name with
 * the same TYPE_NAME_* literals. Inlined into the generated program so standalone
 * AOT (which does not link the runtime value layer) still produces identical
 * user-visible type names. Ids are runtime XrTypeId values (see xrt_typeof_id). */
static inline const char *xrt_type_name(int64_t tid) {
    switch ((XrTypeId) tid) {
        case XR_TID_NULL:
            return TYPE_NAME_NULL;
        case XR_TID_BOOL:
            return TYPE_NAME_BOOL;
        case XR_TID_INT8:
            return TYPE_NAME_INT8;
        case XR_TID_UINT8:
            return TYPE_NAME_UINT8;
        case XR_TID_INT16:
            return TYPE_NAME_INT16;
        case XR_TID_UINT16:
            return TYPE_NAME_UINT16;
        case XR_TID_INT32:
            return TYPE_NAME_INT32;
        case XR_TID_UINT32:
            return TYPE_NAME_UINT32;
        case XR_TID_INT:
            return TYPE_NAME_INT;
        case XR_TID_UINT64:
            return TYPE_NAME_UINT64;
        case XR_TID_FLOAT32:
            return TYPE_NAME_FLOAT32;
        case XR_TID_FLOAT:
            return TYPE_NAME_FLOAT;
        case XR_TID_STRING:
            return TYPE_NAME_STRING;
        case XR_TID_CHAR:
            return TYPE_NAME_CHAR;
        case XR_TID_FUNCTION:
            return TYPE_NAME_FUNCTION;
        case XR_TID_BOUND_METHOD:
            return TYPE_NAME_BOUND_METHOD;
        case XR_TID_ARRAY:
            return TYPE_NAME_ARRAY;
        case XR_TID_SET:
            return TYPE_NAME_SET;
        case XR_TID_MAP:
            return TYPE_NAME_MAP;
        case XR_TID_INSTANCE:
            return TYPE_NAME_INSTANCE;
        case XR_TID_JSON:
            return TYPE_NAME_JSON;
        case XR_TID_RECORD:
            return TYPE_NAME_RECORD;
        case XR_TID_BIGINT:
            return TYPE_NAME_BIGINT;
        case XR_TID_STRINGBUILDER:
            return TYPE_NAME_STRINGBUILDER;
        case XR_TID_CHANNEL:
            return TYPE_NAME_CHANNEL;
        case XR_TID_REGEX:
            return TYPE_NAME_REGEX;
        case XR_TID_DATETIME:
            return TYPE_NAME_DATETIME;
        case XR_TID_PANIC_INFO:
            return TYPE_NAME_PANIC_INFO;
        case XR_TID_ENUM_VALUE:
            return TYPE_NAME_ENUM_VALUE;
        case XR_TID_ENUM_TYPE:
            return TYPE_NAME_ENUM_TYPE;
        case XR_TID_ITERATOR:
            return TYPE_NAME_ITERATOR;
        case XR_TID_MODULE:
            return TYPE_NAME_MODULE;
        case XR_TID_COROUTINE:
            return TYPE_NAME_COROUTINE;
        case XR_TID_RANGE:
            return TYPE_NAME_RANGE;
        case XR_TID_TASK:
            return TYPE_NAME_TASK;
        case XR_TID_NETCONN:
            return TYPE_NAME_NETCONN;
        case XR_TID_NETLISTENER:
            return TYPE_NAME_NETLISTENER;
        case XR_TID_ATOMIC:
            return TYPE_NAME_ATOMIC;
        case XR_TID_WORKQUEUE:
            return TYPE_NAME_WORKQUEUE;
        case XR_TID_RESULTGROUP:
            return TYPE_NAME_RESULTGROUP;
        case XR_TID_WEAKMAP:
            return TYPE_NAME_WEAKMAP;
        case XR_TID_WEAKSET:
            return TYPE_NAME_WEAKSET;
        default:
            return TYPE_NAME_UNKNOWN;
    }
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

#ifdef XRT_IMPL
XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc) {
    exc = xrt_exception_normalize(exc);
    if (xrt_exc_top) {
        /* Caught panic: run the defers of every frame skipped on the way to the
         * handler (down to its recorded mark), then jump. An uncaught panic
         * aborts WITHOUT running defers, matching the VM. */
        xrt_defer_unwind_to_mark(xrt_exc_top->defer_scope_mark, xrt_exc_top->defer_count_mark);
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
XRT_COLD _Noreturn void xrt_index_oob(int64_t idx, int64_t length) {
    char buf[XR_ERROR_CORE_INDEX_OOB_BUFSZ];
    xr_error_core_format_array_index_oob(buf, sizeof(buf), idx, length);
    xrt_throw_exc(xrt_exception_new_value(XR_ERR_INDEX_OUT_OF_BOUNDS, buf, strlen(buf)));
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
