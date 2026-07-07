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
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TENUM_TYPE;
}

bool xr_value_is_enum_ctor(XrValue v) {
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TENUM_CTOR;
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
