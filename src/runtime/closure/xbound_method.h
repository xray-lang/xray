/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbound_method.h - Bound method value (receiver + native handler)
 *
 * KEY CONCEPT:
 *   XrBoundMethod pairs a receiver value with a direct C handler for
 *   zero-dispatch builtin method calls (e.g. `arr.push`). It is a runtime
 *   callable value, peer of XrClosure, so it belongs in runtime/closure.
 */

#ifndef XBOUND_METHOD_H
#define XBOUND_METHOD_H

#include "../../base/xdefs.h"
#include "../value/xvalue.h"
#include "../gc/xgc_header.h"

struct XrayIsolate;

// Builtin method dispatch signature.
typedef XrValue (*MethodHandler)(struct XrayIsolate *isolate, XrValue receiver, XrValue *args,
                                 int argc);

typedef struct XrBoundMethod {
    XrGCHeader gc;
    XrValue receiver;
    MethodHandler handler;  // direct function pointer, zero-dispatch call
} XrBoundMethod;

XR_FUNC XrBoundMethod *xr_bound_method_new(struct XrayIsolate *isolate, XrValue receiver,
                                           MethodHandler handler);
XR_FUNC XrValue xr_value_from_bound_method(XrBoundMethod *bm);
XR_FUNC XrBoundMethod *xr_value_to_bound_method(XrValue v);
XR_FUNC bool xr_value_is_bound_method(XrValue v);

/* ========== Symbol -> MethodHandler bridges ==========
 *
 * Builtin-method values (e.g. `arr.push`) are produced by the cold
 * GETPROP path: it grabs a MethodHandler for the (receiver type,
 * symbol) pair and wraps it in an XrBoundMethod. The resulting bound
 * method can later be called as a first-class function with no extra
 * dispatch — handler() is invoked directly.
 *
 * Each bridge resolves via XrClass native_type_classes lookup;
 * closure-taking methods (foreach / map / filter / reduce / find / ...)
 * that need a bytecode-interpreting adapter resolve to a stub that
 * returns XR_NOTFOUND.
 *
 * Enum getMember takes an integer index, so it gets its own thin
 * wrapper here. */
XR_FUNC MethodHandler xr_map_get_handler(struct XrayIsolate *isolate, int symbol);
XR_FUNC MethodHandler xr_array_get_handler(struct XrayIsolate *isolate, int symbol);
XR_FUNC MethodHandler xr_set_get_handler(struct XrayIsolate *isolate, int symbol);
XR_FUNC MethodHandler xr_string_get_handler(struct XrayIsolate *isolate, int symbol);

/* Enum.getMember(int) -> XrEnumValue. Exported as a MethodHandler so
 * GETPROP can wrap it in a bound method without a special opcode. */
XR_FUNC XrValue xr_enum_get_member_handler(struct XrayIsolate *isolate, XrValue receiver,
                                           XrValue *args, int argc);

#endif  // XBOUND_METHOD_H
