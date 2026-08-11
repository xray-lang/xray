/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_refinement.h - Immutable TargetPlan-native AOT refinement protocol
 *
 * KEY CONCEPT:
 *   A refinement records proof obligations against an exact verified
 *   TargetPlan. Unsupported transformations preserve that baseline and carry
 *   a stable refusal reason; backends never receive analyzer or Xi state.
 */

#ifndef XR_AOT_REFINEMENT_H
#define XR_AOT_REFINEMENT_H

#include "../../plan/target/xr_target_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_AOT_REFINEMENT_SCHEMA_VERSION UINT32_C(1)
#define XR_AOT_REFINEMENT_BACKEND_ABI_VERSION UINT32_C(1)

typedef enum XrAotInvariant {
    XR_AOT_INV_CFG = 0,
    XR_AOT_INV_VALUES,
    XR_AOT_INV_TYPES,
    XR_AOT_INV_CALL_TARGET,
    XR_AOT_INV_CALL_ABI,
    XR_AOT_INV_EFFECT,
    XR_AOT_INV_ESCAPE,
    XR_AOT_INV_OWNERSHIP,
    XR_AOT_INV_LIFETIME,
    XR_AOT_INV_ERROR,
    XR_AOT_INV_ENVIRONMENT,
    XR_AOT_INV_GENERATION,
    XR_AOT_INV_DEBUG,
    XR_AOT_INV_COUNT,
} XrAotInvariant;

typedef uint32_t XrAotInvariantMask;

#define XR_AOT_INV_BIT(kind) ((XrAotInvariantMask) (UINT32_C(1) << (kind)))
#define XR_AOT_INV_ALL \
    ((XrAotInvariantMask) ((UINT32_C(1) << XR_AOT_INV_COUNT) - 1u))

typedef enum XrAotTransformKind {
    XR_AOT_TRANSFORM_DIRECT_CALL = 1,
    XR_AOT_TRANSFORM_COUNT,
} XrAotTransformKind;

#define XR_AOT_TRANSFORM_BIT(kind) (UINT32_C(1) << ((kind) - 1u))

typedef enum XrAotRefinementDecision {
    XR_AOT_REFINEMENT_APPLIED = 1,
    XR_AOT_REFINEMENT_REFUSED = 2,
} XrAotRefinementDecision;

typedef enum XrAotRefinementIssue {
    XR_AOT_REFINEMENT_OK = 0,
    XR_AOT_REFINEMENT_INVALID_ARGUMENT,
    XR_AOT_REFINEMENT_OUT_OF_MEMORY,
    XR_AOT_REFINEMENT_BASELINE_FINGERPRINT,
    XR_AOT_REFINEMENT_PASS_PROTOCOL,
    XR_AOT_REFINEMENT_STALE_EVIDENCE,
    XR_AOT_REFINEMENT_INVALIDATED_EVIDENCE,
    XR_AOT_REFINEMENT_DIRECT_CALL_SCHEMA_UNAVAILABLE,
    XR_AOT_REFINEMENT_PLAN_STATE,
    XR_AOT_REFINEMENT_BACKEND_ABI,
    XR_AOT_REFINEMENT_BACKEND_INCOMPLETE_COVERAGE,
    XR_AOT_REFINEMENT_BACKEND_FAILURE,
} XrAotRefinementIssue;

typedef struct XrAotRefinementDiagnostic {
    uint32_t issue;
    uint32_t record_index;
    uint32_t pass_id;
    uint32_t target_call_index;
} XrAotRefinementDiagnostic;

/* These are the only baseline identities retained by a refinement plan. */
typedef struct XrAotBaselineRef {
    XrFingerprint semantic_fingerprint;
    XrFingerprint target_plan_fingerprint;
    XrFingerprint target_profile_fingerprint;
    uint64_t completed_family_mask;
} XrAotBaselineRef;

typedef struct XrAotInvariantState {
    XrAotInvariantMask available;
    uint64_t generation[XR_AOT_INV_COUNT];
} XrAotInvariantState;

typedef struct XrAotPassProtocol {
    uint32_t schema_version;
    uint32_t pass_id;
    uint16_t transform_kind;
    XrAotInvariantMask requires;
    XrAotInvariantMask produces;
    XrAotInvariantMask invalidates;
    XrAotInvariantMask preserves;
} XrAotPassProtocol;

