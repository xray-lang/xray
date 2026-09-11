/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_vm.h - Private typed executor for validated XrProgram graphs
 */

#ifndef XR_PROGRAM_VM_H
#define XR_PROGRAM_VM_H

#include "../execution/xr_execution.h"

#define XR_VM_CODE_OPTIONS_SCHEMA_VERSION UINT32_C(1)
#define XR_VM_BUILD_ID "xray-program-vm-v1"

typedef enum XrVmDecodePolicy {
    XR_VM_DECODE_INVALID = 0,
    XR_VM_DECODE_BASELINE_VIEW = 1,
    XR_VM_DECODE_FIXED_ROWS = 2,
} XrVmDecodePolicy;

typedef enum XrVmQuickeningPolicy {
    XR_VM_QUICKENING_NONE = 0,
} XrVmQuickeningPolicy;

typedef enum XrVmLifecycleEventKind {
    XR_VM_EVENT_CLASS_CONSTRUCT = 1,
    XR_VM_EVENT_CLASS_SHARE,
    XR_VM_EVENT_CLASS_COPY,
    XR_VM_EVENT_CLASS_FIELD_LOAD,
    XR_VM_EVENT_CLASS_FIELD_PLACE,
    XR_VM_EVENT_PLACE_EXCHANGE,
    XR_VM_EVENT_OWNER_DROP,
    XR_VM_EVENT_CLASS_FINALIZE,
    XR_VM_EVENT_CLASS_RECLAIM,
} XrVmLifecycleEventKind;

typedef enum XrVmLifecycleEventOrigin {
    XR_VM_EVENT_ORIGIN_PROGRAM_OPERATION = 1,
    XR_VM_EVENT_ORIGIN_FIELD_FINALIZATION,
    XR_VM_EVENT_ORIGIN_CLONE_ROLLBACK,
    XR_VM_EVENT_ORIGIN_DOMAIN_TEARDOWN,
} XrVmLifecycleEventOrigin;

typedef struct XrVmLifecycleEvent {
    XrVmLifecycleEventKind kind;
    XrVmLifecycleEventOrigin origin;
    uint16_t type_id;
    uint16_t reserved16;
    uint32_t field_ordinal;
    uint64_t identity;
    uint64_t related_identity;
    uint32_t previous_value_kind;
    uint32_t replacement_value_kind;
    int64_t previous_i64;
    int64_t replacement_i64;
} XrVmLifecycleEvent;

typedef void (*XrVmLifecycleEventHandler)(void *context, const XrVmLifecycleEvent *event);

typedef struct XrVmCodeOptions {
    uint32_t schema_version;
    uint8_t decode_policy;
    uint8_t quickening_policy;
    uint16_t reserved16;
    uint64_t max_steps;
    uint32_t max_value_cells;
    uint32_t max_call_depth;
    /* Optional semantic observer. It never participates in code identity. */
    void *lifecycle_context;
    XrVmLifecycleEventHandler lifecycle_event;
} XrVmCodeOptions;

typedef enum XrVmValueKind {
    XR_VM_VALUE_VOID = 0,
    XR_VM_VALUE_BOOL,
    XR_VM_VALUE_I64,
    XR_VM_VALUE_U32,
    XR_VM_VALUE_U16,
    XR_VM_VALUE_TARGET_OS,
    XR_VM_VALUE_TARGET_ARCH,
    XR_VM_VALUE_TARGET_ABI,
    XR_VM_VALUE_TARGET_ENDIAN,
    XR_VM_VALUE_ERROR,
    XR_VM_VALUE_PANIC_INFO,
    XR_VM_VALUE_AGGREGATE,
    XR_VM_VALUE_CLASS_REFERENCE,
    XR_VM_VALUE_EXISTENTIAL,
    XR_VM_VALUE_CALLABLE,
} XrVmValueKind;

typedef struct XrVmValue {
    XrVmValueKind kind;
    union {
        bool boolean;
        int64_t i64;
        uint32_t u32;
        uint16_t u16;
        uint16_t target_enum;
        uint32_t error;
        uint32_t panic_info;
        const void *aggregate;
        const void *class_reference;
        const void *existential;
        const void *callable;
    } as;
} XrVmValue;

typedef enum XrVmOutcomeKind {
    XR_VM_OUTCOME_RETURN = 0,
    XR_VM_OUTCOME_SUSPENDED,
    XR_VM_OUTCOME_CANCELLED,
    XR_VM_OUTCOME_TRAP,
    XR_VM_OUTCOME_ERROR,
    XR_VM_OUTCOME_PANIC,
    XR_VM_OUTCOME_RESOURCE_LIMIT,
    XR_VM_OUTCOME_INVALID_INVOCATION,
    XR_VM_OUTCOME_STALE_CODE,
} XrVmOutcomeKind;

