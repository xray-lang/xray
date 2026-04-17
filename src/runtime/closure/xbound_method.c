/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbound_method.c - Bound method allocation and value conversions
 */

#include "xbound_method.h"
#include "../../base/xchecks.h"
#include "../gc/xgc.h"
#include "../xisolate_internal.h"

XrBoundMethod *xr_bound_method_new(XrayIsolate *isolate, XrValue receiver,
                                   MethodHandler handler) {
    XR_DCHECK(isolate != NULL, "bound_method_new: NULL isolate");
    XrBoundMethod *bm = (XrBoundMethod *)xr_gc_alloc(&isolate->gc,
                                                     sizeof(XrBoundMethod),
                                                     XR_TBOUND_METHOD);
    if (bm == NULL) {
        return NULL;
    }
    xr_gc_header_init_type(&bm->gc, XR_TBOUND_METHOD);
    bm->receiver = receiver;
    bm->handler = handler;
    return bm;
}

XrValue xr_value_from_bound_method(XrBoundMethod *bm) {
    return XR_FROM_PTR(bm);
}

bool xr_value_is_bound_method(XrValue v) {
    return XR_IS_BOUND_METHOD(v);
}

XrBoundMethod *xr_value_to_bound_method(XrValue v) {
    if (!XR_IS_BOUND_METHOD(v)) {
        return NULL;
    }
    return (XrBoundMethod *)XR_TO_PTR(v);
}
