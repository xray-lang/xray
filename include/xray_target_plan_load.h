/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_target_plan_load.h - Verified TargetPlan artifact load boundary
 *
 * KEY CONCEPT:
 *   Loading validates and materializes an XTP v2 artifact against explicit
 *   semantic and target authorities. It returns a verified TargetPlan but
 *   performs no provider binding, module activation, or entry execution.
 */

#ifndef XRAY_TARGET_PLAN_LOAD_H
#define XRAY_TARGET_PLAN_LOAD_H

#include "xray_export.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrSemanticPlan XrSemanticPlan;
typedef struct XrTargetProfile XrTargetProfile;
typedef struct XrTargetPlan XrTargetPlan;

XRAY_API bool xr_runtime_target_plan_load(
    const uint8_t *artifact_bytes, size_t artifact_size,
    const XrSemanticPlan *verified_semantic_plan,
    const XrTargetProfile *exact_target_profile,
    XrTargetPlan **verified_target_plan, char *diagnostic,
    size_t diagnostic_size);

/* Existing TargetPlan lifetime owner; this is a declaration, not an alias. */
XRAY_API void xr_target_plan_free(XrTargetPlan *plan);

#endif  // XRAY_TARGET_PLAN_LOAD_H
