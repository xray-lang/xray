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

#define XR_ASSERTION_PLAN_SCHEMA_VERSION UINT32_C(1)
#define XR_ASSERTION_OPERAND_NONE UINT8_MAX
enum { XR_ASSERTION_MAX_OPERANDS = 3 };

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
    XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS,
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

static inline bool xr_assertion_classify_action_channels(
    XrCoreIntrinsicExpectedFailureChannel expected_channel, XrAssertionActionOutcome outcome,
    XrAssertionFailureKind *failure_kind) {
    if (!failure_kind ||
        (expected_channel != XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR &&
         expected_channel != XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC))
        return false;
    if (!outcome.returned_normally && outcome.has_typed_error && outcome.has_panic) {
        *failure_kind = XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS;
        return true;
    }
    unsigned observations = (outcome.returned_normally ? 1u : 0u) +
                            (outcome.has_typed_error ? 1u : 0u) +
                            (outcome.has_panic ? 1u : 0u);
    if (observations != 1u)
        return false;
    XrCoreIntrinsicExpectedFailureChannel observed = XR_CORE_INTRINSIC_FAILURE_CHANNEL_NONE;
    if (outcome.has_typed_error)
        observed = XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR;
    else if (outcome.has_panic)
        observed = XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC;
    if (observed == expected_channel) {
        *failure_kind = XR_ASSERTION_FAILURE_NONE;
    } else if (expected_channel == XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR) {
        *failure_kind = observed == XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC
                            ? XR_ASSERTION_FAILURE_UNEXPECTED_PANIC
                            : XR_ASSERTION_FAILURE_EXPECTED_TYPED_ERROR;
    } else {
        *failure_kind = observed == XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR
                            ? XR_ASSERTION_FAILURE_UNEXPECTED_TYPED_ERROR
                            : XR_ASSERTION_FAILURE_EXPECTED_PANIC;
    }
    return true;
}

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
        case XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS:
            return "conflicting-failure-channels";
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
        case XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS:
            return !failure->actual && !failure->expected && failure->caught_error &&
                   failure->caught_panic;
        default:
            return false;
    }
}

typedef struct XrAssertionFailureWriter {
    char *buffer;
    size_t capacity;
    size_t length;
    bool failed;
} XrAssertionFailureWriter;

static inline void xr_assertion_writer_append_bytes(XrAssertionFailureWriter *writer,
                                                    const char *bytes, size_t length) {
    if (!writer || !bytes || writer->failed || length > SIZE_MAX - writer->length) {
        if (writer)
            writer->failed = true;
        return;
    }
    size_t begin = writer->length;
    writer->length += length;
    if (writer->length > writer->capacity) {
        writer->failed = true;
        return;
    }
    if (writer->buffer) {
        for (size_t i = 0; i < length; i++)
            writer->buffer[begin + i] = bytes[i];
    }
}

static inline void xr_assertion_writer_append_cstr(XrAssertionFailureWriter *writer,
                                                   const char *text) {
    if (!text) {
        writer->failed = true;
        return;
    }
    size_t length = 0;
    while (text[length]) {
        if (length == SIZE_MAX) {
            writer->failed = true;
            return;
        }
        length++;
    }
    xr_assertion_writer_append_bytes(writer, text, length);
}

static inline void xr_assertion_writer_append_u32(XrAssertionFailureWriter *writer,
                                                  uint32_t value) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char) ('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    for (size_t i = 0; i < count / 2u; i++) {
        char swap = digits[i];
        digits[i] = digits[count - i - 1u];
        digits[count - i - 1u] = swap;
    }
    xr_assertion_writer_append_bytes(writer, digits, count);
}

static inline bool xr_assertion_failure_write(XrAssertionFailureWriter *writer,
                                              const XrAssertionFailure *failure) {
    if (!writer || !xr_assertion_failure_validate(failure))
        return false;
#define XR_ASSERTION_APPEND_LITERAL(value)                                                         \
    xr_assertion_writer_append_bytes(writer, value, sizeof(value) - 1u)
#define XR_ASSERTION_APPEND_OPTIONAL(label, value)                                                 \
    do {                                                                                           \
        if (value) {                                                                               \
            XR_ASSERTION_APPEND_LITERAL(label);                                                    \
            xr_assertion_writer_append_cstr(writer, value);                                        \
        }                                                                                          \
    } while (0)
    XR_ASSERTION_APPEND_LITERAL("AssertionFailure[");
    xr_assertion_writer_append_cstr(writer, xr_assertion_failure_kind_name(failure->kind));
    XR_ASSERTION_APPEND_LITERAL("] at ");
    xr_assertion_writer_append_cstr(writer, failure->source.file);
    XR_ASSERTION_APPEND_LITERAL(":");
    xr_assertion_writer_append_u32(writer, failure->source.line);
    XR_ASSERTION_APPEND_LITERAL(":");
    xr_assertion_writer_append_u32(writer, failure->source.column);
    XR_ASSERTION_APPEND_OPTIONAL("\n  message: ", failure->message);
    XR_ASSERTION_APPEND_OPTIONAL("\n  actual: ", failure->actual);
    XR_ASSERTION_APPEND_OPTIONAL("\n  expected: ", failure->expected);
    XR_ASSERTION_APPEND_OPTIONAL("\n  caught_error: ", failure->caught_error);
    XR_ASSERTION_APPEND_OPTIONAL("\n  caught_panic: ", failure->caught_panic);
#undef XR_ASSERTION_APPEND_OPTIONAL
#undef XR_ASSERTION_APPEND_LITERAL
    return !writer->failed;
}

/* Values are formatted by the language's canonical value formatter before
 * entering this representation-neutral renderer.  The writer intentionally
 * has no stdio or allocation dependency, so hosted and freestanding providers
 * consume identical bytes. */
static inline int xr_assertion_failure_render(char *buffer, size_t capacity,
                                              const XrAssertionFailure *failure) {
    if (!buffer || capacity == 0) {
        return -1;
    }
    XrAssertionFailureWriter writer = {
        .buffer = buffer,
        .capacity = capacity - 1u,
    };
    if (!xr_assertion_failure_write(&writer, failure) || writer.length > (size_t) INT32_MAX) {
        buffer[0] = '\0';
        return -1;
    }
    buffer[writer.length] = '\0';
    return (int) writer.length;
}

/* Measure through the same bounded writer as rendering. */
static inline int xr_assertion_failure_render_size(const XrAssertionFailure *failure) {
    XrAssertionFailureWriter writer = {.capacity = SIZE_MAX};
    if (!xr_assertion_failure_write(&writer, failure) || writer.length > (size_t) INT32_MAX)
        return -1;
    return (int) writer.length;
}

#endif /* XR_ASSERTION_PLAN_H */
