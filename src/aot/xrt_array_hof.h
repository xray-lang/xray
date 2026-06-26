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
#include "../shared/xr_array_core.h"

static inline void xrt_array_write_preallocated(xrt_array_t *a, int64_t index, XrValue value) {
    xrt_array_check_store_or_abort(a, value, "xrt_array_write_preallocated");
    xr_typed_set(a->data, (int32_t) index, value, a->elem_type);
}

typedef XrValue (*xrt_array_hof_fn1_t)(xrt_closure_t *, XrValue);
typedef XrValue (*xrt_array_hof_fn2_t)(xrt_closure_t *, XrValue, XrValue);

typedef struct XrtArrayHofCtx {
    xrt_array_t *input;
    xrt_array_t *output;
    xrt_closure_t *closure;
    xrt_array_hof_fn1_t fn1;
    xrt_array_hof_fn2_t fn2;
} XrtArrayHofCtx;

static inline XrValue xrt_array_hof_read(void *ctx_ptr, int64_t index) {
    XrtArrayHofCtx *ctx = (XrtArrayHofCtx *) ctx_ptr;
    if (!ctx || !ctx->input || index < 0 || index > INT32_MAX)
        return XR_NULL_VAL;
    return xr_typed_get(ctx->input->data, (int32_t) index, ctx->input->elem_type);
}

static inline XrValue xrt_array_hof_map(void *ctx_ptr, XrValue value) {
    XrtArrayHofCtx *ctx = (XrtArrayHofCtx *) ctx_ptr;
    return ctx->fn1(ctx->closure, value);
}

static inline bool xrt_array_hof_each(void *ctx_ptr, XrValue value) {
    XrtArrayHofCtx *ctx = (XrtArrayHofCtx *) ctx_ptr;
    (void) ctx->fn1(ctx->closure, value);
    return true;
}

static inline bool xrt_array_hof_write(void *ctx_ptr, int64_t index, XrValue value) {
    XrtArrayHofCtx *ctx = (XrtArrayHofCtx *) ctx_ptr;
    if (!ctx || !ctx->output || index < 0 || index > INT32_MAX)
        return false;
    xrt_array_write_preallocated(ctx->output, index, value);
    return true;
}

static inline bool xrt_array_hof_filter_predicate(void *ctx_ptr, XrValue value) {
    XrtArrayHofCtx *ctx = (XrtArrayHofCtx *) ctx_ptr;
    return xr_truthy(ctx->fn1(ctx->closure, value));
}

static inline XrValue xrt_array_hof_reduce(void *ctx_ptr, XrValue acc, XrValue value) {
    XrtArrayHofCtx *ctx = (XrtArrayHofCtx *) ctx_ptr;
    return ctx->fn2(ctx->closure, acc, value);
}

static inline XrValue xrt_array_map_typed(XrValue recv, XrValue callback,
                                          uint8_t result_elem_type) {
    if (!XR_IS_ARRAY(recv) || callback.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;
    if (result_elem_type >= XR_ELEM_COUNT)
        result_elem_type = XR_ELEM_ANY;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    XrValue arr = xrt_array_new_typed_uninit(a->length, result_elem_type);
    xrt_array_t *out = (xrt_array_t *) arr.ptr;
    XrtArrayHofCtx ctx = {a, out, cl, (xrt_array_hof_fn1_t) cl->fn, NULL};
    int64_t mapped = 0;
    (void) xr_array_core_hof_map(a->length, xrt_array_hof_read, xrt_array_hof_map,
                                 xrt_array_hof_write, &ctx, &mapped);
    out->length = mapped;
    return arr;
}

static inline XrValue xrt_array_filter_typed(XrValue recv, XrValue callback) {
    if (!XR_IS_ARRAY(recv) || callback.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    XrValue arr = xrt_array_new_typed_uninit(a->length, a->elem_type);
    xrt_array_t *out = (xrt_array_t *) arr.ptr;
    XrtArrayHofCtx ctx = {a, out, cl, (xrt_array_hof_fn1_t) cl->fn, NULL};
    int64_t kept = 0;
    (void) xr_array_core_hof_filter(a->length, xrt_array_hof_read, xrt_array_hof_filter_predicate,
                                    xrt_array_hof_write, &ctx, &kept);
    out->length = kept;
    return arr;
}

static inline XrValue xrt_array_reduce_typed(XrValue recv, XrValue callback, XrValue initial) {
    if (!XR_IS_ARRAY(recv) || callback.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    XrtArrayHofCtx ctx = {a, NULL, cl, NULL, (xrt_array_hof_fn2_t) cl->fn};
    return xr_array_core_hof_reduce(a->length, xrt_array_hof_read, xrt_array_hof_reduce, &ctx,
                                    initial);
}

static inline XrValue xrt_array_for_each_typed(XrValue recv, XrValue callback) {
    if (!XR_IS_ARRAY(recv) || callback.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    XrtArrayHofCtx ctx = {a, NULL, cl, (xrt_array_hof_fn1_t) cl->fn, NULL};
    (void) xr_array_core_hof_for_each(a->length, xrt_array_hof_read, xrt_array_hof_each, &ctx,
                                      NULL);
    return XR_NULL_VAL;
}

static inline XrValue xrt_array_find_typed(XrValue recv, XrValue callback) {
    if (!XR_IS_ARRAY(recv) || callback.tag != XR_TAG_CLOSURE)
        return XR_NULL_VAL;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    XrtArrayHofCtx ctx = {a, NULL, cl, (xrt_array_hof_fn1_t) cl->fn, NULL};
    XrArrayCoreFindResult result =
        xr_array_core_hof_find(a->length, xrt_array_hof_read, xrt_array_hof_filter_predicate, &ctx);
    return result.found ? result.value : XR_NULL_VAL;
}

static inline XrValue xrt_array_find_index_typed(XrValue recv, XrValue callback) {
    if (!XR_IS_ARRAY(recv) || callback.tag != XR_TAG_CLOSURE)
        return XR_FROM_INT(-1);
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    XrtArrayHofCtx ctx = {a, NULL, cl, (xrt_array_hof_fn1_t) cl->fn, NULL};
    return XR_FROM_INT(xr_array_core_hof_find_index(a->length, xrt_array_hof_read,
                                                    xrt_array_hof_filter_predicate, &ctx));
}

static inline XrValue xrt_array_every_typed(XrValue recv, XrValue callback) {
    if (!XR_IS_ARRAY(recv) || callback.tag != XR_TAG_CLOSURE)
        return XR_TRUE_VAL;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    XrtArrayHofCtx ctx = {a, NULL, cl, (xrt_array_hof_fn1_t) cl->fn, NULL};
    return XR_FROM_BOOL(xr_array_core_hof_every(a->length, xrt_array_hof_read,
                                                xrt_array_hof_filter_predicate, &ctx));
}

static inline XrValue xrt_array_some_typed(XrValue recv, XrValue callback) {
    if (!XR_IS_ARRAY(recv) || callback.tag != XR_TAG_CLOSURE)
        return XR_FALSE_VAL;
    xrt_array_t *a = (xrt_array_t *) recv.ptr;
    xrt_closure_t *cl = (xrt_closure_t *) callback.ptr;
    XrtArrayHofCtx ctx = {a, NULL, cl, (xrt_array_hof_fn1_t) cl->fn, NULL};
    return XR_FROM_BOOL(xr_array_core_hof_some(a->length, xrt_array_hof_read,
                                               xrt_array_hof_filter_predicate, &ctx));
}

#endif /* XRT_ARRAY_HOF_H */
