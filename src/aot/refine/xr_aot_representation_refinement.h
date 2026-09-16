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

/* Canonical policy identity retained even by an empty refinement plan. */
XR_FUNC XrFingerprint xr_aot_representation_policy_fingerprint(
    const struct XiRepPolicy *policy);

/* Derive the complete adapter obligation set from immutable authority only.
 * Unsupported source types or use shapes fail closed instead of producing a
 * partial plan. */
XR_FUNC bool xr_aot_representation_refinement_build_from_authority(
    const XrTargetPlan *target_plan, const XrSemanticPlan *semantic_plan,
    const struct XiRepPolicy *policy,
    XrAotRefinementPlan **out_plan, XrAotRefinementDiagnostic *diag);

/* One synchronous immutable admission covers the batch. Failure frees and
 * clears every output, including modules already built successfully. */
XR_FUNC bool xr_aot_representation_refinement_build_modules(
    const XrTargetPlan *target_plan, const XrSemanticPlan *const *semantics,
    uint32_t module_count, const struct XiRepPolicy *policy,
    XrAotRefinementPlan **out_plans, uint32_t *failed_module,
    XrAotRefinementDiagnostic *diag);

/* Internal synchronous collector. It cannot expose the builder or invoke
 * external callbacks; the owning build function retains target admission. */
XR_FUNC bool xr_aot_representation_collect_authority(
    XrAotRefinementBuilder *builder, const XrTargetPlan *target_plan,
    const XrSemanticPlan *semantic_plan, const struct XiRepPolicy *policy,
    XrAotRefinementDiagnostic *diag);

/* Verify that a backend-stage Xi graph is the exact materialization of an
 * immutable representation refinement. */
XR_FUNC bool xr_aot_representation_materialization_verify(
    const XrAotRefinementPlanView *view, const struct XiFunc *root,
    const XrTargetPlan *target_plan, const struct XiRepPolicy *policy,
    XrAotRefinementDiagnostic *diag);

XR_FUNC bool xr_aot_representation_materialization_verify_modules(
    const XrAotRefinementPlanView *views, const struct XiFunc *const *roots,
    uint32_t module_count, const XrTargetPlan *target_plan,
    const struct XiRepPolicy *policy, uint32_t *failed_module,
    XrAotRefinementDiagnostic *diag);

#endif  // XR_AOT_REPRESENTATION_REFINEMENT_H
