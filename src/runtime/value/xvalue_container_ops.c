/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_container_ops.c - Container and module XrValue helpers.
 */

#include "xvalue.h"

#define DEFINE_VALUE_OPS_WITH_MACRO(name, check_macro, cast_type)                                  \
    XrValue xr_value_from_##name(cast_type *obj) {                                                 \
        return XR_FROM_PTR(obj);                                                                   \
    }                                                                                              \
    bool xr_value_is_##name(XrValue v) {                                                           \
        return check_macro(v);                                                                     \
    }                                                                                              \
    cast_type *xr_value_to_##name(XrValue v) {                                                     \
        return check_macro(v) ? (cast_type *) XR_TO_PTR(v) : NULL;                                 \
    }

DEFINE_VALUE_OPS_WITH_MACRO(array, XR_IS_ARRAY, XrArray)
DEFINE_VALUE_OPS_WITH_MACRO(map, XR_IS_MAP, struct XrMap)
DEFINE_VALUE_OPS_WITH_MACRO(set, XR_IS_SET, struct XrSet)
DEFINE_VALUE_OPS_WITH_MACRO(module, XR_IS_MODULE, struct XrModule)
