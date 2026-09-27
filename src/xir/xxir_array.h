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
#include "xxir_value.h"
#include "xxir_fault.h"

typedef struct XrXirArrayPlace {
    XrXirType type;
    void *payload;
} XrXirArrayPlace;

XR_FUNC XrXirValueStatus xr_xir_array_new(XrXirType array_type,
    const XrXirValue *values, size_t count, XrXirValueAdmission *admission,
    XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_array_len(const XrXirValue *array,
    XrXirValueAdmission *admission, int64_t *output);
XR_FUNC XrXirValueStatus xr_xir_array_get(const XrXirValue *array, int64_t index,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_array_set(const XrXirArrayPlace *place, int64_t index,
    const XrXirValue *element, XrXirValueAdmission *admission, XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_array_push(const XrXirArrayPlace *place,
    const XrXirValue *element, XrXirValueAdmission *admission);
XR_FUNC XrXirValueStatus xr_xir_cell_array_place(const XrXirValue *cell,
    XrXirValueAdmission *admission, XrXirArrayPlace *output);
#endif // XXIR_ARRAY_H
