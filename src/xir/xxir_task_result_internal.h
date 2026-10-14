/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_result_internal.h - Executable primitive Task result families
 *
 * KEY CONCEPT:
 *   Capability admission does not replace the worker's Sendable or root proof.
 */
#ifndef XXIR_TASK_RESULT_INTERNAL_H
#define XXIR_TASK_RESULT_INTERNAL_H
#include "xxir_value.h"
static inline bool xr_xir_task_scalar_result_supported(XrXirType type) {
    return type == XR_XIR_UNIT || type == XR_XIR_BOOL || type == XR_XIR_RUNE ||
        xr_xir_type_is_number(type) || type == XR_XIR_STRING;
}
#endif // XXIR_TASK_RESULT_INTERNAL_H
