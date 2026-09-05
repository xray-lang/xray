/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_execution.h - Exact program/profile/provider execution binding
 *
 * KEY CONCEPT:
 *   XrInstance is the generation-scoped execution authority. It binds one
 *   validated program to one target profile and an exact provider set before
 *   either VM or AOT execution can begin.
 */

#ifndef XR_EXECUTION_H
#define XR_EXECUTION_H

#include "xr_execution_identity.h"

#define XR_EXECUTION_BINDING_SCHEMA_VERSION UINT32_C(3)

typedef enum XrProviderBehaviorFlags {
    XR_PROVIDER_BEHAVIOR_THREAD_SAFE = UINT32_C(1) << 0,
    XR_PROVIDER_BEHAVIOR_REENTRANT = UINT32_C(1) << 1,
    XR_PROVIDER_BEHAVIOR_CALLBACK_SAFE = UINT32_C(1) << 2,
} XrProviderBehaviorFlags;

#define XR_PROVIDER_BEHAVIOR_FLAGS_ALL                                                             \
    (XR_PROVIDER_BEHAVIOR_THREAD_SAFE | XR_PROVIDER_BEHAVIOR_REENTRANT |                           \
     XR_PROVIDER_BEHAVIOR_CALLBACK_SAFE)

typedef enum XrProviderCallStatus {
    XR_PROVIDER_CALL_OK = 0,
    XR_PROVIDER_CALL_FAILED,
} XrProviderCallStatus;

typedef enum XrProviderTrampolineKind {
    XR_PROVIDER_TRAMPOLINE_INVALID = 0,
    XR_PROVIDER_TRAMPOLINE_I64_TO_I64,
    XR_PROVIDER_TRAMPOLINE_OUTPUT_WRITE,
} XrProviderTrampolineKind;

/* Every admitted provider entry is an execution-owned typed trampoline.  The
 * provider-specific C ABI remains behind this boundary and cannot escape into
 * VM or AOT code. */
typedef XrProviderCallStatus (*XrProviderI64OperationEntry)(void *context, int64_t argument,
                                                           int64_t *result_out);
typedef XrProviderCallStatus (*XrProviderOutputWriteEntry)(void *context, const uint8_t *bytes,
                                                          size_t size);

typedef struct XrProviderOperationBinding {
    XrStableId operation_id;
    XrProviderTrampolineKind trampoline_kind;
    union {
        XrProviderI64OperationEntry i64_to_i64;
        XrProviderOutputWriteEntry output_write;
    } entry;
    void *context;
} XrProviderOperationBinding;

typedef struct XrProviderBinding {
    XrStableId contract_id;
    XrFingerprint contract_fingerprint;
    uint32_t behavior_flags;
    const XrProviderOperationBinding *operations;
    uint16_t operation_count;
    uint16_t reserved16;
} XrProviderBinding;

typedef struct XrExecutionBindingInput {
    uint32_t schema_version;
    uint32_t reserved32;
    XrValidatedProgram *program;
    XrTargetProfile *profile;
    const XrProviderBinding *providers;
    size_t provider_count;
    uint64_t generation;
} XrExecutionBindingInput;

typedef enum XrExecutionDiagnosticKind {
    XR_EXECUTION_DIAGNOSTIC_NONE = 0,
    XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT,
    XR_EXECUTION_DIAGNOSTIC_PROFILE,
    XR_EXECUTION_DIAGNOSTIC_PROVIDER_COUNT,
    XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT,
    XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION,
    XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR,
    XR_EXECUTION_DIAGNOSTIC_PROVIDER_ABI,
    XR_EXECUTION_DIAGNOSTIC_OUT_OF_MEMORY,
    XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE,
    XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY,
} XrExecutionDiagnosticKind;

typedef struct XrExecutionDiagnostic {
    XrExecutionDiagnosticKind kind;
    uint32_t provider_index;
    uint32_t operation_index;
    XrStableId contract_id;
    XrStableId operation_id;
} XrExecutionDiagnostic;

