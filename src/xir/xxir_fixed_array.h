/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_fixed_array.h - Exact nonempty scalar values with fixed storage
 *
 * KEY CONCEPT:
 *   The type owns element and length; each owner has exact storage, without capacity.
 */
#ifndef XXIR_FIXED_ARRAY_H
#define XXIR_FIXED_ARRAY_H
#include "xxir_value.h"
#include "xxir_fault.h"
XR_FUNC XrXirValueStatus xr_xir_fixed_array_new(XrXirType type,const XrXirValue *values,
    uint32_t count,XrXirValueAdmission *admission,XrXirValue *output);
/* This byte-span convenience constructor admits only an exact u8 element type. */
XR_FUNC XrXirValueStatus xr_xir_fixed_array_from_bytes(XrXirType type,const uint8_t *bytes,
    uint32_t count,XrXirValueAdmission *admission,XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_fixed_array_get(const XrXirValue *value,int64_t index,
    XrXirValue *output,XrXirFaultDetail *fault);
XR_FUNC XrXirValueStatus xr_xir_fixed_array_count(const XrXirValue *value,int64_t *output);
#endif
