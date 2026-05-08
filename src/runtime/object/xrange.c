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

XrRange *xr_range_new(struct XrCoroutine *coro, int64_t start, int64_t end) {
    XR_DCHECK(coro != NULL, "xr_range_new: coro must not be NULL");
    XrRange *r = (XrRange *) xr_alloc(coro, sizeof(XrRange), XR_TRANGE);
    if (!r)
        return NULL;
    r->start = start;
    r->end = end;
    r->step = 1;
    return r;
}

XrRange *xr_range_new_with_step(struct XrCoroutine *coro, int64_t start, int64_t end,
                                int64_t step) {
    XR_DCHECK(coro != NULL, "xr_range_new_with_step: coro must not be NULL");
    XR_CHECK(step != 0, "xr_range_new_with_step: step must not be zero");
    XrRange *r = (XrRange *) xr_alloc(coro, sizeof(XrRange), XR_TRANGE);
    if (!r)
        return NULL;
    r->start = start;
    r->end = end;
    r->step = step;
    return r;
}

/* ========== Conversion ========== */

XrValue xr_range_to_array(struct XrCoroutine *coro, XrRange *r) {
    XR_DCHECK(coro != NULL, "xr_range_to_array: coro must not be NULL");
    if (!r)
        return xr_null();

    int64_t len = xr_range_length(r);
    if (len <= 0) {
        XrArray *arr = xr_array_with_capacity(coro, 0);
        return xr_value_from_array(arr);
    }

    // Safety cap to prevent OOM (10M elements ~= 160MB for XrValue array)
    XR_CHECK(len <= 10000000, "range_to_array: range too large");

    XrArray *arr = xr_array_with_capacity(coro, (int32_t) len);
    if (!arr)
        return xr_null();

    XrValue *data = (XrValue *) arr->data;
    int64_t val = r->start;
    for (int64_t i = 0; i < len; i++) {
        data[i] = xr_int(val);
        val += r->step;
    }
    arr->length = (int32_t) len;

    return xr_value_from_array(arr);
}

/* ========== Native Type Registration ========== */

#include "xnative_type.h"
#include "xstring.h"
#include "../value/xvalue_format.h"
#include "../xisolate_api.h"

static XrValue m_range_to_string(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    return xr_string_value(xr_value_to_string(iso, self));
}

static XrValue m_range_to_array(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrRange *rng = xr_value_to_range(self);
    XR_DCHECK(rng != NULL, "Range.toArray: invalid range");
    return xr_range_to_array(xr_current_coro(iso), rng);
}

static XrValue m_range_contains(XrayIsolate *iso, XrValue self, XrValue *args, int argc) {
    (void) iso;
    XrRange *rng = xr_value_to_range(self);
    XR_DCHECK(rng != NULL, "Range.contains: invalid range");
    if (argc >= 1 && XR_IS_INT(args[0])) {
        return xr_bool(xr_range_contains(rng, XR_TO_INT(args[0])));
    }
    return xr_bool(false);
}

void xr_range_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod range_methods[] = {
        {"toString", m_range_to_string, 0},
        {"toArray", m_range_to_array, 0},
        {"contains", m_range_contains, 1},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo range_info = {
        .name = "Range",
        .gc_type = XR_TRANGE,
        .methods = range_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(isolate, &range_info);
}
