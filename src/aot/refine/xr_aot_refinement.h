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

#define XR_AOT_REFINEMENT_SCHEMA_VERSION UINT32_C(5)
#define XR_AOT_REFINEMENT_MAX_RECORDS UINT32_C(1048576)

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
    XR_AOT_TRANSFORM_REPRESENTATION_ADAPTER,
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
    XR_AOT_REFINEMENT_SOURCE_IDENTITY,
    XR_AOT_REFINEMENT_USE_SITE,
    XR_AOT_REFINEMENT_SOURCE_TYPE,
    XR_AOT_REFINEMENT_REPRESENTATION,
    XR_AOT_REFINEMENT_LAYOUT,
    XR_AOT_REFINEMENT_RECORD_FINGERPRINT,
    XR_AOT_REFINEMENT_REPRESENTATION_SCHEMA_UNAVAILABLE,
    XR_AOT_REFINEMENT_DUPLICATE_USE,
    XR_AOT_REFINEMENT_NONCANONICAL_ORDER,
    XR_AOT_REFINEMENT_INCOMPLETE_COVERAGE,
    XR_AOT_REFINEMENT_RESOURCE_BUDGET,
    XR_AOT_REFINEMENT_PLAN_FINGERPRINT,
    XR_AOT_REFINEMENT_PLAN_STATE,
    /* Direct-call refusals. Each names the exact obligation the validator
     * could not discharge from the verified baseline, so an unproved binding
     * is distinguishable from a checker-detected inconsistency. */
    XR_AOT_REFINEMENT_DIRECT_CALL_TARGET_NOT_CLOSED,
    XR_AOT_REFINEMENT_DIRECT_CALL_CALLEE_IDENTITY,
    XR_AOT_REFINEMENT_DIRECT_CALL_ARGUMENT_MAPPING,
    XR_AOT_REFINEMENT_DIRECT_CALL_RESULT_MAPPING,
    XR_AOT_REFINEMENT_DIRECT_CALL_ERROR_MAPPING,
    XR_AOT_REFINEMENT_DIRECT_CALL_ENVIRONMENT_MAPPING,
    XR_AOT_REFINEMENT_DIRECT_CALL_GENERATION_MAPPING,
    XR_AOT_REFINEMENT_DIRECT_CALL_EFFECT_MAPPING,
} XrAotRefinementIssue;

