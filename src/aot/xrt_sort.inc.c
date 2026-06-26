/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_sort.inc.c - AOT Array sort adapter (included by xrt_method.h).
 */

#include "../shared/xr_sort_core.h"

typedef struct {
    xrt_closure_t *cl;
} XrtSortCtx;

static inline int xrt_sort_cmp_default(XrValue a, XrValue b) {
    if (XR_IS_STR(a) && XR_IS_STR(b)) {
        return xr_sort_core_compare_default(a, b, xr_str_data(a), xr_str_len(a), xr_str_data(b),
                                            xr_str_len(b));
    }
    return xr_sort_core_compare_default(a, b, NULL, 0, NULL, 0);
}

static inline int xrt_sort_cmp_adapter(void *ctx_ptr, XrValue a, XrValue b) {
    XrtSortCtx *ctx = (XrtSortCtx *) ctx_ptr;
    if (ctx && ctx->cl) {
        typedef XrValue (*xrt_fn2_t)(xrt_closure_t *, XrValue, XrValue);
        return xr_sort_core_compare_result(((xrt_fn2_t) ctx->cl->fn)(ctx->cl, a, b));
    }
    return xrt_sort_cmp_default(a, b);
}

/* Entry point: in-place sort, optional comparator closure.
 * Typed arrays sort their native buffers directly when no comparator is given.
 * With a comparator, typed elements round-trip through boxed scratch so the
 * callback sees ordinary XrValue arguments. */
static XrValue xrt_array_sort(XrValue recv, xrt_closure_t *cl) {
    if (!XR_IS_ARRAY(recv) || !recv.ptr)
        return recv;
    xrt_array_t *arr = (xrt_array_t *) recv.ptr;
    int64_t n = arr->length;
    if (n < 2)
        return recv;

    if (!cl && xr_sort_core_typed(arr->data, n, arr->elem_type))
        return recv;

    XrtSortCtx ctx = {cl};
    if (arr->elem_type == XR_ELEM_ANY) {
        xr_sort_core_values((XrValue *) arr->data, n, xrt_sort_cmp_adapter, &ctx);
        return recv;
    }

    XrValue *tmp = (XrValue *) XRT_MALLOC((size_t) n * sizeof(XrValue));
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_array_sort: out of memory\n");
        abort();
    }
    for (int64_t i = 0; i < n; i++)
        tmp[i] = xr_typed_get(arr->data, (int32_t) i, arr->elem_type);
    xr_sort_core_values(tmp, n, xrt_sort_cmp_adapter, &ctx);
    for (int64_t i = 0; i < n; i++)
        xr_typed_set(arr->data, (int32_t) i, tmp[i], arr->elem_type);
    XRT_FREE(tmp);
    return recv;
}
