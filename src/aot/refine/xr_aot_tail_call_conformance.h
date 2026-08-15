/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_tail_call_conformance.h - Exact native tail-call conformance gate
 */

#ifndef XR_AOT_TAIL_CALL_CONFORMANCE_H
#define XR_AOT_TAIL_CALL_CONFORMANCE_H

#include "xr_aot_refinement.h"

struct XiFunc;

typedef enum XrAotTailCallConformanceIssue {
    XR_AOT_TAIL_CALL_CONFORMANCE_OK = 0,
    XR_AOT_TAIL_CALL_CONFORMANCE_INVALID_ARGUMENT,
    XR_AOT_TAIL_CALL_CONFORMANCE_PLAN_STATE,
    XR_AOT_TAIL_CALL_CONFORMANCE_SOURCE_IDENTITY,
    XR_AOT_TAIL_CALL_CONFORMANCE_LIVE_OPCODE,
    XR_AOT_TAIL_CALL_CONFORMANCE_OPERAND_MAPPING,
    XR_AOT_TAIL_CALL_CONFORMANCE_CALL_AUTHORITY,
    XR_AOT_TAIL_CALL_CONFORMANCE_DIRECT_CALL_RECORD,
    XR_AOT_TAIL_CALL_CONFORMANCE_CALLEE_MAPPING,
    XR_AOT_TAIL_CALL_CONFORMANCE_INCOMPLETE_COVERAGE,
    XR_AOT_TAIL_CALL_CONFORMANCE_RESOURCE_BUDGET,
} XrAotTailCallConformanceIssue;

typedef struct XrAotTailCallDiagnostic {
    uint32_t issue;
    uint32_t semantic_operation;
    uint32_t target_call_index;
    uint32_t semantic_function;
    uint32_t semantic_value;
} XrAotTailCallDiagnostic;

typedef struct XrAotTailCallConformance {
    XrFingerprint semantic_fingerprint;
    XrFingerprint target_plan_fingerprint;
    XrFingerprint direct_call_authority_fingerprint;
    XrFingerprint fingerprint;
    uint32_t tail_call_count;
    uint32_t reserved;
} XrAotTailCallConformance;

XR_FUNC const char *xr_aot_tail_call_conformance_issue_name(uint32_t issue);

/* Prove that every frozen XI_TAIL_CALL has one exact live Xi member, one
 * verified direct-local TargetPlan call and one applied direct-call authority
 * record. The certificate contains no Xi pointers and is deterministic across
 * processes; ordinary XI_CALL is never accepted as a materialized alias. */
XR_FUNC bool xr_aot_tail_call_conformance_verify(
    const struct XiFunc *root, const XrTargetPlan *target_plan,
    const XrAotRefinementPlanView *direct_call_authority,
    XrAotTailCallConformance *out_conformance,
    XrAotTailCallDiagnostic *diag);

#endif  // XR_AOT_TAIL_CALL_CONFORMANCE_H
