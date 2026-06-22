/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_oop_ops.c - Class and instance XrValue helpers.
 */

#include "xvalue.h"

#define DEFINE_VALUE_OPS_WITH_TYPE(name, type_enum, cast_type)                                     \
    XrValue xr_value_from_##name(cast_type *obj) {                                                 \
        return XR_FROM_PTR(obj);                                                                   \
    }                                                                                              \
    bool xr_value_is_##name(XrValue v) {                                                           \
        return XR_IS_PTR(v) && (v).heap_type == (type_enum);                                       \
    }                                                                                              \
    cast_type *xr_value_to_##name(XrValue v) {                                                     \
        return xr_value_is_##name(v) ? (cast_type *) XR_TO_PTR(v) : NULL;                          \
    }

DEFINE_VALUE_OPS_WITH_TYPE(class, XR_TCLASS, struct XrClass)
DEFINE_VALUE_OPS_WITH_TYPE(instance, XR_TINSTANCE, struct XrInstance)
