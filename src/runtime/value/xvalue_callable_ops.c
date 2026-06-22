/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_callable_ops.c - Callable XrValue helpers.
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

DEFINE_VALUE_OPS_WITH_MACRO(closure, XR_IS_FUNCTION, struct XrClosure)
DEFINE_VALUE_OPS_WITH_TYPE(cfunction, XR_TCFUNCTION, struct XrCFunction)
