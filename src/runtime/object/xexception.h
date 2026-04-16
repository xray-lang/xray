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
    XrErrorCode code;          // error code
    XrString *message;         // error message
    
    // Source location
    XrString *file;            // filename (GC managed)
    int line;                  // line number
    int column;                // column number
    
    // Stack trace
    XrArray *stackTrace;       // call stack array (GC managed)
    
    // User data
    XrValue userData;          // custom data attached by user
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
XR_FUNC const char* xr_exception_get_message(XrValue exception);
XR_FUNC XrValue xr_exception_get_stacktrace(XrayIsolate *iso, XrValue exception);

/* ========== Stack Trace ========== */

// Add stack frame to exception
XR_FUNC void xr_exception_add_frame(XrayIsolate *X, XrValue exception,
                            const char *funcName, int line);

/* ========== Output ========== */

XR_FUNC void xr_exception_print(XrayIsolate *X, XrValue exception);

/* ========== Type Check Macros ========== */

// XR_IS_EXCEPTION is defined in xvalue.h
#ifndef XR_IS_EXCEPTION
#define XR_IS_EXCEPTION(v) \
    (XR_IS_PTR(v) && XR_GC_GET_TYPE(&((XrGCHeader*)XR_TO_PTR(v))[0]) == XR_TEXCEPTION)
#endif

#define XR_AS_EXCEPTION(v) \
    ((XrException*)XR_TO_PTR(v))

#define XR_EXCEPTION_VAL(e) \
    XR_FROM_PTR(e)

/* ========== Convenience Macros ========== */

#define xr_is_exception(v)    XR_IS_EXCEPTION(v)
#define xr_as_exception(v)    XR_AS_EXCEPTION(v)
#define xr_exception_val(e)   XR_EXCEPTION_VAL(e)

#endif // XEXCEPTION_H
