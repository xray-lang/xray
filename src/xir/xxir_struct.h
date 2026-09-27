/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_struct.h - Owned nominal snapshots behind checked field authority
 */
#ifndef XXIR_STRUCT_H
#define XXIR_STRUCT_H
#include "xxir_value_place.h"
/* Trusted callers must already hold checked construction/member permission. */
XR_FUNC XrXirValueStatus xr_xir_struct_new(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_struct_get(const XrXirValue *value, uint32_t field,
    XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_struct_set(const XrXirValuePlace *place, uint32_t field,
    const XrXirValue *value, XrXirValueAdmission *admission);
#endif // XXIR_STRUCT_H
