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
#include "../object/xarray.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../object/xstring.h"
#include "../../shared/xr_truthy_core.h"

bool xr_value_is_truthy(XrValue value) {
    if (XR_IS_NULL(value))
        return xr_truthy_core_eval(XR_TRUTHY_CORE_NULL, 0, 0.0, 0);
    if (XR_IS_BOOL(value))
        return xr_truthy_core_eval(XR_TRUTHY_CORE_BOOL, XR_TO_BOOL(value), 0.0, 0);
    if (XR_IS_INT(value))
        return xr_truthy_core_eval(XR_TRUTHY_CORE_INT, XR_TO_INT(value), 0.0, 0);
    if (XR_IS_FLOAT(value))
        return xr_truthy_core_eval(XR_TRUTHY_CORE_FLOAT, 0, XR_TO_FLOAT(value), 0);
    if (XR_IS_STRING(value))
        return xr_truthy_core_eval(XR_TRUTHY_CORE_SIZED, 0, 0.0,
                                   (int64_t) XR_TO_STRING(value)->length);
    if (XR_IS_ARRAY(value))
        return xr_truthy_core_eval(XR_TRUTHY_CORE_SIZED, 0, 0.0,
                                   (int64_t) XR_TO_ARRAY(value)->length);
    if (XR_IS_MAP(value))
        return xr_truthy_core_eval(XR_TRUTHY_CORE_SIZED, 0, 0.0, (int64_t) XR_TO_MAP(value)->count);
    if (XR_IS_SET(value))
        return xr_truthy_core_eval(XR_TRUTHY_CORE_SIZED, 0, 0.0, (int64_t) XR_TO_SET(value)->count);
    return xr_truthy_core_eval(XR_TRUTHY_CORE_OBJECT, 0, 0.0, 0);
}