typedef enum XrExecutionStatus {
    XR_EXECUTION_OK = 0,
    XR_EXECUTION_INVALID_INPUT,
    XR_EXECUTION_PROFILE_REJECTED,
    XR_EXECUTION_PROVIDER_REJECTED,
    XR_EXECUTION_OUT_OF_MEMORY,
    XR_EXECUTION_GENERATION_REJECTED,
} XrExecutionStatus;

typedef enum XrInstanceState {
    XR_INSTANCE_ACTIVE = 1,
    XR_INSTANCE_DRAINING,
    XR_INSTANCE_RETIRED,
} XrInstanceState;

typedef struct XrExecutionCacheKey {
    XrExecutionId execution_id;
    uint64_t generation;
} XrExecutionCacheKey;

typedef struct XrInstance XrInstance;

/* A lease is the only authority for using generation-bound state after
 * admission. Its ticket is registered by the instance and is consumed
 * exactly once by release. Copies name the same ticket: only the first
 * release succeeds, and every later use fails closed. The caller must keep
 * the XrInstance object alive until acquire returns; a successful acquire
 * keeps it alive until that ticket is consumed. */
typedef struct XrExecutionLease {
    XrInstance *instance;
    uint64_t ticket;
} XrExecutionLease;

typedef enum XrExecutionProviderCallResult {
    XR_EXECUTION_PROVIDER_CALL_OK = 0,
    XR_EXECUTION_PROVIDER_CALL_FAILED,
    XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE,
    XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE,
} XrExecutionProviderCallResult;

XR_FUNC XrExecutionStatus xr_execution_instance_create(const XrExecutionBindingInput *input,
                                                       XrInstance **instance_out,
                                                       XrExecutionDiagnostic *diagnostic_out);
XR_FUNC XrExecutionStatus xr_execution_instance_create_successor(
    const XrInstance *retired, const XrProviderBinding *providers, size_t provider_count,
    XrInstance **instance_out, XrExecutionDiagnostic *diagnostic_out);
XR_FUNC bool xr_execution_instance_acquire(XrInstance *instance, XrExecutionLease *lease_out);
XR_FUNC bool xr_execution_lease_release(XrExecutionLease *lease);
XR_FUNC bool xr_execution_lease_is_valid(const XrExecutionLease *lease);
XR_FUNC XrValidatedProgram *xr_execution_lease_retain_program(const XrExecutionLease *lease);
XR_FUNC XrTargetProfile *xr_execution_lease_retain_profile(const XrExecutionLease *lease);
XR_FUNC XrExecutionProviderCallResult xr_execution_lease_provider_call_i64(
    const XrExecutionLease *lease, uint32_t requirement_index, uint32_t operation_index,
    int64_t argument, int64_t *result_out);
XR_FUNC XrExecutionProviderCallResult xr_execution_lease_provider_output_write(
    const XrExecutionLease *lease, uint32_t requirement_index, uint32_t operation_index,
    const uint8_t *bytes, size_t size);
XR_FUNC XrExecutionStatus xr_execution_instance_begin_drain(XrInstance *instance,
                                                            XrExecutionDiagnostic *diagnostic_out);
XR_FUNC XrExecutionStatus xr_execution_instance_retire(XrInstance *instance,
                                                       XrExecutionDiagnostic *diagnostic_out);
XR_FUNC XrExecutionStatus xr_execution_instance_free(XrInstance **instance,
                                                     XrExecutionDiagnostic *diagnostic_out);
XR_FUNC XrInstanceState xr_execution_instance_state(const XrInstance *instance);
XR_FUNC uint64_t xr_execution_instance_lease_count(const XrInstance *instance);
XR_FUNC uint64_t xr_execution_instance_generation(const XrInstance *instance);
XR_FUNC XrExecutionId xr_execution_instance_id(const XrInstance *instance);
XR_FUNC XrExecutionCacheKey xr_execution_instance_cache_key(const XrInstance *instance);
XR_FUNC const char *xr_execution_status_name(XrExecutionStatus status);
XR_FUNC const char *xr_execution_diagnostic_kind_name(XrExecutionDiagnosticKind kind);

#endif  // XR_EXECUTION_H
