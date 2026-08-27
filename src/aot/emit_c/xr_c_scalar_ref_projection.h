/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_scalar_ref_projection.h - Exact scalar-ref-v1 C projection
 */

#ifndef XR_C_SCALAR_REF_PROJECTION_H
#define XR_C_SCALAR_REF_PROJECTION_H

#include "xr_c_emission_schema.h"
#include "../../plan/target/xr_target_plan.h"

typedef enum XrCScalarRefProjectionStatus {
    XR_C_SCALAR_REF_NOT_THIS_FAMILY = 0,
    XR_C_SCALAR_REF_EXACT,
    XR_C_SCALAR_REF_MALFORMED,
} XrCScalarRefProjectionStatus;

typedef struct XrCScalarRefProjection {
    uint32_t source_value;
    XrCCallArgumentEmissionView call_argument;
    XrCFunctionAbiEmissionView function_abi;
} XrCScalarRefProjection;

XR_FUNC XrCScalarRefProjectionStatus xr_c_scalar_ref_project_argument(
    const XrTargetPlan *target_plan, const XrTargetCallArgumentRecord *argument,
    XrCScalarRefProjection *out);

XR_FUNC XrCScalarRefProjectionStatus xr_c_scalar_ref_project_address(
    const XrTargetPlan *target_plan, const XrTargetValueRepRecord *binding,
    XrCScalarRefProjection *out);

XR_FUNC bool xr_c_scalar_ref_projection_views_are_exact(
    const XrCScalarRefProjection *projection,
    const XrCCallArgumentEmissionView *call_arguments, uint32_t call_argument_count,
    const XrCFunctionAbiEmissionView *function_abis, uint32_t function_abi_count);

#endif  // XR_C_SCALAR_REF_PROJECTION_H
