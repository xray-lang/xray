/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_scalar_value.h - Xi to scalar TargetPlan identity bridge
 *
 * KEY CONCEPT:
 *   Compiler pointers terminate at this refinement boundary. C emission sees
 *   only the numeric identities selected by the attached SemanticPlan.
 */

#ifndef XR_AOT_SCALAR_VALUE_H
#define XR_AOT_SCALAR_VALUE_H

#include "../../ir/xi.h"
#include "../../plan/target/xr_target_plan.h"

XR_FUNC bool xr_aot_scalar_semantic_value_id(const XrTargetPlan *target_plan,
                                             const XiFunc *function,
                                             const XiValue *value,
                                             uint32_t *out_semantic_function,
                                             uint32_t *out_semantic_value,
                                             char *error,
                                             size_t error_size);

XR_FUNC bool xr_aot_rep_adapter_value_is_exact(
    const XrTargetPlan *target_plan, const XiFunc *function,
    const XiValue *value, char *error, size_t error_size);

#endif  // XR_AOT_SCALAR_VALUE_H
