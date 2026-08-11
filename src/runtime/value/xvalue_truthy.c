/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_truthy.c - Runtime truthiness checks.
 */

#include "xvalue.h"
#include "../../shared/xr_truthy_core.h"

bool xr_value_is_truthy(XrValue value) {
    XrTruthyCoreKind kind = XR_TRUTHY_CORE_OBJECT;
    int64_t integer = 0;
    double floating = 0.0;
    if (XR_IS_NULL(value)) {
        kind = XR_TRUTHY_CORE_NULL;
    } else if (XR_IS_BOOL(value)) {
        kind = XR_TRUTHY_CORE_BOOL;
        integer = XR_TO_BOOL(value);
    } else if (XR_IS_INT(value)) {
        kind = XR_TRUTHY_CORE_INT;
        integer = XR_TO_INT(value);
    } else if (XR_IS_FLOAT(value)) {
        kind = XR_TRUTHY_CORE_FLOAT;
        floating = XR_TO_FLOAT(value);
    }
    return xr_truthy_core_eval(XR_SEM_OWNER_ID_SHARED_TRUTHINESS_HI,
                               XR_SEM_OWNER_ID_SHARED_TRUTHINESS_LO, XR_SEM_CONSUMER_RUNTIME,
                               kind, integer, floating, 0);
}
