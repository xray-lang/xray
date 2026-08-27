/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_i64_overflow_decision.h - Sealed i64 overflow predicate decisions
 */

#ifndef XR_I64_OVERFLOW_DECISION_H
#define XR_I64_OVERFLOW_DECISION_H

#include "../semantic/xr_i64_overflow_predicate_semantics.h"
#include "../semantic/xr_program_semantic_closure.h"
#include "xr_target_plan.h"
#include "xr_target_profile.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_I64_OVERFLOW_DECISION_SCHEMA_VERSION UINT32_C(1)

typedef struct XrI64OverflowDecisionRow {
    XrStableId program_call;
    XrStableId callsite;
    XrStableId caller_function;
    XrStableId builtin_identity;
    XrFingerprint contract;
    XrFingerprint fingerprint;
    uint32_t program_row;
    uint32_t method_symbol;
    uint8_t kind;
    uint8_t receiver_rep;
    uint8_t argument_rep;
    uint8_t result_rep;
    uint8_t reserved[4];
} XrI64OverflowDecisionRow;

typedef struct XrI64OverflowDecisionTable {
    uint32_t schema;
    uint32_t row_count;
    uint16_t native_abi;
    uint8_t sealed;
    uint8_t reserved[1];
    XrGenerationClosureId generation_id;
    XrFingerprint closure_fingerprint;
    XrFingerprint target_profile_fingerprint;
    XrFingerprint fingerprint;
    XrI64OverflowDecisionRow *rows;
} XrI64OverflowDecisionTable;

XR_FUNC bool xr_i64_overflow_decision_build(
    const XrProgramSemanticClosure *closure, XrGenerationClosureId expected_generation,
    const XrTargetProfile *target_profile, XrI64OverflowDecisionTable *out, char *error,
    size_t error_size);
XR_FUNC bool xr_i64_overflow_decision_verify(
    const XrI64OverflowDecisionTable *table, const XrProgramSemanticClosure *closure,
    const XrTargetProfile *target_profile, char *error, size_t error_size);
XR_FUNC const XrI64OverflowDecisionRow *xr_i64_overflow_decision_for_program_row(
    const XrI64OverflowDecisionTable *table, uint32_t program_row);
XR_FUNC void xr_i64_overflow_decision_dispose(XrI64OverflowDecisionTable *table);

#endif  // XR_I64_OVERFLOW_DECISION_H