typedef struct XrAotRefinementDiagnostic {
    uint32_t issue;
    uint32_t record_index;
    uint32_t pass_id;
    uint32_t target_call_index;
    uint32_t semantic_value;
    uint32_t semantic_operation;
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

/* The request names an existing TargetPlan call row; it does not manufacture
 * one and it carries no conclusion about that row. Every fact consumed by a
 * backend lives in the derived XrAotDirectCallRecord below, which the
 * validator recomputes from the verified baseline on its own. */
typedef struct XrAotDirectCallRequest {
    uint32_t target_call_index;
} XrAotDirectCallRequest;

/* A statically bound direct call, re-derived from the verified TargetPlan and
 * the semantic module that owns each global function rather than copied from
 * whatever proposed the binding. A record is only APPLIED when the exact
 * global call instruction, caller/callee symbol identities, parameter,
 * return, error, ownership, environment and generation mappings are each
 * discharged against their owning semantic contracts. */
typedef struct XrAotDirectCallRecord {
    uint32_t target_call_index;
    uint32_t target_instruction;
    uint32_t caller_function;
    uint32_t callee_function;
    uint32_t semantic_call_target;
    uint32_t semantic_operation;
    uint32_t argument_begin;
    uint32_t result_value;
    uint32_t result_slot;
    uint32_t error_slot;
    uint16_t argument_count;
    uint16_t parameter_count;
    uint16_t call_flags;
    uint16_t result_register_rep;
    uint16_t result_memory_rep;
    uint16_t native_abi;
    uint8_t target_kind;
    uint8_t calling_convention;
    uint8_t result_mode;
    uint8_t result_ownership;
    uint8_t error_mode;
    uint8_t semantic_target_kind;
    uint8_t environment_required;
    uint8_t generation_required;
    XrStableId caller_identity;
    XrStableId callee_identity;
    XrStableId operation_id;
    XrFingerprint argument_map_fingerprint;
    XrFingerprint fingerprint;
} XrAotDirectCallRecord;

typedef enum XrAotRepresentationAdapterKind {
    XR_AOT_REP_ADAPTER_BOX = 1,
    XR_AOT_REP_ADAPTER_UNBOX,
    XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_BOX,
    XR_AOT_REP_ADAPTER_ENUM_DESCRIPTOR_UNBOX,
    XR_AOT_REP_ADAPTER_COUNT,
} XrAotRepresentationAdapterKind;

typedef enum XrAotRepresentationUseKind {
    XR_AOT_REP_USE_OPERATION = 1,
    XR_AOT_REP_USE_BLOCK_CONTROL,
} XrAotRepresentationUseKind;

typedef enum XrAotRepresentationSourceKind {
    XR_AOT_REP_SOURCE_OPERATION = 1,
    XR_AOT_REP_SOURCE_PARAMETER,
} XrAotRepresentationSourceKind;

typedef enum XrAotRepresentationRecipe {
    XR_AOT_REP_RECIPE_NONE = 0,
    XR_AOT_REP_RECIPE_BOX_INTEGER,
    XR_AOT_REP_RECIPE_UNBOX_INTEGER,
    XR_AOT_REP_RECIPE_BOX_FLOAT,
    XR_AOT_REP_RECIPE_UNBOX_FLOAT,
    XR_AOT_REP_RECIPE_BOX_REFERENCE,
    XR_AOT_REP_RECIPE_UNBOX_REFERENCE,
    XR_AOT_REP_RECIPE_COUNT,
} XrAotRepresentationRecipe;

typedef struct XrAotRepresentationAdapterRequest {
    uint32_t source_value;
    uint32_t use_operation;
    uint32_t use_block;
    uint16_t use_operand;
    uint16_t use_kind;
    uint16_t adapter_kind;
    uint16_t input_rep_kind;
    uint16_t output_rep_kind;
    uint32_t layout;
    XrFingerprint policy_fingerprint;
} XrAotRepresentationAdapterRequest;

typedef struct XrAotRepresentationAdapterRecord {
    uint32_t source_function;
    uint32_t source_value;
    uint32_t source_operation;
    uint32_t source_type;
    uint16_t source_kind;
    uint16_t reserved;
    uint32_t use_operation;
    uint32_t use_block;
    uint16_t use_operand;
    uint16_t use_kind;
    uint16_t adapter_kind;
    uint16_t recipe;
    uint16_t input_rep_kind;
    uint16_t output_rep_kind;
    uint16_t target_register_rep;
    uint16_t target_memory_rep;
    uint32_t target_slot;
    uint32_t layout;
    uint8_t source_auxiliary_kind;
    uint8_t source_flags;
    uint8_t use_auxiliary_kind;
    uint8_t use_flags;
    int64_t source_semantic_immediate;
    int64_t use_semantic_immediate;
    XrStableId source_operation_id;
    XrStableId source_type_id;
    XrStableId use_operation_id;
    XrFingerprint policy_fingerprint;
    XrFingerprint machine_rep_fingerprint;
    XrFingerprint layout_fingerprint;
    XrFingerprint fingerprint;
} XrAotRepresentationAdapterRecord;

typedef struct XrAotTransformationRecord {
    XrAotPassProtocol protocol;
    XrAotInvariantState input_state;
    XrAotInvariantState output_state;
    XrAotDirectCallRequest direct_call;
    XrAotDirectCallRecord direct_call_binding;
    XrAotRepresentationAdapterRecord representation_adapter;
    uint16_t decision;
    uint16_t transform_kind;
    uint32_t diagnostic_issue;
    XrFingerprint fingerprint;
} XrAotTransformationRecord;

typedef struct XrAotRefinementPlan XrAotRefinementPlan;
typedef struct XrAotRefinementBuilder XrAotRefinementBuilder;

typedef struct XrAotRefinementPlanView {
    uint32_t schema_version;
    XrAotBaselineRef baseline;
    XrAotInvariantState initial_state;
    const XrAotTransformationRecord *records;
    uint32_t record_count;
    XrFingerprint fingerprint;
    bool frozen;
    bool verified;
} XrAotRefinementPlanView;

XR_FUNC const char *xr_aot_refinement_issue_name(uint32_t issue);
XR_FUNC bool xr_aot_refinement_baseline_from_target_plan(
    const XrTargetPlan *target_plan, XrAotBaselineRef *out_baseline,
    XrAotRefinementDiagnostic *diag);
XR_FUNC XrAotInvariantState xr_aot_refinement_initial_state(
    const XrAotBaselineRef *baseline);
XR_FUNC XrAotPassProtocol xr_aot_refinement_direct_call_protocol(
    uint32_t pass_id);
XR_FUNC XrAotPassProtocol xr_aot_refinement_representation_protocol(
    uint32_t pass_id);
XR_FUNC XrAotRefinementBuilder *xr_aot_refinement_builder_create(
    const XrTargetPlan *target_plan, XrAotRefinementDiagnostic *diag);
XR_FUNC void xr_aot_refinement_builder_free(XrAotRefinementBuilder *builder);
XR_FUNC bool xr_aot_refinement_try_direct_call(
    XrAotRefinementBuilder *builder, const XrAotPassProtocol *protocol,
    const XrTargetPlan *target_plan, const XrAotDirectCallRequest *request,
    uint32_t *out_decision, XrAotRefinementDiagnostic *diag);
XR_FUNC bool xr_aot_refinement_try_representation_adapter(
    XrAotRefinementBuilder *builder, const XrAotPassProtocol *protocol,
    const XrTargetPlan *target_plan,
    const XrAotRepresentationAdapterRequest *request,
    uint32_t *out_decision,
    XrAotRefinementDiagnostic *diag);
/* Production entry point: record one direct-call transformation per call row
 * of the verified TargetPlan, then freeze and independently verify the plan.
 * Non-program rows whose binding cannot be proved are recorded as refusals
 * with a stable diagnostic. PROGRAM_DIRECT is already a static program-graph
 * commitment, so it must produce one APPLIED global binding or fail closed. */
XR_FUNC bool xr_aot_refinement_direct_call_authority_build(
    const XrTargetPlan *target_plan, uint32_t pass_id,
    XrAotRefinementPlan **out_plan, XrAotRefinementDiagnostic *diag);
XR_FUNC bool xr_aot_refinement_builder_freeze(
    XrAotRefinementBuilder *builder, const XrTargetPlan *target_plan,
    XrAotRefinementPlan **out_plan, XrAotRefinementDiagnostic *diag);
XR_FUNC void xr_aot_refinement_plan_free(XrAotRefinementPlan *plan);
XR_FUNC XrAotRefinementPlanView xr_aot_refinement_plan_view(
    const XrAotRefinementPlan *plan);
XR_FUNC bool xr_aot_refinement_verify(const XrAotRefinementPlanView *view,
                                      const XrTargetPlan *target_plan,
                                      XrAotRefinementDiagnostic *diag);

#endif  // XR_AOT_REFINEMENT_H
