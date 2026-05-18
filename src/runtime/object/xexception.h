/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexception.h - Exception object for try-catch-finally
 *
 * KEY CONCEPT:
 *   GC-managed exception with error code, message, stack trace.
 */

#ifndef XEXCEPTION_H
#define XEXCEPTION_H

#include "../value/xvalue.h"
#include "xstring.h"
#include "xarray.h"
#include "../xerror.h"

typedef struct XrException XrException;

struct XrException {
    XrGCHeader gc;
    XrErrorCode code;   // error code
    XrString *message;  // error message

    // Source location
    XrString *file;  // filename (GC managed)
    int line;        // line number
    int column;      // column number

    // Stack trace
    XrArray *stackTrace;  // call stack array (GC managed)

    // Causal chain (the exception that caused this one to be thrown).
    // NULL when this exception is the root cause. Surfaced to user code
    // via the `cause` field on the prelude Exception class so wrapping
    // patterns like `throw new ConfigError("bad", cause: ioErr)` keep
    // the original failure reachable through `e.cause`.
    XrException *cause;

    // User data
    XrValue userData;  // custom data attached by user
};

/* ========== Exception API ========== */

// Create exception with code and message
XR_FUNC XrValue xr_exception_new(XrayIsolate *X, XrErrorCode code, const char *message);

// Create exception with formatted message
XR_FUNC XrValue xr_exception_newf(XrayIsolate *X, XrErrorCode code, const char *fmt, ...);

// Create exception from XrError
XR_FUNC XrValue xr_exception_from_error(XrayIsolate *X, XrError *error);

// Create exception from any value (auto-wrap)
XR_FUNC XrValue xr_exception_from_value(XrayIsolate *X, XrValue value);

/* ========== Exception Info ========== */

XR_FUNC XrErrorCode xr_exception_get_code(XrValue exception);
XR_FUNC const char *xr_exception_get_message(XrValue exception);
XR_FUNC XrValue xr_exception_get_stacktrace(XrayIsolate *iso, XrValue exception);

/* ========== Stack Trace ========== */

// Add stack frame to exception
XR_FUNC void xr_exception_add_frame(XrayIsolate *X, XrValue exception, const char *funcName,
                                    int line);

/* ========== Output ========== */

XR_FUNC void xr_exception_print(XrayIsolate *X, XrValue exception);

/* ========== Native Class Registration ==========
 *
 * Registers the user-facing `Exception` XrClass on the isolate so that
 * user code can write `new Exception("msg")` or `new Exception("msg",
 * cause)` and subclass it via `class HttpError extends Exception`.
 *
 * Called from xr_prelude_register_all_native_types during isolate init.
 * Idempotent: the registry skips re-registration of the same gc_type.
 */
XR_FUNC void xr_exception_register_native_type(XrayIsolate *isolate);

/* User-facing constructor primitive.
 *
 * Signature mirrors the prelude declaration:
 *   constructor(message: string = "", cause: Exception? = null)
 *
 * Allocates a fresh XrException, populates message + cause, and returns
 * the wrapped XrValue. Called both via the registered native static
 * method dispatch and directly from the OP_NEWEXCEPTION fast path so
 * `new Exception("msg")` works without going through the generic
 * "instantiate XrInstance + invoke constructor" pipeline (XrException
 * has a separate GC type, not a generic instance).
 */
XR_FUNC XrValue xr_exception_user_construct(XrayIsolate *isolate, XrValue self, XrValue *args,
                                            int argc);

/* ========== Type Check Macros ========== */

// XR_IS_EXCEPTION is defined in xvalue.h (single source of truth)

#define XR_AS_EXCEPTION(v) ((XrException *) XR_TO_PTR(v))

#define XR_EXCEPTION_VAL(e) XR_FROM_PTR(e)

/* ========== Convenience Macros ========== */

#define xr_is_exception(v) XR_IS_EXCEPTION(v)
#define xr_as_exception(v) XR_AS_EXCEPTION(v)
#define xr_exception_val(e) XR_EXCEPTION_VAL(e)

#endif  // XEXCEPTION_H