/* The request names a future TargetPlan call row; it does not manufacture one. */
typedef struct XrAotDirectCallRequest {
    uint32_t target_call_index;
} XrAotDirectCallRequest;

typedef struct XrAotTransformationRecord {
    XrAotPassProtocol protocol;
    XrAotInvariantState input_state;
    XrAotInvariantState output_state;
    XrAotDirectCallRequest direct_call;
    uint16_t decision;
    uint16_t transform_kind;
    uint32_t diagnostic_issue;
} XrAotTransformationRecord;

typedef struct XrAotRefinementPlan XrAotRefinementPlan;
typedef struct XrAotRefinementBuilder XrAotRefinementBuilder;

typedef struct XrAotRefinementPlanView {
    uint32_t schema_version;
    XrAotBaselineRef baseline;
    XrAotInvariantState initial_state;
    const XrAotTransformationRecord *records;
    uint32_t record_count;
    bool frozen;
    bool verified;
} XrAotRefinementPlanView;

typedef struct XrAotBackendStats {
    uint32_t visited;
    uint32_t applied;
    uint32_t refused;
} XrAotBackendStats;

typedef struct XrAotBackendInterface {
    uint32_t abi_version;
    uint32_t supported_transforms;
    bool (*begin)(void *context, const XrAotBaselineRef *baseline,
                  uint32_t record_count);
    bool (*visit)(void *context, uint32_t index,
                  const XrAotTransformationRecord *record);
    bool (*finish)(void *context);
} XrAotBackendInterface;

typedef struct XrAotNullBackend {
    XrAotBackendStats stats;
    uint32_t expected_records;
} XrAotNullBackend;

typedef struct XrAotTestBackend {
    char *buffer;
    size_t capacity;
    size_t length;
    uint32_t expected_records;
    uint32_t visited;
} XrAotTestBackend;

XR_FUNC const char *xr_aot_refinement_issue_name(uint32_t issue);
XR_FUNC bool xr_aot_refinement_baseline_from_target_plan(
    const XrTargetPlan *target_plan, XrAotBaselineRef *out_baseline,
    XrAotRefinementDiagnostic *diag);
XR_FUNC XrAotInvariantState xr_aot_refinement_initial_state(
    const XrAotBaselineRef *baseline);
XR_FUNC bool xr_aot_refinement_state_after_invalidation(
    const XrAotInvariantState *input, XrAotInvariantMask invalidates,
    XrAotInvariantState *output, XrAotRefinementDiagnostic *diag);
XR_FUNC XrAotPassProtocol xr_aot_refinement_direct_call_protocol(
    uint32_t pass_id);
XR_FUNC XrAotRefinementBuilder *xr_aot_refinement_builder_create(
    const XrTargetPlan *target_plan, XrAotRefinementDiagnostic *diag);
XR_FUNC void xr_aot_refinement_builder_free(XrAotRefinementBuilder *builder);
XR_FUNC bool xr_aot_refinement_try_direct_call(
    XrAotRefinementBuilder *builder, const XrAotPassProtocol *protocol,
    const XrTargetPlan *target_plan, const XrAotDirectCallRequest *request,
    uint32_t *out_decision, XrAotRefinementDiagnostic *diag);
XR_FUNC bool xr_aot_refinement_builder_freeze(
    XrAotRefinementBuilder *builder, const XrTargetPlan *target_plan,
    XrAotRefinementPlan **out_plan, XrAotRefinementDiagnostic *diag);
XR_FUNC void xr_aot_refinement_plan_free(XrAotRefinementPlan *plan);
XR_FUNC XrAotRefinementPlanView xr_aot_refinement_plan_view(
    const XrAotRefinementPlan *plan);
XR_FUNC bool xr_aot_refinement_verify(const XrAotRefinementPlanView *view,
                                      const XrTargetPlan *target_plan,
                                      XrAotRefinementDiagnostic *diag);
XR_FUNC bool xr_aot_backend_run(const XrAotRefinementPlanView *view,
                                const XrTargetPlan *target_plan,
                                const XrAotBackendInterface *backend,
                                void *context, XrAotBackendStats *out_stats,
                                XrAotRefinementDiagnostic *diag);
XR_FUNC const XrAotBackendInterface *xr_aot_null_backend_interface(void);
XR_FUNC const XrAotBackendInterface *xr_aot_test_backend_interface(void);

#endif  // XR_AOT_REFINEMENT_H
