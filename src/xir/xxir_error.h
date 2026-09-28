/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_error.h - Enum-only owned existential conversion
 */
#ifndef XXIR_ERROR_H
#define XXIR_ERROR_H
#include "xxir_value.h"
/* Outputs own a retained value; failure preserves the source and unit output. */
XR_FUNC XrXirValueStatus xr_xir_error_erase(const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirValue *output);
/* Trusted callers must prove permission to name the concrete target type. */
XR_FUNC XrXirValueStatus xr_xir_error_narrow(const XrXirValue *value, XrXirType type,
    XrXirValueAdmission *admission, XrXirValue *output);
/* The output borrows the source owner and must never be dropped independently. */
XR_FUNC bool xr_xir_error_borrow(const XrXirValue *value, XrXirValue *output);
#endif // XXIR_ERROR_H
