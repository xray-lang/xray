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

bool xr_value_is_truthy(XrValue value) {
    if (XR_IS_NULL(value))
        return false;
    if (XR_IS_BOOL(value))
        return XR_TO_BOOL(value) != 0;
    if (XR_IS_INT(value))
        return XR_TO_INT(value) != 0;
    if (XR_IS_FLOAT(value))
        return XR_TO_FLOAT(value) != 0.0;
    if (XR_IS_STRING(value))
        return XR_TO_STRING(value)->length != 0;
    if (XR_IS_ARRAY(value))
        return XR_TO_ARRAY(value)->length != 0;
    if (XR_IS_MAP(value))
        return XR_TO_MAP(value)->count != 0;
    if (XR_IS_SET(value))
        return XR_TO_SET(value)->count != 0;
    return true;
}
