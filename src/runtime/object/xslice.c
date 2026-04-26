/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xslice.c - Slice type implementation
 *
 * KEY CONCEPT:
 *   Only contains XrValue conversion functions.
 *   Core slice functions are defined in xarray.h.
 */

#include "xslice.h"
#include "../../base/xchecks.h"
#include "../gc/xgc_header.h"

/* ========== ArraySlice - XrValue Operations ========== */

XrValue xr_value_from_array_slice(XrArraySlice *slice) {
    return XR_FROM_PTR(slice);
}

bool xr_value_is_array_slice(XrValue v) {
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TARRAY_SLICE;
}

XrArraySlice *xr_value_to_array_slice(XrValue v) {
    if (!xr_value_is_array_slice(v))
        return NULL;
    return (XrArraySlice *) XR_TO_PTR(v);
}
