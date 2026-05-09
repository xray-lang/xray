/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_ops.h - Shared typed element get/set on raw data pointers.
 *
 * These operate on a raw `void *data` buffer + `XrArrayElemType`, independent
 * of the container struct (XrArray, xrt_array_t, etc.). All consumers
 * (VM, JIT, AOT) include this to get consistent typed storage semantics.
 *
 * Dependency: caller must include their own XrValue header first, providing:
 *   XrValue, XR_FROM_INT/XR_TO_INT, XR_FROM_FLOAT/XR_TO_FLOAT,
 *   XR_IS_INT/XR_IS_FLOAT/XR_IS_NULL/XR_IS_FALSE,
 *   xr_value_to_int64_coerce, xr_value_to_f64_coerce
 *
 * Then include xr_elem_type.h (for XrArrayElemType).
 */

#ifndef XR_TYPED_OPS_H
#define XR_TYPED_OPS_H

#include "xr_elem_type.h"

/* Read one element from a typed buffer, returning a boxed XrValue.
 * Caller must ensure index is within bounds. */
static inline XrValue xr_typed_get(void *data, int32_t index, uint8_t elem_type) {
    switch (elem_type) {
        case XR_ELEM_ANY:
            return ((XrValue *) data)[index];
        case XR_ELEM_I8:
            return XR_FROM_INT((int64_t) ((int8_t *) data)[index]);
        case XR_ELEM_U8:
            return XR_FROM_INT((int64_t) ((uint8_t *) data)[index]);
        case XR_ELEM_I16:
            return XR_FROM_INT((int64_t) ((int16_t *) data)[index]);
        case XR_ELEM_U16:
            return XR_FROM_INT((int64_t) ((uint16_t *) data)[index]);
        case XR_ELEM_I32:
            return XR_FROM_INT((int64_t) ((int32_t *) data)[index]);
        case XR_ELEM_U32:
            return XR_FROM_INT((int64_t) ((uint32_t *) data)[index]);
        case XR_ELEM_I64:
            return XR_FROM_INT(((int64_t *) data)[index]);
        case XR_ELEM_U64:
            return XR_FROM_INT((int64_t) ((uint64_t *) data)[index]);
        case XR_ELEM_F32:
            return XR_FROM_FLOAT((double) ((float *) data)[index]);
        case XR_ELEM_F64:
            return XR_FROM_FLOAT(((double *) data)[index]);
        case XR_ELEM_BOOL:
            return ((uint8_t *) data)[index] ? XR_TRUE_VAL : XR_FALSE_VAL;
        default:
            return XR_NULL_VAL;
    }
}

/* Write one element into a typed buffer from a boxed XrValue.
 * Caller must ensure index is within bounds.
 * Returns true if the value might contain a GC pointer (XR_ELEM_ANY only). */
static inline bool xr_typed_set(void *data, int32_t index, XrValue value, uint8_t elem_type) {
    switch (elem_type) {
        case XR_ELEM_ANY:
            ((XrValue *) data)[index] = value;
            return true; /* caller must handle GC barrier */
        case XR_ELEM_I8:
            ((int8_t *) data)[index] = (int8_t) xr_value_to_int64_coerce(value);
            return false;
        case XR_ELEM_U8:
            ((uint8_t *) data)[index] = (uint8_t) xr_value_to_int64_coerce(value);
            return false;
        case XR_ELEM_I16:
            ((int16_t *) data)[index] = (int16_t) xr_value_to_int64_coerce(value);
            return false;
        case XR_ELEM_U16:
            ((uint16_t *) data)[index] = (uint16_t) xr_value_to_int64_coerce(value);
            return false;
        case XR_ELEM_I32:
            ((int32_t *) data)[index] = (int32_t) xr_value_to_int64_coerce(value);
            return false;
        case XR_ELEM_U32:
            ((uint32_t *) data)[index] = (uint32_t) xr_value_to_int64_coerce(value);
            return false;
        case XR_ELEM_I64:
            ((int64_t *) data)[index] = xr_value_to_int64_coerce(value);
            return false;
        case XR_ELEM_U64:
            ((uint64_t *) data)[index] = (uint64_t) xr_value_to_int64_coerce(value);
            return false;
        case XR_ELEM_F32:
            ((float *) data)[index] = (float) xr_value_to_f64_coerce(value);
            return false;
        case XR_ELEM_F64:
            ((double *) data)[index] = xr_value_to_f64_coerce(value);
            return false;
        case XR_ELEM_BOOL: {
            bool falsy = XR_IS_FALSE(value) || XR_IS_NULL(value) ||
                         (XR_IS_INT(value) && XR_TO_INT(value) == 0) ||
                         (XR_IS_FLOAT(value) && XR_TO_FLOAT(value) == 0.0);
            ((uint8_t *) data)[index] = falsy ? 0 : 1;
            return false;
        }
        default:
            return false;
    }
}

#endif  // XR_TYPED_OPS_H
