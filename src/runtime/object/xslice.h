/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xslice.h - Slice types for arrays
 *
 * KEY CONCEPT:
 *   Slice is a view into underlying array (zero-copy).
 *   - ArraySlice: view into XrArray, shares data pointer
 *   - Slices are non-growable (capacity=0)
 *   - Source object tracked for GC
 *   - Typed array slices (Array<uint8> etc.) inherit elem_type
 */

#ifndef XSLICE_H
#define XSLICE_H

#include <string.h>
#include "../value/xvalue.h"
#include "xarray.h"

/* ========== Slice Utility Functions ========== */
static inline int32_t xr_normalize_index(int32_t index, int32_t length) {
    if (index < 0) {
        index += length;
    }
    if (index < 0)
        return 0;
    if (index > length)
        return length;
    return index;
}

// Normalize slice range [start, end)
static inline void xr_normalize_range(int32_t *start, int32_t *end, int32_t length) {
    *start = xr_normalize_index(*start, length);
    *end = xr_normalize_index(*end, length);
    if (*end < *start) {
        *end = *start;
    }
}

/* ========== ArraySlice Helpers ========== */
// ArraySlice is defined in xarray.h (XrArraySlice = XrArray)

static inline int32_t xr_array_slice_length(XrArraySlice *slice) {
    return slice ? slice->length : 0;
}

static inline XrValue xr_array_slice_get(XrArraySlice *slice, int32_t index) {
    if (!slice || index < 0 || index >= slice->length) {
        return XR_NULL_VAL;
    }
    return ((XrValue *) slice->data)[index];
}

static inline void xr_array_slice_set(XrArraySlice *slice, int32_t index, XrValue value) {
    if (!slice || index < 0 || index >= slice->length) {
        return;
    }
    ((XrValue *) slice->data)[index] = value;
}

// XrValue Operations
XR_FUNC XrValue xr_value_from_array_slice(XrArraySlice *slice);
XR_FUNC bool xr_value_is_array_slice(XrValue v);
XR_FUNC XrArraySlice *xr_value_to_array_slice(XrValue v);

#endif  // XSLICE_H
