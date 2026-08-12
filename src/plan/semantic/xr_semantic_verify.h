/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_verify.h - Independent SemanticPlan verifier
 */

#ifndef XR_SEMANTIC_VERIFY_H
#define XR_SEMANTIC_VERIFY_H

#include "xr_semantic_plan.h"
#include <stdbool.h>
#include <stddef.h>

XR_FUNC bool xr_semantic_plan_verify(const XrSemanticPlan *plan, char *error, size_t error_size);
XR_FUNC bool xr_semantic_plan_verify_module_set(
    const XrSemanticPlan *plan, const XrSemanticPlan *const *dependencies,
    uint32_t dependency_count, char *error, size_t error_size);

#endif  // XR_SEMANTIC_VERIFY_H
