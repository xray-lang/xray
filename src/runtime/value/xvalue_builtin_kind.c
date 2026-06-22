/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_builtin_kind.c - Builtin-kind based instance predicates.
 */

#include "xvalue.h"
#include "../class/xinstance.h"

bool xr_value_is_enum_type(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_ENUM_TYPE;
}

bool xr_value_is_enum_value(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_ENUM_VALUE;
}

bool xr_value_is_iterator(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_ITERATOR;
}

bool xr_value_is_bigint(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_BIGINT;
}
