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
typedef XrValue (*MethodHandler)(struct XrayIsolate *isolate, XrValue receiver,
                                 XrValue *args, int argc);

typedef struct XrBoundMethod {
    XrGCHeader gc;
    XrValue receiver;
    MethodHandler handler;  // direct function pointer, zero-dispatch call
} XrBoundMethod;

XR_FUNC XrBoundMethod *xr_bound_method_new(struct XrayIsolate *isolate,
                                           XrValue receiver, MethodHandler handler);
XR_FUNC XrValue xr_value_from_bound_method(XrBoundMethod *bm);
XR_FUNC XrBoundMethod *xr_value_to_bound_method(XrValue v);
XR_FUNC bool xr_value_is_bound_method(XrValue v);

#endif // XBOUND_METHOD_H
