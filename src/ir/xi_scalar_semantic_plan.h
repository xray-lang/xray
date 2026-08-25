/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_scalar_semantic_plan.h - Independent scalar SemanticPlan authority proof
 */

#ifndef XI_SCALAR_SEMANTIC_PLAN_H
#define XI_SCALAR_SEMANTIC_PLAN_H

#include "xi.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include <stdbool.h>
#include <stddef.h>

struct XrTargetProfile;

/* Consumes XiModule's typed PSC, CallDecision, and TargetProfile to recheck
 * pointer-free SemanticPlan provenance and row joins without consulting source
 * declarations. */
XR_FUNC bool xi_scalar_semantic_plan_verify(
    const XiFunc *root, const XrSemanticPlan *plan,
    const struct XrTargetProfile *target_profile, char *error,
    size_t error_size);

#endif  // XI_SCALAR_SEMANTIC_PLAN_H
