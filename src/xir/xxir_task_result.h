/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_result.h - Governed Task outcome types over existing owned carriers
 */
#ifndef XXIR_TASK_RESULT_H
#define XXIR_TASK_RESULT_H
#include "xxir_value.h"
typedef struct XrXirTypes XrXirTypes;

typedef enum XrXirTaskResultVariant {
    XR_XIR_TASK_RESULT_SUCCESS,
    XR_XIR_TASK_RESULT_ERROR,
    XR_XIR_TASK_RESULT_PANIC,
    XR_XIR_TASK_RESULT_CANCELLED
} XrXirTaskResultVariant;

typedef struct XrXirTaskResultType {
    XrXirType type,element,error;
} XrXirTaskResultType;

/* Arguments and declaration metadata belong to this exact admitted type pool.
 * Error stays canonical metadata; its active physical field is XR_XIR_ERROR.
 * Failure leaves the caller's output unchanged. */
XR_FUNC bool xr_xir_task_result_type_info(const XrXirTypes *types,XrXirType type,
    XrXirTaskResultType *output);
/* Match the invariant value/error arguments without interpreting foreign IDs. */
XR_FUNC bool xr_xir_task_result_matches_task(const XrXirTypes *types,
    XrXirType result,XrXirType task);
#endif // XXIR_TASK_RESULT_H
