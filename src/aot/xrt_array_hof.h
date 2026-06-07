/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_array_hof.h - AOT Array higher-order helpers.
 */

#ifndef XRT_ARRAY_HOF_H
#define XRT_ARRAY_HOF_H

#include "xrt_coll.h"

static inline void xrt_array_write_preallocated(xrt_array_t *a, int64_t index, XrValue value) {
    xr_typed_set(a->data, (int32_t) index, value, a->elem_type);
}

static inline XrValue xrt_array_map_typed(XrValue recv, XrValue callback,
                                          uint8_t result_elem_type) {
    if (recv.tag != XR_TAG_ARRAY || callback.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;
    if (result_elem_type >= XR_ELEM_COUNT)
        result_elem_type = XR_ELEM_ANY;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    typedef XrValue (*xrt_fn1_t)(xrt_closure_t *, XrValue);
    xrt_fn1_t fn = (xrt_fn1_t) cl->fn;
    XrValue arr = xrt_array_new_typed_uninit(a->len, result_elem_type);
    xrt_array_t *out = (xrt_array_t *) arr.ptr;
    for (int64_t i = 0; i < a->len; i++) {
        XrValue elem = xr_typed_get(a->data, (int32_t) i, a->elem_type);
        xrt_array_write_preallocated(out, i, fn(cl, elem));
    }
    out->len = a->len;
    return arr;
}

static inline XrValue xrt_array_filter_typed(XrValue recv, XrValue callback) {
    if (recv.tag != XR_TAG_ARRAY || callback.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    typedef XrValue (*xrt_fn1_t)(xrt_closure_t *, XrValue);
    xrt_fn1_t fn = (xrt_fn1_t) cl->fn;
    XrValue arr = xrt_array_new_typed_uninit(a->len, a->elem_type);
    xrt_array_t *out = (xrt_array_t *) arr.ptr;
    for (int64_t i = 0; i < a->len; i++) {
        XrValue elem = xr_typed_get(a->data, (int32_t) i, a->elem_type);
        if (xr_truthy(fn(cl, elem))) {
            xrt_array_write_preallocated(out, out->len, elem);
            out->len++;
        }
    }
    return arr;
}

static inline XrValue xrt_array_reduce_typed(XrValue recv, XrValue callback, XrValue initial) {
    if (recv.tag != XR_TAG_ARRAY || callback.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    typedef XrValue (*xrt_fn2_t)(xrt_closure_t *, XrValue, XrValue);
    xrt_fn2_t fn = (xrt_fn2_t) cl->fn;
    XrValue acc = initial;
    for (int64_t i = 0; i < a->len; i++) {
        XrValue elem = xr_typed_get(a->data, (int32_t) i, a->elem_type);
        acc = fn(cl, acc, elem);
    }
    return acc;
}

#endif /* XRT_ARRAY_HOF_H */
