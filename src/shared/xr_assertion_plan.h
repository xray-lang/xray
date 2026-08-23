/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_assertion_plan.h - Target-neutral assertion plan and failure schema
 *
 * CoreIntrinsicRegistry owns public binding identity.  This schema is a typed
 * projection of those rows; it never resolves source spellings or chooses a
 * second semantic operation.  Executors consume the resulting plan and share
 * the same action-outcome classifier and failure renderer.
 */

#ifndef XR_ASSERTION_PLAN_H
#define XR_ASSERTION_PLAN_H

#include "../base/xlocation.h"
#include "xr_core_intrinsic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define XR_ASSERTION_PLAN_SCHEMA_VERSION UINT32_C(1)
#define XR_ASSERTION_OPERAND_NONE UINT8_MAX

typedef enum XrAssertionKind {
    XR_ASSERTION_KIND_NONE = 0,
    XR_ASSERTION_KIND_CONDITION,
    XR_ASSERTION_KIND_EQUAL,
    XR_ASSERTION_KIND_THROWS,
    XR_ASSERTION_KIND_PANICS,
    XR_ASSERTION_KIND_COUNT,
} XrAssertionKind;

typedef enum XrAssertionEvaluationStep {
    XR_ASSERTION_EVAL_NONE = 0,
    XR_ASSERTION_EVAL_CONDITION,
    XR_ASSERTION_EVAL_ACTUAL,
    XR_ASSERTION_EVAL_EXPECTED,
    XR_ASSERTION_EVAL_ACTION,
    XR_ASSERTION_EVAL_MESSAGE,
    XR_ASSERTION_EVAL_COUNT,
} XrAssertionEvaluationStep;

typedef enum XrAssertionEqualityAuthority {
    XR_ASSERTION_EQUALITY_NONE = 0,
    XR_ASSERTION_EQUALITY_LANGUAGE_DEEP,
    XR_ASSERTION_EQUALITY_COUNT,
} XrAssertionEqualityAuthority;

typedef enum XrAssertionCapability {
    XR_ASSERTION_CAPABILITY_NONE = 0,
    XR_ASSERTION_CAPABILITY_FAILURE_REPORT = 1u << 0,
    XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY = 1u << 1,
    XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY = 1u << 2,
    XR_ASSERTION_CAPABILITY_ALL = XR_ASSERTION_CAPABILITY_FAILURE_REPORT |
                                  XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY |
                                  XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY,
} XrAssertionCapability;

typedef enum XrAssertionPlanFlag {
    XR_ASSERTION_PLAN_FLAG_NONE = 0,
    XR_ASSERTION_PLAN_FLAG_FAILURE_EDGE_COLD = 1u << 0,
} XrAssertionPlanFlag;

typedef enum XrAssertionPlanStatus {
    XR_ASSERTION_PLAN_OK = 0,
    XR_ASSERTION_PLAN_INVALID_ARGUMENT,
    XR_ASSERTION_PLAN_NOT_ASSERTION,
    XR_ASSERTION_PLAN_INVALID_ARITY,
    XR_ASSERTION_PLAN_UNSUPPORTED_TARGET,
    XR_ASSERTION_PLAN_MISSING_CAPABILITY,
    XR_ASSERTION_PLAN_INVALID_SCHEMA,
} XrAssertionPlanStatus;

typedef struct XrAssertionPlan {
    uint32_t schema_version;
    XrCoreBuiltinId builtin_id;
    XrAssertionKind kind;
    XrLocation source;
    XrAssertionEqualityAuthority equality_authority;
    XrCoreIntrinsicExpectedFailureChannel expected_failure_channel;
    XrCoreIntrinsicFlowRule flow_rule;
    XrCoreIntrinsicEffectKind effect;
    uint32_t target;
    uint32_t required_capabilities;
    uint32_t flags;
    uint16_t arity;
    uint8_t message_operand;
    uint8_t evaluation_count;
    XrAssertionEvaluationStep evaluation_order[3];
} XrAssertionPlan;

typedef enum XrAssertionFailureKind {
    /* NONE means that the observed action outcome satisfied the assertion. */
    XR_ASSERTION_FAILURE_NONE = 0,
    XR_ASSERTION_FAILURE_CONDITION_FALSE,
    XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL,
    XR_ASSERTION_FAILURE_EXPECTED_TYPED_ERROR,
    XR_ASSERTION_FAILURE_EXPECTED_PANIC,
    XR_ASSERTION_FAILURE_UNEXPECTED_PANIC,
    XR_ASSERTION_FAILURE_UNEXPECTED_TYPED_ERROR,
    XR_ASSERTION_FAILURE_COUNT,
} XrAssertionFailureKind;

typedef struct XrAssertionFailure {
    XrAssertionFailureKind kind;
    XrLocation source;
    const char *message;
    const char *actual;
    const char *expected;
    const char *caught_error;
    const char *caught_panic;
} XrAssertionFailure;

