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
#include "../shared/xr_value_format_core.h"

#define XR_VM_CODE_OPTIONS_SCHEMA_VERSION UINT32_C(2)
#define XR_VM_BUILD_ID "xray-program-vm-v16"

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
    uint8_t reserved8;
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
    XR_VM_VALUE_STRING,
    XR_VM_VALUE_RUNE,
    XR_VM_VALUE_I8,
    XR_VM_VALUE_U8,
    XR_VM_VALUE_I16,
    XR_VM_VALUE_I32,
    XR_VM_VALUE_U64,
    XR_VM_VALUE_ATOMIC,
    XR_VM_VALUE_F64,
    XR_VM_VALUE_RESOURCE,
} XrVmValueKind;

/* Bounds diagnostics travel with the panic through cleanup and suspension. */
typedef struct XrVmPanicInfo {
    uint32_t code;
    bool has_bounds;
    int64_t index;
    uint64_t length;
    /* Optional immutable string owner; borrowed views share the panic lifetime. */
    const void *message;
} XrVmPanicInfo;

typedef struct XrVmValue {
    XrVmValueKind kind;
    union {
        bool boolean;
        int64_t i64;
        int8_t i8;
        uint8_t u8;
        int16_t i16;
        int32_t i32;
        uint64_t u64;
        uint64_t f64_bits;
        uint32_t u32;
        uint16_t u16;
        uint16_t target_enum;
        uint32_t error;
        XrVmPanicInfo panic_info;
        const void *aggregate;
        const void *class_reference;
        const void *existential;
        const void *callable;
        const void *string;
        const void *atomic_storage;
        const void *resource;
        uint32_t rune;
    } as;
} XrVmValue;

/* Borrowed view of a string value; bytes belong to the VM arena or to a
 * owned outcome until that owner is disposed. */
typedef struct XrVmStringView {
    const uint8_t *bytes;
    uint32_t size;
} XrVmStringView;

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
    XR_VM_OUTCOME_INITIALIZING,
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
    XR_VM_TRAP_MODULE_SLOT_UNINITIALIZED = 8,
    XR_VM_TRAP_MODULE_SLOT_ALREADY_INITIALIZED = 9,
} XrVmTrap;

typedef struct XrVmOutcome {
    XrVmOutcomeKind kind;
    bool owns_dynamic_values;
    uint8_t reserved8[3];
    /* Opaque keepalive for instance-owned failure payloads. */
    void *private_owner;
    XrVmValue value;
    XrVmValue error_value;
    /* PanicInfo may own a message; private_owner or the observing execution
     * retains that message through the full failure observation lifetime. */
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
/* Immutable code depends only on admitted Program/Profile and code options.
 * It owns a Program reference and requires no active provider instance. Every
 * execution separately admits its instance and holds that instance's lease. */
XR_FUNC XrVmCodeStatus xr_vm_code_build(const XrValidatedProgram *program,
                                        const XrTargetProfile *profile,
                                        const XrVmCodeOptions *options, XrVmCode **code_out,
                                        XrVmCodeDiagnostic *diagnostic_out);
XR_FUNC void xr_vm_code_free(XrVmCode *code);
XR_FUNC XrVmCode *xr_vm_code_retain(const XrVmCode *code);
XR_FUNC bool xr_vm_code_matches_instance(const XrVmCode *code, const XrInstance *instance);
XR_FUNC XrExecutionId xr_vm_code_execution_id(const XrVmCode *code);
XR_FUNC XrFingerprint xr_vm_code_private_digest(const XrVmCode *code);
XR_FUNC XrVmOutcome xr_vm_code_execute(const XrVmCode *code, XrInstance *instance,
                                       uint32_t function_id, const XrVmValue *arguments,
                                       uint32_t argument_count);
/* One-shot dynamic results retain their value graph, Code and instance lease.
 * Views borrow that owner until outcome disposal. A sticky initializer error
 * retains its original identity and prevents instance retirement until disposed. */
XR_FUNC bool xr_vm_value_aggregate_view(const XrVmValue *value, XrVmAggregateView *view_out);
XR_FUNC bool xr_vm_value_string_view(const XrVmValue *value, XrVmStringView *view_out);
XR_FUNC bool xr_vm_panic_message_view(const XrVmPanicInfo *panic, XrVmStringView *view_out);
/* The reader borrows code metadata; nodes borrow values from their result owner. */
XR_FUNC XrValueFormatReader xr_vm_value_format_reader(const XrVmCode *code);
XR_FUNC void xr_vm_outcome_dispose(XrVmOutcome *outcome);
/* Ordinary and resumable entries use the same frame dispatcher. Step returns
 * at a suspension or terminal outcome; returned views borrow frame storage
 * until free. Cancellation remains valid only at a verified suspension. */
XR_FUNC bool xr_vm_execution_create(const XrVmCode *code, XrInstance *instance,
                                    uint32_t function_id, const XrVmValue *arguments,
                                    uint32_t argument_count, XrVmExecution **execution_out);
/* Install before the first step. The borrowed context outlives this execution.
 * The callback is polled before each operation in the complete call tree,
 * including module initialization. A true result terminates through the same
 * ownership cleanup as resource exhaustion. Code and instances do not retain it. */
XR_FUNC bool xr_vm_execution_set_interrupt(XrVmExecution *execution, void *context,
                                          bool (*requested)(void *context));
XR_FUNC XrVmOutcome xr_vm_execution_step(XrVmExecution *execution);
XR_FUNC XrVmOutcome xr_vm_execution_cancel(XrVmExecution *execution);
XR_FUNC void xr_vm_execution_free(XrVmExecution *execution);
XR_FUNC const char *xr_vm_code_status_name(XrVmCodeStatus status);

#endif  // XR_PROGRAM_VM_H
