/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_enum.h - Owned active payloads behind checked enum authority
 */
#ifndef XXIR_ENUM_H
#define XXIR_ENUM_H
#include "xxir_value.h"
/* Trusted callers must hold checked declaration and variant authority. */
XR_FUNC XrXirValueStatus xr_xir_enum_new(XrXirType type, uint32_t variant,
    const XrXirValue *fields, uint32_t count, XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_enum_get(const XrXirValue *value, uint32_t variant,
    uint32_t field, XrXirValueAdmission *admission, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_enum_variant(const XrXirValue *value, uint32_t *output);
#endif // XXIR_ENUM_H
