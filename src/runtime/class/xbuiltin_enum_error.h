/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbuiltin_enum_error.h - Canonical builtin enum error construction.
 *
 * KEY CONCEPT:
 *   Construction depends only on runtime-core builtin authority. It returns a
 *   typed result and never observes or publishes executor error state.
 */

#ifndef XBUILTIN_ENUM_ERROR_H
#define XBUILTIN_ENUM_ERROR_H

#include "../value/xvalue.h"
#include <stdint.h>

struct XrRuntimeCore;

typedef enum XrBuiltinEnumErrorStatus {
    XR_BUILTIN_ENUM_ERROR_OK = 0,
    XR_BUILTIN_ENUM_ERROR_INVALID_BUILTIN,
    XR_BUILTIN_ENUM_ERROR_BUILTIN_NOT_ENUM,
    XR_BUILTIN_ENUM_ERROR_INVALID_VARIANT,
    XR_BUILTIN_ENUM_ERROR_PAYLOAD_REQUIRED,
    XR_BUILTIN_ENUM_ERROR_CONSTRUCTION_FAILED,
} XrBuiltinEnumErrorStatus;

typedef struct XrBuiltinEnumErrorResult {
    XrBuiltinEnumErrorStatus status;
    XrValue value;
} XrBuiltinEnumErrorResult;

XR_FUNC XrBuiltinEnumErrorResult
xr_builtin_enum_error_construct(struct XrRuntimeCore *core, int32_t builtin_index,
                                uint32_t member_index);

#endif  // XBUILTIN_ENUM_ERROR_H
