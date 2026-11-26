/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_fixed_array.h - Exact nonempty fixed storage of one admitted element
 *
 * KEY CONCEPT:
 *   The type owns the element and its length; a value owns exactly
 *   length * element-width bytes, without capacity.
 */
#ifndef XXIR_FIXED_ARRAY_H
#define XXIR_FIXED_ARRAY_H
#include "xxir_value.h"
#include "xxir_fault.h"
XR_FUNC XrXirValueStatus xr_xir_fixed_array_new(XrXirType type,const XrXirValue *values,
    uint32_t count,XrXirValueAdmission *admission,XrXirValue *output);
/* bytes holds count elements at the type's element width, in host order. */
XR_FUNC XrXirValueStatus xr_xir_fixed_array_from_bytes(XrXirType type,const uint8_t *bytes,
    uint32_t count,XrXirValueAdmission *admission,XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_fixed_array_get(const XrXirValue *value,int64_t index,
    XrXirValue *output,XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_fixed_array_count(const XrXirValue *value,int64_t *output);
#endif
