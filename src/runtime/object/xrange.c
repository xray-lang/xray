/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrange.c - Lightweight lazy Range implementation
 */

#include "xrange.h"
#include "xarray.h"
#include "../gc/xgc.h"
#include "../../base/xchecks.h"

/* ========== Creation ========== */

XrRange* xr_range_new(struct XrCoroutine *coro, int64_t start, int64_t end) {
    XR_DCHECK(coro != NULL, "xr_range_new: coro must not be NULL");
    XrRange *r = (XrRange*)xr_alloc(coro, sizeof(XrRange), XR_TRANGE);
    if (!r) return NULL;
    r->start = start;
    r->end = end;
    r->step = 1;
    return r;
}

XrRange* xr_range_new_with_step(struct XrCoroutine *coro, int64_t start, int64_t end, int64_t step) {
    XR_DCHECK(coro != NULL, "xr_range_new_with_step: coro must not be NULL");
    XR_CHECK(step != 0, "xr_range_new_with_step: step must not be zero");
    XrRange *r = (XrRange*)xr_alloc(coro, sizeof(XrRange), XR_TRANGE);
    if (!r) return NULL;
    r->start = start;
    r->end = end;
    r->step = step;
    return r;
}

/* ========== Conversion ========== */

XrValue xr_range_to_array(struct XrCoroutine *coro, XrRange *r) {
    XR_DCHECK(coro != NULL, "xr_range_to_array: coro must not be NULL");
    if (!r) return xr_null();

    int64_t len = xr_range_length(r);
    if (len <= 0) {
        XrArray *arr = xr_array_with_capacity(coro, 0);
        return xr_value_from_array(arr);
    }

    // Safety cap to prevent OOM (10M elements ~= 160MB for XrValue array)
    XR_CHECK(len <= 10000000, "range_to_array: range too large");

    XrArray *arr = xr_array_with_capacity(coro, (int32_t)len);
    if (!arr) return xr_null();

    XrValue *data = (XrValue*)arr->data;
    int64_t val = r->start;
    for (int64_t i = 0; i < len; i++) {
        data[i] = xr_int(val);
        val += r->step;
    }
    arr->length = (int32_t)len;

    return xr_value_from_array(arr);
}
