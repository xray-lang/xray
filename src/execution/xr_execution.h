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

#include "../plan/target/xr_target_profile.h"
#include "../program/xr_program_verify.h"

typedef XrFingerprint XrExecutionId;

#define XR_EXECUTION_BINDING_SCHEMA_VERSION UINT32_C(1)

typedef enum XrProviderBehaviorFlags {
    XR_PROVIDER_BEHAVIOR_THREAD_SAFE = UINT32_C(1) << 0,
    XR_PROVIDER_BEHAVIOR_REENTRANT = UINT32_C(1) << 1,
    XR_PROVIDER_BEHAVIOR_CALLBACK_SAFE = UINT32_C(1) << 2,
} XrProviderBehaviorFlags;

#define XR_PROVIDER_BEHAVIOR_FLAGS_ALL                                                             \
    (XR_PROVIDER_BEHAVIOR_THREAD_SAFE | XR_PROVIDER_BEHAVIOR_REENTRANT |                           \
     XR_PROVIDER_BEHAVIOR_CALLBACK_SAFE)

typedef void (*XrProviderOperationEntry)(void);

typedef struct XrProviderOperationBinding {
    XrStableId operation_id;
    XrProviderOperationEntry entry;
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

XR_FUNC XrExecutionStatus xr_execution_instance_create(const XrExecutionBindingInput *input,
                                                       XrInstance **instance_out,
                                                       XrExecutionDiagnostic *diagnostic_out);
XR_FUNC XrExecutionStatus xr_execution_instance_create_successor(
    const XrInstance *retired, const XrProviderBinding *providers, size_t provider_count,
    XrInstance **instance_out, XrExecutionDiagnostic *diagnostic_out);
XR_FUNC bool xr_execution_instance_pin(XrInstance *instance);
XR_FUNC void xr_execution_instance_unpin(XrInstance *instance);
XR_FUNC XrExecutionStatus xr_execution_instance_begin_drain(XrInstance *instance,
                                                            XrExecutionDiagnostic *diagnostic_out);
XR_FUNC XrExecutionStatus xr_execution_instance_retire(XrInstance *instance,
                                                       XrExecutionDiagnostic *diagnostic_out);
XR_FUNC XrExecutionStatus xr_execution_instance_free(XrInstance **instance,
                                                     XrExecutionDiagnostic *diagnostic_out);
XR_FUNC XrInstanceState xr_execution_instance_state(const XrInstance *instance);
XR_FUNC uint64_t xr_execution_instance_pin_count(const XrInstance *instance);
XR_FUNC XrExecutionId xr_execution_instance_id(const XrInstance *instance);
XR_FUNC XrExecutionCacheKey xr_execution_instance_cache_key(const XrInstance *instance);
XR_FUNC const XrValidatedProgram *xr_execution_instance_program(const XrInstance *instance);
XR_FUNC const XrTargetProfile *xr_execution_instance_profile(const XrInstance *instance);
XR_FUNC const char *xr_execution_status_name(XrExecutionStatus status);
XR_FUNC const char *xr_execution_diagnostic_kind_name(XrExecutionDiagnosticKind kind);

#endif  // XR_EXECUTION_H