typedef enum XrVmTrap {
    XR_VM_TRAP_NONE = 0,
    XR_VM_TRAP_INTEGER_OVERFLOW = 1,
    XR_VM_TRAP_INTEGER_DIVISION_BY_ZERO = 2,
    XR_VM_TRAP_INTEGER_DIVISION_OVERFLOW = 3,
    XR_VM_TRAP_EXPLICIT = 4,
    XR_VM_TRAP_PROFILE_UNAVAILABLE = 5,
    XR_VM_TRAP_VARIANT_TAG_MISMATCH = 6,
    XR_VM_TRAP_PROVIDER_CALL_FAILED = 7,
} XrVmTrap;

typedef struct XrVmOutcome {
    XrVmOutcomeKind kind;
    bool owns_dynamic_values;
    uint8_t reserved8[3];
    XrVmValue value;
    XrVmValue error_value;
    XrVmValue panic_value;
    XrVmTrap trap;
    uint64_t steps;
    uint32_t state_id;
    uint32_t safepoint_id;
    XrSuspensionRequest suspension;
    XrFingerprint logical_trace;
} XrVmOutcome;

typedef struct XrVmAggregateView {
    uint16_t type_id;
    uint16_t reserved16;
    uint32_t variant_ordinal;
    const XrVmValue *fields;
    uint32_t field_count;
} XrVmAggregateView;

typedef enum XrVmCodeStatus {
    XR_VM_CODE_OK = 0,
    XR_VM_CODE_INVALID_INPUT,
    XR_VM_CODE_UNSUPPORTED_OPERATION,
    XR_VM_CODE_INSTANCE_UNAVAILABLE,
    XR_VM_CODE_POLICY_REJECTED,
    XR_VM_CODE_OUT_OF_MEMORY,
} XrVmCodeStatus;

typedef struct XrVmCodeDiagnostic {
    XrVmCodeStatus status;
    uint16_t operation_id;
    uint16_t reserved16;
    uint32_t function_id;
    uint32_t block_id;
    uint32_t instruction_id;
} XrVmCodeDiagnostic;

typedef struct XrVmCode XrVmCode;
typedef struct XrVmExecution XrVmExecution;

XR_FUNC XrVmCodeOptions xr_vm_code_default_options(void);
XR_FUNC XrVmCodeStatus xr_vm_code_build(XrInstance *instance, const XrVmCodeOptions *options,
                                        XrVmCode **code_out, XrVmCodeDiagnostic *diagnostic_out);
XR_FUNC void xr_vm_code_free(XrVmCode *code);
XR_FUNC XrVmCode *xr_vm_code_retain(const XrVmCode *code);
XR_FUNC bool xr_vm_code_matches_instance(const XrVmCode *code, const XrInstance *instance);
XR_FUNC XrExecutionCacheKey xr_vm_code_cache_key(const XrVmCode *code);
XR_FUNC XrFingerprint xr_vm_code_private_digest(const XrVmCode *code);
XR_FUNC size_t xr_vm_code_private_size(const XrVmCode *code);
XR_FUNC XrVmDecodePolicy xr_vm_code_decode_policy(const XrVmCode *code);
XR_FUNC XrVmOutcome xr_vm_code_execute(const XrVmCode *code, XrInstance *instance,
                                       uint32_t function_id, const XrVmValue *arguments,
                                       uint32_t argument_count);
/* Aggregate values returned by one-shot execution own an executor-private
 * detached tree. Views
 * borrow that tree until the outcome is disposed. */
XR_FUNC bool xr_vm_value_aggregate_view(const XrVmValue *value, XrVmAggregateView *view_out);
XR_FUNC void xr_vm_outcome_dispose(XrVmOutcome *outcome);
XR_FUNC bool xr_vm_execution_create(const XrVmCode *code, XrInstance *instance,
                                    uint32_t function_id, const XrVmValue *arguments,
                                    uint32_t argument_count, XrVmExecution **execution_out);
XR_FUNC XrVmOutcome xr_vm_execution_step(XrVmExecution *execution);
XR_FUNC XrVmOutcome xr_vm_execution_cancel(XrVmExecution *execution);
XR_FUNC void xr_vm_execution_free(XrVmExecution *execution);
XR_FUNC const char *xr_vm_code_status_name(XrVmCodeStatus status);

#endif  // XR_PROGRAM_VM_H
