/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbuiltin_enum_error.c - Canonical builtin enum error construction.
 */

#include "xbuiltin_enum_error.h"
#include "xenum.h"
#include "../core/xr_runtime_core.h"

static XrBuiltinEnumErrorResult builtin_enum_error_result(XrBuiltinEnumErrorStatus status,
                                                          XrValue value) {
    XrBuiltinEnumErrorResult result = {
        .status = status,
        .value = value,
    };
    return result;
}

XrBuiltinEnumErrorResult xr_builtin_enum_error_construct(XrRuntimeCore *core,
                                                         int32_t builtin_index,
                                                         uint32_t member_index) {
    if (!core || builtin_index < 0 || builtin_index >= XR_USER_GLOBALS_START) {
        return builtin_enum_error_result(XR_BUILTIN_ENUM_ERROR_INVALID_BUILTIN, XR_NULL_VAL);
    }

    XrValue builtin = xr_runtime_core_builtin(core, builtin_index);
    if (!XR_IS_ENUM_TYPE(builtin)) {
        return builtin_enum_error_result(XR_BUILTIN_ENUM_ERROR_BUILTIN_NOT_ENUM, XR_NULL_VAL);
    }

    XrEnumType *type = XR_TO_ENUM_TYPE(builtin);
    if (member_index >= type->member_count) {
        return builtin_enum_error_result(XR_BUILTIN_ENUM_ERROR_INVALID_VARIANT, XR_NULL_VAL);
    }
    if (xr_enum_type_payload_count(type, member_index) != 0) {
        return builtin_enum_error_result(XR_BUILTIN_ENUM_ERROR_PAYLOAD_REQUIRED, XR_NULL_VAL);
    }

    XrEnumAggregateValue *value = xr_enum_zero_payload_value(core, type, member_index);
    if (!value) {
        return builtin_enum_error_result(XR_BUILTIN_ENUM_ERROR_CONSTRUCTION_FAILED, XR_NULL_VAL);
    }
    return builtin_enum_error_result(XR_BUILTIN_ENUM_ERROR_OK, XR_FROM_PTR(value));
}
