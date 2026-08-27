/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Independent SemanticPlan proof for the sealed i64 overflow family.
 */

#ifndef XI_I64_OVERFLOW_SEMANTIC_PLAN_H
#define XI_I64_OVERFLOW_SEMANTIC_PLAN_H

#include "xi.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include <stdbool.h>
#include <stddef.h>

struct XrTargetProfile;

XR_FUNC bool xi_i64_overflow_semantic_plan_verify(
    const XiFunc *root, const XrSemanticPlan *plan,
    const struct XrTargetProfile *target_profile, char *error, size_t error_size);

#endif  // XI_I64_OVERFLOW_SEMANTIC_PLAN_H