/* Executor boundaries populate all three observations before classification.
 * Exactly one must be true, so a corrupted state cannot silently prefer the
 * typed-error or panic channel. */
typedef struct XrAssertionActionOutcome {
    bool returned_normally;
    bool has_typed_error;
    bool has_panic;
} XrAssertionActionOutcome;

XR_FUNC XrAssertionPlanStatus xr_assertion_plan_build(XrCoreBuiltinId builtin_id, uint16_t arity,
                                                      XrLocation source, uint32_t target,
                                                      uint32_t available_capabilities,
                                                      XrAssertionPlan *out);
XR_FUNC bool xr_assertion_plan_validate(const XrAssertionPlan *plan);
XR_FUNC bool xr_assertion_classify_action_outcome(
    const XrAssertionPlan *plan, XrCoreIntrinsicExpectedFailureChannel observed_channel,
    XrAssertionFailureKind *failure_kind);
XR_FUNC bool xr_assertion_classify_action_result(const XrAssertionPlan *plan,
                                                 XrAssertionActionOutcome outcome,
                                                 XrAssertionFailureKind *failure_kind);

static inline bool xr_assertion_location_is_complete(XrLocation source) {
    return source.file && source.file[0] && source.line != 0 && source.column != 0 &&
           source.end_line != 0 && source.end_column != 0 && source.end_line >= source.line &&
           (source.end_line != source.line || source.end_column >= source.column);
}

static inline const char *xr_assertion_failure_kind_name(XrAssertionFailureKind kind) {
    switch (kind) {
        case XR_ASSERTION_FAILURE_CONDITION_FALSE:
            return "condition-false";
        case XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL:
            return "values-not-equal";
        case XR_ASSERTION_FAILURE_EXPECTED_TYPED_ERROR:
            return "expected-typed-error";
        case XR_ASSERTION_FAILURE_EXPECTED_PANIC:
            return "expected-panic";
        case XR_ASSERTION_FAILURE_UNEXPECTED_PANIC:
            return "unexpected-panic";
        case XR_ASSERTION_FAILURE_UNEXPECTED_TYPED_ERROR:
            return "unexpected-typed-error";
        default:
            return NULL;
    }
}

static inline bool xr_assertion_failure_validate(const XrAssertionFailure *failure) {
    if (!failure || !xr_assertion_location_is_complete(failure->source) ||
        !xr_assertion_failure_kind_name(failure->kind))
        return false;
    switch (failure->kind) {
        case XR_ASSERTION_FAILURE_CONDITION_FALSE:
            return !failure->actual && !failure->expected && !failure->caught_error &&
                   !failure->caught_panic;
        case XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL:
            return failure->actual && failure->expected && !failure->caught_error &&
                   !failure->caught_panic;
        case XR_ASSERTION_FAILURE_EXPECTED_TYPED_ERROR:
        case XR_ASSERTION_FAILURE_EXPECTED_PANIC:
            return !failure->actual && !failure->expected && !failure->caught_error &&
                   !failure->caught_panic;
        case XR_ASSERTION_FAILURE_UNEXPECTED_PANIC:
            return !failure->actual && !failure->expected && !failure->caught_error &&
                   failure->caught_panic;
        case XR_ASSERTION_FAILURE_UNEXPECTED_TYPED_ERROR:
            return !failure->actual && !failure->expected && failure->caught_error &&
                   !failure->caught_panic;
        default:
            return false;
    }
}

/* Values are formatted by the language's canonical value formatter before
 * entering this representation-neutral renderer.  Both executors therefore
 * share field presence, ordering, labels, and bytes without this layer knowing
 * either tagged VM values or native AOT representations. */
static inline int xr_assertion_failure_render(char *buffer, size_t capacity,
                                              const XrAssertionFailure *failure) {
    if (!buffer || capacity == 0 || !xr_assertion_failure_validate(failure))
        return -1;
    const char *kind = xr_assertion_failure_kind_name(failure->kind);
    int written = snprintf(
        buffer, capacity, "AssertionFailure[%s] at %s:%u:%u%s%s%s%s%s%s%s%s%s%s", kind,
        failure->source.file, failure->source.line, failure->source.column,
        failure->message ? "\n  message: " : "", failure->message ? failure->message : "",
        failure->actual ? "\n  actual: " : "", failure->actual ? failure->actual : "",
        failure->expected ? "\n  expected: " : "", failure->expected ? failure->expected : "",
        failure->caught_error ? "\n  caught_error: " : "",
        failure->caught_error ? failure->caught_error : "",
        failure->caught_panic ? "\n  caught_panic: " : "",
        failure->caught_panic ? failure->caught_panic : "");
    if (written < 0 || (size_t) written >= capacity) {
        buffer[0] = '\0';
        return -1;
    }
    return written;
}

#endif /* XR_ASSERTION_PLAN_H */
