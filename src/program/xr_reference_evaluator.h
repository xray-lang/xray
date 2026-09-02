/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_reference_evaluator.h - Independent CoreSpec reference evaluator
 */

#ifndef XR_REFERENCE_EVALUATOR_H
#define XR_REFERENCE_EVALUATOR_H

#include "xr_program_verify.h"

typedef enum XrReferenceValueKind {
    XR_REFERENCE_VALUE_VOID = 0,
    XR_REFERENCE_VALUE_BOOL,
    XR_REFERENCE_VALUE_I64,
    XR_REFERENCE_VALUE_U32,
    XR_REFERENCE_VALUE_ERROR,
    XR_REFERENCE_VALUE_AGGREGATE,
} XrReferenceValueKind;

typedef struct XrReferenceValue {
    XrReferenceValueKind kind;
    union {
        bool boolean;
        int64_t i64;
        uint32_t u32;
        uint32_t error;
        const void *aggregate;
    } as;
} XrReferenceValue;

typedef struct XrReferenceProfile {
    uint32_t pointer_width;
} XrReferenceProfile;

typedef struct XrReferenceBudget {
    uint64_t max_steps;
    uint64_t max_value_cells;
    uint32_t max_call_depth;
} XrReferenceBudget;

typedef enum XrReferenceOutcomeKind {
    XR_REFERENCE_OUTCOME_RETURN = 0,
    XR_REFERENCE_OUTCOME_TRAP,
    XR_REFERENCE_OUTCOME_ERROR,
    XR_REFERENCE_OUTCOME_RESOURCE_LIMIT,
    XR_REFERENCE_OUTCOME_INVALID_INVOCATION,
} XrReferenceOutcomeKind;

typedef enum XrReferenceTrap {
    XR_REFERENCE_TRAP_NONE = 0,
    XR_REFERENCE_TRAP_INTEGER_OVERFLOW = 1,
    XR_REFERENCE_TRAP_INTEGER_DIVISION_BY_ZERO = 2,
    XR_REFERENCE_TRAP_INTEGER_DIVISION_OVERFLOW = 3,
    XR_REFERENCE_TRAP_EXPLICIT = 4,
    XR_REFERENCE_TRAP_PROFILE_UNAVAILABLE = 5,
    XR_REFERENCE_TRAP_VARIANT_TAG_MISMATCH = 6,
} XrReferenceTrap;

typedef struct XrReferenceOutcome {
    XrReferenceOutcomeKind kind;
    XrReferenceValue value;
    XrReferenceValue error_value;
    XrReferenceTrap trap;
    uint64_t steps;
} XrReferenceOutcome;

XR_FUNC XrReferenceBudget xr_reference_default_budget(void);
XR_FUNC XrReferenceOutcome xr_reference_evaluate(
    const XrValidatedProgram *program, uint32_t function_id, const XrReferenceValue *arguments,
    uint32_t argument_count, const XrReferenceProfile *profile, const XrReferenceBudget *budget);

#endif /* XR_REFERENCE_EVALUATOR_H */
