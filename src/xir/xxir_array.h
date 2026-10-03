/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_array.h - Compact owned arrays and synchronous root mutation
 *
 * KEY CONCEPT:
 *   A resolved root borrows real owner storage only until mutation returns.
 */
#ifndef XXIR_ARRAY_H
#define XXIR_ARRAY_H
#include "xxir_value_place.h"
#include "xxir_fault.h"

XR_FUNC XrXirValueStatus xr_xir_array_new(XrXirType array_type,
    const XrXirValue *values, size_t count, XrXirValueAdmission *admission,
    XrXirValue *output);
/* A new array of `length` copies of `fill`; the caller rejects a negative length. */
XR_FUNC XrXirValueStatus xr_xir_array_repeat(XrXirType array_type, int64_t length,
    const XrXirValue *fill, XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_array_len(const XrXirValue *array,
    XrXirValueAdmission *admission, int64_t *output);
XR_FUNC XrXirValueStatus xr_xir_array_get(const XrXirValue *array, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_array_set(const XrXirValuePlace *place, int64_t index,
    const XrXirValue *element, XrXirValueAdmission *admission, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_array_push(const XrXirValuePlace *place,
    const XrXirValue *element, XrXirValueAdmission *admission);
#endif // XXIR_ARRAY_H
