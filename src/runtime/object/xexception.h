/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xexception.h - Exception API on top of the unified XrInstance class
 *
 * KEY CONCEPT:
 *   Exception is a regular Xray class registered into core->exceptionClass
 *   by xr_register_exception_class (xclass_system.c). It has no dedicated
 *   GC type, no dedicated struct, and no dedicated bytecode opcode.
 *   Subclasses participate in throw/catch via the standard super-class
 *   chain — xr_value_is_exception walks XrClass->primary_supers.
 *
 * MEMORY:
 *   Allocation goes through xr_instance_new(X, core->exceptionClass).
 *   Each field lives at the index baked into EXCEPTION_FIELD_* (declared
 *   in xclass_system.h). The C runtime writes those indices directly;
 *   user code accesses them through the standard property pipeline.
 */

#ifndef XEXCEPTION_H
#define XEXCEPTION_H

#include "../value/xvalue.h"
#include "xstring.h"
#include "xarray.h"
#include "../xerror.h"

/* ========== Type Check ==========
 *
 * Walks XrInstance->klass through xr_class_instanceof against
 * core->exceptionClass. Returns false for non-instances and for
 * instances whose class chain doesn't reach Exception.
 */
XR_FUNC bool xr_value_is_exception(XrayIsolate *X, XrValue v);

/* ========== Exception Construction ========== */

// Allocate a fresh Exception instance with code + message; stack starts
// as an empty Array<string>, cause = null, data = null. Used by VM error
// paths (OOM, type mismatch, etc.) that must succeed without re-entering
// closure dispatch.
XR_FUNC XrValue xr_exception_new(XrayIsolate *X, XrErrorCode code, const char *message);

// Same but accepts a printf-style format.
XR_FUNC XrValue xr_exception_newf(XrayIsolate *X, XrErrorCode code, const char *fmt, ...);

// Build an Exception from an XrError. message is moved; the original
// XrError is not freed (caller's lifetime).
XR_FUNC XrValue xr_exception_from_error(XrayIsolate *X, XrError *error);

// If `value` is already an Exception (or subclass), returns it. If it's
// an XrError, lifts it. Otherwise wraps `value` in a new Exception with
// data field set to the original value so catch handlers can recover it.
XR_FUNC XrValue xr_exception_from_value(XrayIsolate *X, XrValue value);

/* ========== Exception Field Accessors ==========
 *
 * Each helper expects an Exception (or subclass) value; non-exceptions
 * return a benign default. C consumers prefer these over direct field
 * indexing because they keep the index constants encapsulated.
 */

XR_FUNC XrErrorCode xr_exception_get_code(XrayIsolate *X, XrValue exception);
XR_FUNC const char *xr_exception_get_message(XrayIsolate *X, XrValue exception);
XR_FUNC XrValue xr_exception_get_stacktrace(XrayIsolate *X, XrValue exception);
XR_FUNC XrValue xr_exception_get_data(XrayIsolate *X, XrValue exception);

/* ========== Stack Trace ========== */

// Append "at <funcName> (line <line>)" to the exception's stack array.
// Lazily creates the stack array if it's null.
XR_FUNC void xr_exception_add_frame(XrayIsolate *X, XrValue exception, const char *funcName,
                                    int line);

/* ========== Output ========== */

XR_FUNC void xr_exception_print(XrayIsolate *X, XrValue exception);

/* ========== Class Registration ========== */

// Build the Exception XrClass and store it in core->exceptionClass.
// Must be called from xr_core_init after Object is registered.
XR_FUNC void xr_register_exception_class(XrayIsolate *X);

#endif  // XEXCEPTION_H
