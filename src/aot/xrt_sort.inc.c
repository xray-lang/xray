/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_sort.inc.c - AOT Array sort representation adapter.
 *
 * The observable ordering and introsort algorithm live in xr_sort_core.h.
 * This file only bridges AOT strings, closures, and typed array storage.
 */

#include "../shared/xr_sort_core.h"

typedef struct {
    xrt_closure_t *closure;
} XrtSortContext;

static inline int xrt_sort_compare_default(XrValue a, XrValue b) {
    if (XR_IS_STR(a) && XR_IS_STR(b)) {
        return xr_sort_core_compare_default(a, b, xr_str_data(a), xr_str_len(a), xr_str_data(b),
                                            xr_str_len(b));
    }
    return xr_sort_core_compare_default(a, b, NULL, 0, NULL, 0);
}

static inline int xrt_sort_compare_adapter(void *context, XrValue a, XrValue b) {
    XrtSortContext *sort_context = (XrtSortContext *) context;
    if (sort_context && sort_context->closure) {
        typedef XrValue (*XrtSortFn)(xrt_closure_t *, XrValue, XrValue);
        xrt_closure_t *closure = sort_context->closure;
        XrValue result = ((XrtSortFn) closure->callable->sync_entry)(closure, a, b);
        return xr_sort_core_compare_result(result);
    }
    return xrt_sort_compare_default(a, b);
}

/* A default typed sort stays entirely in native storage. A custom comparator
 * necessarily observes language values, so the adapter boxes one scratch
 * array and writes the final order back after the canonical kernel returns. */
static XrValue xrt_array_sort(XrValue receiver, xrt_closure_t *comparator) {
    if (!XR_IS_ARRAY(receiver) || !receiver.ptr)
        return receiver;

    xrt_array_t *array = (xrt_array_t *) receiver.ptr;
    const int64_t length = array->length;
    if (length < 2)
        return receiver;

    if (!comparator && xr_sort_core_typed(array->data, length, array->elem_type))
        return receiver;

    XrtSortContext context = {.closure = comparator};
    if (array->elem_type == XR_ELEM_ANY) {
        xr_sort_core_values((XrValue *) array->data, length, xrt_sort_compare_adapter, &context);
        return receiver;
    }

    if (!comparator) {
        fprintf(stderr, "xrt_array_sort: unsupported typed element representation\n");
        abort();
    }

    XrValue *scratch = (XrValue *) XRT_MALLOC((size_t) length * sizeof(XrValue));
    if (XR_UNLIKELY(!scratch)) {
        fprintf(stderr, "xrt_array_sort: out of memory\n");
        abort();
    }
    for (int64_t i = 0; i < length; i++)
        scratch[i] = xr_typed_get(array->data, (int32_t) i, array->elem_type);
    xr_sort_core_values(scratch, length, xrt_sort_compare_adapter, &context);
    for (int64_t i = 0; i < length; i++)
        xr_typed_set(array->data, (int32_t) i, scratch[i], array->elem_type);
    XRT_FREE(scratch);
    return receiver;
}
