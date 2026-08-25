/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_scalar_call_decision.h - Sealed direct scalar call decision
 */

#ifndef XR_SCALAR_CALL_DECISION_H
#define XR_SCALAR_CALL_DECISION_H

#include "../semantic/xr_program_semantic_closure.h"
#include "xr_target_plan.h"
#include "xr_target_profile.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_SCALAR_CALL_DECISION_SCHEMA_VERSION UINT32_C(1)

typedef enum XrScalarCallEntryPolicy {
    XR_SCALAR_CALL_ENTRY_POLICY_INVALID = 0,
    XR_SCALAR_CALL_ENTRY_STATIC_DIRECT = 1,
} XrScalarCallEntryPolicy;

typedef enum XrScalarCallSlotPolicy {
    XR_SCALAR_CALL_SLOT_POLICY_INVALID = 0,
    XR_SCALAR_CALL_SLOT_REGISTER_ONLY = 1,
} XrScalarCallSlotPolicy;

typedef struct XrScalarCallDecisionSlot {
    uint8_t machine_rep;
    uint8_t mode;
    uint8_t ownership;
    uint8_t slot_policy;
} XrScalarCallDecisionSlot;

/* This sealed output owns no pointers, handles, entry cells, or cleanup data. */
typedef struct XrScalarCallDecision {
    uint32_t schema;
    uint16_t native_abi;
    uint8_t sealed;
    uint8_t calling_convention;
    uint8_t target_kind;
    uint8_t entry_policy;
    uint8_t argument_count;
    uint8_t result_count;
    uint8_t entry_cell_count;
    uint8_t adapter_count;
    uint8_t cleanup_count;
    uint8_t error_channel_count;
    uint8_t suspend_point_count;
    uint8_t reserved[1];
    uint64_t capability_mask;
    XrScalarCallDecisionSlot argument;
    XrScalarCallDecisionSlot result;
    XrGenerationClosureId generation_id;
    XrFingerprint closure_fingerprint;
    XrFingerprint target_profile_fingerprint;
    XrStableId call_identity;
    XrStableId callsite_identity;
    XrStableId caller_function;
    XrStableId callee_function;
    XrFingerprint fingerprint;
} XrScalarCallDecision;

XR_FUNC bool xr_scalar_call_decision_build(
    const XrProgramSemanticClosure *closure,
    XrGenerationClosureId expected_generation,
    const XrTargetProfile *target_profile, XrScalarCallDecision *out,
    char *error, size_t error_size);
XR_FUNC bool xr_scalar_call_decision_verify(
    const XrScalarCallDecision *decision,
    const XrProgramSemanticClosure *closure,
    const XrTargetProfile *target_profile, char *error, size_t error_size);

#endif  // XR_SCALAR_CALL_DECISION_H
