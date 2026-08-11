/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_representation_refinement.h - Read-only Xi representation adapter bridge
 */

#ifndef XR_AOT_REPRESENTATION_REFINEMENT_H
#define XR_AOT_REPRESENTATION_REFINEMENT_H

#include "xr_aot_refinement.h"

struct XiFunc;
struct XiRepPolicy;

/* Materialize BOX/UNBOX obligations from the existing representation-rule
 * owner without editing the frozen Xi graph. The returned generic refinement
 * is immutable and consumable through the null/test/backend interface. */
XR_FUNC bool xr_aot_representation_refinement_build(
    const struct XiFunc *root, const XrTargetPlan *target_plan,
    const struct XiRepPolicy *policy, XrAotRefinementPlan **out_plan,
    XrAotRefinementDiagnostic *diag);

/* Re-derive every source value, use-site, type, representation and layout
 * obligation from the read-only graph and exact TargetPlan baseline. */
XR_FUNC bool xr_aot_representation_refinement_verify(
    const XrAotRefinementPlanView *view, const struct XiFunc *root,
    const XrTargetPlan *target_plan, const struct XiRepPolicy *policy,
    XrAotRefinementDiagnostic *diag);

#endif  // XR_AOT_REPRESENTATION_REFINEMENT_H
