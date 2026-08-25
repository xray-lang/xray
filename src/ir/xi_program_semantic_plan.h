/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_program_semantic_plan.h - Independent Xi/SemanticPlan authority proof
 */

#ifndef XI_PROGRAM_SEMANTIC_PLAN_H
#define XI_PROGRAM_SEMANTIC_PLAN_H

#include "xi.h"
#include "../plan/semantic/xr_semantic_plan.h"
#include <stdbool.h>
#include <stddef.h>

struct XrTargetProfile;

/* Family-aware independent Xi/PSC to pointer-free SemanticPlan proof. */
XR_FUNC bool xi_program_semantic_plan_verify(const XiFunc *root, const XrSemanticPlan *plan,
                                             const struct XrTargetProfile *target_profile,
                                             char *error, size_t error_size);

#endif  // XI_PROGRAM_SEMANTIC_PLAN_H
