/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexception.c - Exception object implementation
 *
 * KEY CONCEPT:
 *   GC-managed exception with error code, message, stack trace.
 */

#include "xexception.h"
#include "../../base/xchecks.h"
#include "../xerror_impl.h"
#include "../xisolate_api.h"
#include "../gc/xgc.h"
#include "../../base/xmalloc.h"
#include "xstring.h"
#include "xarray.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ========== Exception Creation ========== */

// Allocate exception object (internal)
static XrException* xr_exception_alloc(XrayIsolate *X) {
    (void)X;
    XrException *exc = XR_ALLOCATE(XrException);
    if (!exc) return NULL;
    
    xr_gc_header_init_type(&exc->gc, XR_TEXCEPTION);
    
    exc->code = 0;
    exc->message = NULL;
    exc->file = NULL;
    exc->line = 0;
    exc->column = 0;
    exc->stackTrace = NULL;
    exc->userData = xr_null();
    
    return exc;
}

// Create exception object
XrValue xr_exception_new(XrayIsolate *X, XrErrorCode code, const char *message) {
    XR_DCHECK(X != NULL, "exception_new: NULL isolate");
    XrException *exc = xr_exception_alloc(X);
    if (!exc) {
        return xr_null();
    }
    
    exc->code = code;
    exc->message = xr_string_intern(X, message, strlen(message), 0);
    
    // Create empty stack trace array
    exc->stackTrace = xr_array_new(xr_current_coro(X));
    
    return XR_EXCEPTION_VAL(exc);
}

// Create exception with formatted message
XrValue xr_exception_newf(XrayIsolate *X, XrErrorCode code, const char *fmt, ...) {
    char buffer[512];
    va_list args;
    
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    return xr_exception_new(X, code, buffer);
}

// Create exception from XrError
XrValue xr_exception_from_error(XrayIsolate *X, XrError *error) {
    XR_DCHECK(X != NULL, "exception_from_error: NULL isolate");
    if (!error) {
        return xr_exception_new(X, XR_ERR_UNKNOWN, "Unknown error");
    }
    
    XrException *exc = xr_exception_alloc(X);
    if (!exc) {
        return xr_null();
    }
    
    exc->code = error->code;
    exc->message = error->message;  // Direct reference, both GC managed
    exc->line = error->line;
    exc->column = error->column;
    
    exc->stackTrace = xr_array_new(xr_current_coro(X));
    
    return XR_EXCEPTION_VAL(exc);
}

// Create exception from any value (auto-wrap)
XrValue xr_exception_from_value(XrayIsolate *X, XrValue value) {
    XR_DCHECK(X != NULL, "exception_from_value: NULL isolate");
    // Already an exception, return directly
    if (XR_IS_EXCEPTION(value)) {
        return value;
    }
    
    // If error object, convert
    if (XR_IS_PTR(value)) {
        XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(value);
        if (XR_GC_GET_TYPE(gc) == XR_TERROR) {
            return xr_exception_from_error(X, (XrError*)gc);
        }
    }
    
    // Other values: wrap and save original to userData
    XrValue exc = xr_exception_new(X, XR_ERR_RUNTIME, "Value thrown as exception");
    
    // Save original value for catch to retrieve
    if (XR_IS_EXCEPTION(exc)) {
        XrException *ex = XR_AS_EXCEPTION(exc);
        ex->userData = value;
    }
    
    return exc;
}

/* ========== Exception Info ========== */

// Get exception error code
XrErrorCode xr_exception_get_code(XrValue exception) {
    if (!XR_IS_EXCEPTION(exception)) {
        return XR_ERR_UNKNOWN;
    }
    XrException *exc = XR_AS_EXCEPTION(exception);
    return exc->code;
}

// Get exception message
const char* xr_exception_get_message(XrValue exception) {
    if (!XR_IS_EXCEPTION(exception)) {
        return "Not an exception";
    }
    XrException *exc = XR_AS_EXCEPTION(exception);
    if (!exc->message) {
        return "";
    }
    return exc->message->data;
}

// Get exception stack trace
XrValue xr_exception_get_stacktrace(XrayIsolate *iso, XrValue exception) {
    if (!XR_IS_EXCEPTION(exception)) {
        return xr_null();
    }
    XrException *exc = XR_AS_EXCEPTION(exception);
    if (!exc->stackTrace) {
        exc->stackTrace = xr_array_new(xr_current_coro(iso));
    }
    return xr_value_from_array(exc->stackTrace);
}

/* ========== Stack Trace Operations ========== */

// Add stack frame to exception
void xr_exception_add_frame(XrayIsolate *X, XrValue exception,
                            const char *funcName, int line) {
    XR_DCHECK(X != NULL, "exception_add_frame: NULL isolate");
    XR_DCHECK(funcName != NULL, "exception_add_frame: NULL funcName");
    if (!XR_IS_EXCEPTION(exception)) {
        return;
    }
    
    XrException *exc = XR_AS_EXCEPTION(exception);
    if (!exc->stackTrace) {
        exc->stackTrace = xr_array_new(xr_current_coro(X));
    }
    
    // Create stack frame string: "at funcName (line X)"
    char frameStr[256];
    snprintf(frameStr, sizeof(frameStr), "at %s (line %d)", funcName, line);
    
    XrString *frameString = xr_string_intern(X, frameStr, strlen(frameStr), 0);
    xr_array_push(exc->stackTrace, xr_string_value(frameString));
}

/* ========== Exception Output ========== */

// Print exception info
void xr_exception_print(XrayIsolate *X, XrValue exception) {
    (void)X;
    if (!XR_IS_EXCEPTION(exception)) {
        fprintf(stderr, "Error: Not an exception object\n");
        return;
    }
    
    XrException *exc = XR_AS_EXCEPTION(exception);
    
    // Print error code and message
    fprintf(stderr, "\033[1;31mUncaught Exception [%d]:\033[0m %s\n",
            exc->code, 
            exc->message ? exc->message->data : "");
    
    // Print location info
    if (exc->file) {
        fprintf(stderr, "  at %s:%d:%d\n", 
                exc->file->data, exc->line, exc->column);
    }
    
    // Print stack trace
    if (exc->stackTrace && exc->stackTrace->length > 0) {
        fprintf(stderr, "\nStack trace:\n");
        for (int i = 0; i < exc->stackTrace->length; i++) {
            XrValue frame = ((XrValue*)exc->stackTrace->data)[i];
            if (XR_IS_STRING(frame)) {
                XrString *str = (XrString*)XR_TO_PTR(frame);
                fprintf(stderr, "  %s\n", str->data);
            }
        }
    }
}

