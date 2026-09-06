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

#include "../execution/xr_execution.h"

typedef enum XrReferenceValueKind {
    XR_REFERENCE_VALUE_VOID = 0,
    XR_REFERENCE_VALUE_BOOL,
    XR_REFERENCE_VALUE_I64,
    XR_REFERENCE_VALUE_U32,
    XR_REFERENCE_VALUE_U16,
    XR_REFERENCE_VALUE_TARGET_OS,
    XR_REFERENCE_VALUE_TARGET_ARCH,
    XR_REFERENCE_VALUE_TARGET_ABI,
    XR_REFERENCE_VALUE_TARGET_ENDIAN,
    XR_REFERENCE_VALUE_ERROR,
    XR_REFERENCE_VALUE_PANIC_INFO,
    XR_REFERENCE_VALUE_AGGREGATE,
    XR_REFERENCE_VALUE_EXISTENTIAL,
    XR_REFERENCE_VALUE_CALLABLE,
} XrReferenceValueKind;

typedef struct XrReferenceValue {
    XrReferenceValueKind kind;
    union {
        bool boolean;
        int64_t i64;
        uint32_t u32;
        uint16_t u16;
        uint16_t target_enum;
        uint32_t error;
        uint32_t panic_info;
        const void *aggregate;
        const void *existential;
        const void *callable;
    } as;
} XrReferenceValue;

typedef struct XrReferenceProfile {
    uint16_t pointer_width;
    uint16_t operating_system;
    uint16_t architecture;
    uint16_t native_abi;
    uint16_t endianness;
} XrReferenceProfile;

typedef struct XrReferenceBudget {
    uint64_t max_steps;
    uint64_t max_value_cells;
    uint32_t max_call_depth;
} XrReferenceBudget;

typedef bool (*XrReferenceProviderCallI64Unary)(void *context, uint32_t requirement_index,
                                                uint32_t operation_index, int64_t argument,
                                                int64_t *result_out);
typedef bool (*XrReferenceProviderCallI64Nullary)(void *context, uint32_t requirement_index,
                                                  uint32_t operation_index, int64_t *result_out);
typedef bool (*XrReferenceProviderCallBoolI64Unary)(void *context,
                                                    uint32_t requirement_index,
                                                    uint32_t operation_index,
                                                    int64_t argument, bool *result_out);
typedef bool (*XrReferenceProviderCallOptionalI64PairNullary)(void *context,
                                                              uint32_t requirement_index,
                                                              uint32_t operation_index,
                                                              bool *present_out, int64_t *first_out,
                                                              int64_t *second_out);
typedef bool (*XrReferenceProviderOutputWrite)(void *context, uint32_t requirement_index,
                                               uint32_t operation_index, const uint8_t *bytes,
                                               size_t size);

typedef struct XrReferenceProviderBinding {
    void *context;
    XrReferenceProviderCallI64Unary call_i64_unary;
    XrReferenceProviderCallI64Nullary call_i64_nullary;
    XrReferenceProviderCallBoolI64Unary call_bool_i64_unary;
    XrReferenceProviderCallOptionalI64PairNullary call_optional_i64_pair_nullary;
    XrReferenceProviderOutputWrite output_write;
} XrReferenceProviderBinding;

typedef enum XrReferenceOutcomeKind {
    XR_REFERENCE_OUTCOME_RETURN = 0,
    XR_REFERENCE_OUTCOME_SUSPENDED,
    XR_REFERENCE_OUTCOME_TRAP,
    XR_REFERENCE_OUTCOME_ERROR,
    XR_REFERENCE_OUTCOME_PANIC,
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
    XR_REFERENCE_TRAP_PROVIDER_CALL_FAILED = 7,
} XrReferenceTrap;

typedef struct XrReferenceOutcome {
    XrReferenceOutcomeKind kind;
    bool owns_dynamic_values;
    uint8_t reserved8[3];
    XrReferenceValue value;
    XrReferenceValue error_value;
    XrReferenceValue panic_value;
    XrReferenceTrap trap;
    uint64_t steps;
    uint32_t state_id;
    uint32_t safepoint_id;
} XrReferenceOutcome;

typedef struct XrReferenceAggregateView {
    uint16_t type_id;
    uint16_t reserved16;
    uint32_t variant_ordinal;
    const XrReferenceValue *fields;
    uint32_t field_count;
} XrReferenceAggregateView;

typedef struct XrReferenceExecution XrReferenceExecution;

XR_FUNC XrReferenceBudget xr_reference_default_budget(void);
XR_FUNC XrReferenceOutcome xr_reference_evaluate(
    const XrValidatedProgram *program, uint32_t function_id, const XrReferenceValue *arguments,
    uint32_t argument_count, const XrReferenceProfile *profile, const XrReferenceBudget *budget);
XR_FUNC XrReferenceOutcome xr_reference_evaluate_bound(
    const XrValidatedProgram *program, uint32_t function_id, const XrReferenceValue *arguments,
    uint32_t argument_count, const XrReferenceProfile *profile, const XrReferenceBudget *budget,
    const XrReferenceProviderBinding *providers);
/* Aggregate values returned by the one-shot evaluator are detached from its
 * private arena. Views borrow that storage until the outcome is disposed. */
XR_FUNC bool xr_reference_value_aggregate_view(const XrReferenceValue *value,
                                                XrReferenceAggregateView *view_out);
XR_FUNC void xr_reference_outcome_dispose(XrReferenceOutcome *outcome);
XR_FUNC bool xr_reference_execution_create(XrInstance *instance, uint32_t function_id,
                                           const XrReferenceValue *arguments,
                                           uint32_t argument_count, const XrReferenceBudget *budget,
                                           XrReferenceExecution **execution_out);
XR_FUNC XrReferenceOutcome xr_reference_execution_step(XrReferenceExecution *execution);
XR_FUNC void xr_reference_execution_free(XrReferenceExecution *execution);

#endif /* XR_REFERENCE_EVALUATOR_H */
