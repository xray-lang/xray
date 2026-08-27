/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_scalar_ref_v1.h - Direct-local scalar ref refinement authority
 */

#ifndef XR_AOT_SCALAR_REF_V1_H
#define XR_AOT_SCALAR_REF_V1_H

#include "../../plan/target/xr_target_plan.h"

typedef enum XrAotScalarRefV1Status {
    XR_AOT_SCALAR_REF_V1_UNRELATED = 0,
    XR_AOT_SCALAR_REF_V1_INVALID,
    XR_AOT_SCALAR_REF_V1_EXACT,
} XrAotScalarRefV1Status;

XR_FUNC XrAotScalarRefV1Status xr_aot_scalar_ref_v1_parameter_status(
    const XrSemanticPlan *semantic, const XrTargetPlan *target,
    uint32_t semantic_value);
XR_FUNC XrAotScalarRefV1Status xr_aot_scalar_ref_v1_local_addr_status(
    const XrSemanticPlan *semantic, const XrTargetPlan *target,
    uint32_t operation_index, uint32_t *source_value);
XR_FUNC XrAotScalarRefV1Status xr_aot_scalar_ref_v1_place_use_status(
    const XrSemanticPlan *semantic, const XrTargetPlan *target,
    uint32_t operation_index, uint16_t operand_index, uint32_t source_value);
XR_FUNC XrAotScalarRefV1Status xr_aot_scalar_ref_v1_call_use_status(
    const XrSemanticPlan *semantic, const XrTargetPlan *target,
    uint32_t operation_index, uint16_t operand_index, uint32_t source_value);

#endif /* XR_AOT_SCALAR_REF_V1_H */
