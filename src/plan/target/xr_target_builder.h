/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_builder.h - Composable TargetPlan construction
 *
 * KEY CONCEPT:
 *   The public API owns one internal collection sequence and one final freeze.
 *   Family collectors contribute intents; one canonical materialization
 *   assigns dense identities and packed frames after coverage is complete.
 */

#ifndef XR_TARGET_BUILDER_H
#define XR_TARGET_BUILDER_H

#include "xr_target_plan.h"

XR_FUNC bool xr_target_plan_build(const XrSemanticPlan *semantic_plan,
                                  XrTargetProfile *profile,
                                  XrTargetPlan **out,
                                  char *error,
                                  size_t error_size);
XR_FUNC bool xr_target_plan_build_module_set(
    const XrSemanticPlan *semantic_plan,
    const XrSemanticPlan *const *dependencies,
    uint32_t dependency_count,
    XrTargetProfile *profile,
    XrTargetPlan **out,
    char *error,
    size_t error_size);
XR_FUNC bool xr_target_plan_build_program_graph(
    const XrSemanticPlan *const *semantic_modules,
    uint32_t semantic_module_count,
    XrTargetProfile *profile,
    XrTargetPlan **out,
    char *error,
    size_t error_size);
/* Build one global TargetPlan over an acyclic module set, partitioned per
 * module. It claims module coverage only: every target row is attributed to the
 * module whose SemanticPlan produced it, and no cross-module call is proven. */
XR_FUNC bool xr_target_plan_build_program_module_set(const XrSemanticPlan *const *modules,
                                                     uint32_t module_count,
                                                     const XrSemanticPlan *entry,
                                                     XrTargetProfile *profile, XrTargetPlan **out,
                                                     char *error, size_t error_size);

#endif  // XR_TARGET_BUILDER_H
