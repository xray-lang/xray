/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_execution.c - Exact program/profile/provider execution binding
 */

#include "xr_execution.h"

#include "../base/xmalloc.h"
#include "../base/xsha256.h"

#include <stdatomic.h>
#include <string.h>

typedef struct XrBoundOperation {
    XrStableId operation_id;
    XrProviderOperationEntry entry;
    void *context;
} XrBoundOperation;

typedef struct XrBoundProvider {
    XrStableId contract_id;
    XrFingerprint contract_fingerprint;
    uint32_t behavior_flags;
    XrBoundOperation *operations;
    uint16_t operation_count;
} XrBoundProvider;

struct XrInstance {
    XrValidatedProgram *program;
    XrTargetProfile *profile;
    XrExecutionId execution_id;
    uint64_t generation;
    XrBoundProvider *providers;
    size_t provider_count;
    atomic_uint_least32_t state;
    atomic_uint_least64_t pins;
};

static void clear_diagnostic(XrExecutionDiagnostic *diagnostic) {
    if (diagnostic)
        memset(diagnostic, 0, sizeof(*diagnostic));
}

static XrExecutionStatus reject(XrExecutionDiagnostic *diagnostic, XrExecutionDiagnosticKind kind,
                                uint32_t provider, uint32_t operation, XrStableId contract_id,
                                XrStableId operation_id, XrExecutionStatus status) {
    if (diagnostic) {
        diagnostic->kind = kind;
        diagnostic->provider_index = provider;
        diagnostic->operation_index = operation;
        diagnostic->contract_id = contract_id;
        diagnostic->operation_id = operation_id;
    }
    return status;
}

static bool stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static uint32_t required_provider_behavior(const XrTargetProfile *profile,
                                           const XrTargetProviderContract *contract) {
    uint32_t required = XR_PROVIDER_BEHAVIOR_REENTRANT;
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    if (machine && machine->runtime_profile == XR_TARGET_RUNTIME_PROFILE_HOSTED)
        required |= XR_PROVIDER_BEHAVIOR_THREAD_SAFE;
    for (uint16_t index = 0; index < contract->operation_count; ++index) {
        if ((contract->operations[index].lifetime_flags & XR_TARGET_PROVIDER_LIFETIME_CALLBACK) !=
            0u)
            required |= XR_PROVIDER_BEHAVIOR_CALLBACK_SAFE;
    }
    return required;
}

static XrExecutionStatus validate_bindings(const XrExecutionBindingInput *input,
                                           XrExecutionDiagnostic *diagnostic) {
    size_t expected_count = xr_target_profile_provider_count(input->profile);
    if (expected_count == 0 || input->provider_count != expected_count || !input->providers)
        return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_COUNT, 0, 0, (XrStableId) {{0}},
                      (XrStableId) {{0}}, XR_EXECUTION_PROVIDER_REJECTED);
    for (size_t provider = 0; provider < expected_count; ++provider) {
        const XrTargetProviderContract *expected =
            xr_target_profile_provider(input->profile, provider);
        const XrProviderBinding *actual = &input->providers[provider];
        XrFingerprint expected_fingerprint = {{0}};
        if (!expected ||
            xr_target_provider_contract_fingerprint(expected, &expected_fingerprint) !=
                XR_RUNTIME_ABI_OK ||
            !stable_id_equal(expected->contract_id, actual->contract_id) ||
            !xr_fingerprint_equal(expected_fingerprint, actual->contract_fingerprint))
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT,
                          (uint32_t) provider, 0,
                          expected ? expected->contract_id : (XrStableId) {{0}}, (XrStableId) {{0}},
                          XR_EXECUTION_PROVIDER_REJECTED);
        if (actual->reserved16 != 0 ||
            (actual->behavior_flags & ~XR_PROVIDER_BEHAVIOR_FLAGS_ALL) != 0u ||
            (actual->behavior_flags & required_provider_behavior(input->profile, expected)) !=
                required_provider_behavior(input->profile, expected))
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR,
                          (uint32_t) provider, 0, expected->contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROVIDER_REJECTED);
        if (actual->operation_count != expected->operation_count || !actual->operations)
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION,
                          (uint32_t) provider, 0, expected->contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROVIDER_REJECTED);
        for (uint16_t operation = 0; operation < expected->operation_count; ++operation) {
            const XrTargetProviderOperationContract *expected_operation =
                &expected->operations[operation];
            const XrProviderOperationBinding *actual_operation = &actual->operations[operation];
            if (!stable_id_equal(expected_operation->stable_id, actual_operation->operation_id) ||
                !actual_operation->entry)
                return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION,
                              (uint32_t) provider, operation, expected->contract_id,
                              expected_operation->stable_id, XR_EXECUTION_PROVIDER_REJECTED);
        }
    }
    return XR_EXECUTION_OK;
}

static void hash_identity(XrSHA256Context *context, XrFingerprint identity) {
    xr_sha256_update(context, identity.bytes, sizeof(identity.bytes));
}

static XrExecutionId compute_execution_id(const XrValidatedProgram *program,
                                          const XrTargetProfile *profile) {
    static const uint8_t domain[] = "xray-execution-id-v1\0";
    XrSHA256Context context;
    XrProgramId program_id = xr_validated_program_id(program);
    XrFingerprint profile_id = xr_target_profile_fingerprint(profile);
    const XrBoundaryAbi *boundary = xr_target_profile_boundary_abi(profile);
    const XrRuntimeKernelContract *kernel = xr_target_profile_runtime_kernel(profile);
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    xr_sha256_update(&context, program_id.bytes, sizeof(program_id.bytes));
    hash_identity(&context, profile_id);
    hash_identity(&context, boundary->id);
    hash_identity(&context, kernel->id);
    XrExecutionId id;
    xr_sha256_final(&context, id.bytes);
    return id;
}

static void destroy_instance(XrInstance *instance) {
    if (!instance)
        return;
    for (size_t provider = 0; provider < instance->provider_count; ++provider)
        xr_free(instance->providers[provider].operations);
    xr_free(instance->providers);
    xr_target_profile_free(instance->profile);
    xr_validated_program_free(instance->program);
    xr_free(instance);
}

static bool copy_bindings(XrInstance *instance, const XrProviderBinding *bindings,
                          size_t binding_count) {
    instance->providers = xr_calloc(binding_count, sizeof(XrBoundProvider));
    if (!instance->providers)
        return false;
    instance->provider_count = binding_count;
    for (size_t provider = 0; provider < binding_count; ++provider) {
        XrBoundProvider *destination = &instance->providers[provider];
        const XrProviderBinding *source = &bindings[provider];
        destination->contract_id = source->contract_id;
        destination->contract_fingerprint = source->contract_fingerprint;
        destination->behavior_flags = source->behavior_flags;
        destination->operation_count = source->operation_count;
        destination->operations = xr_calloc(source->operation_count, sizeof(XrBoundOperation));
        if (!destination->operations)
            return false;
        for (uint16_t operation = 0; operation < source->operation_count; ++operation) {
            destination->operations[operation].operation_id =
                source->operations[operation].operation_id;
            destination->operations[operation].entry = source->operations[operation].entry;
            destination->operations[operation].context = source->operations[operation].context;
        }
    }
    return true;
}

XrExecutionStatus xr_execution_instance_create(const XrExecutionBindingInput *input,
                                               XrInstance **instance_out,
                                               XrExecutionDiagnostic *diagnostic_out) {
    if (instance_out)
        *instance_out = NULL;
    clear_diagnostic(diagnostic_out);
    if (!input || !instance_out || input->schema_version != XR_EXECUTION_BINDING_SCHEMA_VERSION ||
        input->reserved32 != 0 || !input->program || !input->profile || input->generation == 0)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    if (!xr_target_profile_verify(input->profile, NULL, 0) ||
        !xr_target_profile_boundary_abi(input->profile) ||
        !xr_target_profile_runtime_kernel(input->profile))
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_PROFILE, 0, 0, (XrStableId) {{0}},
                      (XrStableId) {{0}}, XR_EXECUTION_PROFILE_REJECTED);
    XrExecutionStatus status = validate_bindings(input, diagnostic_out);
    if (status != XR_EXECUTION_OK)
        return status;

    XrInstance *instance = xr_calloc(1u, sizeof(XrInstance));
    if (!instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_OUT_OF_MEMORY, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_OUT_OF_MEMORY);
    instance->program = xr_validated_program_retain(input->program);
    instance->profile = xr_target_profile_retain(input->profile);
    instance->generation = input->generation;
    instance->execution_id = compute_execution_id(input->program, input->profile);
    atomic_init(&instance->state, XR_INSTANCE_ACTIVE);
    atomic_init(&instance->pins, 0u);
    if (!copy_bindings(instance, input->providers, input->provider_count)) {
        destroy_instance(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_OUT_OF_MEMORY, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_OUT_OF_MEMORY);
    }
    *instance_out = instance;
    return XR_EXECUTION_OK;
}

XrExecutionStatus xr_execution_instance_create_successor(const XrInstance *retired,
                                                         const XrProviderBinding *providers,
                                                         size_t provider_count,
                                                         XrInstance **instance_out,
                                                         XrExecutionDiagnostic *diagnostic_out) {
    if (instance_out)
        *instance_out = NULL;
    clear_diagnostic(diagnostic_out);
    if (!retired || xr_execution_instance_state(retired) != XR_INSTANCE_RETIRED ||
        retired->generation == UINT64_MAX || !instance_out)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    XrExecutionBindingInput input = {
        .schema_version = XR_EXECUTION_BINDING_SCHEMA_VERSION,
        .program = retired->program,
        .profile = retired->profile,
        .providers = providers,
        .provider_count = provider_count,
        .generation = retired->generation + 1u,
    };
    return xr_execution_instance_create(&input, instance_out, diagnostic_out);
}

bool xr_execution_instance_pin(XrInstance *instance) {
    if (!instance)
        return false;
    if (atomic_load_explicit(&instance->state, memory_order_acquire) != XR_INSTANCE_ACTIVE)
        return false;
    atomic_fetch_add_explicit(&instance->pins, 1u, memory_order_acq_rel);
    if (atomic_load_explicit(&instance->state, memory_order_acquire) == XR_INSTANCE_ACTIVE)
        return true;
    atomic_fetch_sub_explicit(&instance->pins, 1u, memory_order_acq_rel);
    return false;
}

void xr_execution_instance_unpin(XrInstance *instance) {
    if (!instance)
        return;
    uint64_t pins = atomic_load_explicit(&instance->pins, memory_order_acquire);
    while (pins != 0u &&
           !atomic_compare_exchange_weak_explicit(&instance->pins, &pins, pins - 1u,
                                                  memory_order_acq_rel, memory_order_acquire)) {
    }
}

XrExecutionStatus xr_execution_instance_begin_drain(XrInstance *instance,
                                                    XrExecutionDiagnostic *diagnostic_out) {
    clear_diagnostic(diagnostic_out);
    if (!instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    uint32_t expected = XR_INSTANCE_ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(&instance->state, &expected, XR_INSTANCE_DRAINING,
                                                 memory_order_acq_rel, memory_order_acquire))
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    return XR_EXECUTION_OK;
}

XrExecutionStatus xr_execution_instance_retire(XrInstance *instance,
                                               XrExecutionDiagnostic *diagnostic_out) {
    clear_diagnostic(diagnostic_out);
    if (!instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    if (atomic_load_explicit(&instance->pins, memory_order_acquire) != 0u)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    uint32_t expected = XR_INSTANCE_DRAINING;
    if (!atomic_compare_exchange_strong_explicit(&instance->state, &expected, XR_INSTANCE_RETIRED,
                                                 memory_order_acq_rel, memory_order_acquire))
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    return XR_EXECUTION_OK;
}

XrExecutionStatus xr_execution_instance_free(XrInstance **instance,
                                             XrExecutionDiagnostic *diagnostic_out) {
    clear_diagnostic(diagnostic_out);
    if (!instance || !*instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    if (xr_execution_instance_state(*instance) != XR_INSTANCE_RETIRED ||
        xr_execution_instance_pin_count(*instance) != 0u)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    destroy_instance(*instance);
    *instance = NULL;
    return XR_EXECUTION_OK;
}

XrInstanceState xr_execution_instance_state(const XrInstance *instance) {
    return instance ? (XrInstanceState) atomic_load_explicit(&instance->state, memory_order_acquire)
                    : 0;
}

uint64_t xr_execution_instance_pin_count(const XrInstance *instance) {
    return instance ? atomic_load_explicit(&instance->pins, memory_order_acquire) : 0u;
}

XrExecutionId xr_execution_instance_id(const XrInstance *instance) {
    XrExecutionId zero = {{0}};
    return instance ? instance->execution_id : zero;
}

XrExecutionCacheKey xr_execution_instance_cache_key(const XrInstance *instance) {
    XrExecutionCacheKey key = {0};
    if (instance) {
        key.execution_id = instance->execution_id;
        key.generation = instance->generation;
    }
    return key;
}

const XrValidatedProgram *xr_execution_instance_program(const XrInstance *instance) {
    return instance ? instance->program : NULL;
}

const XrTargetProfile *xr_execution_instance_profile(const XrInstance *instance) {
    return instance ? instance->profile : NULL;
}

const char *xr_execution_status_name(XrExecutionStatus status) {
    switch (status) {
        case XR_EXECUTION_OK:
            return "ok";
        case XR_EXECUTION_INVALID_INPUT:
            return "invalid-input";
        case XR_EXECUTION_PROFILE_REJECTED:
            return "profile-rejected";
        case XR_EXECUTION_PROVIDER_REJECTED:
            return "provider-rejected";
        case XR_EXECUTION_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_EXECUTION_GENERATION_REJECTED:
            return "generation-rejected";
        default:
            return "unknown";
    }
}

const char *xr_execution_diagnostic_kind_name(XrExecutionDiagnosticKind kind) {
    switch (kind) {
        case XR_EXECUTION_DIAGNOSTIC_NONE:
            return "none";
        case XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT:
            return "invalid-input";
        case XR_EXECUTION_DIAGNOSTIC_PROFILE:
            return "profile";
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_COUNT:
            return "provider-count";
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT:
            return "provider-contract";
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION:
            return "provider-operation";
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR:
            return "provider-behavior";
        case XR_EXECUTION_DIAGNOSTIC_OUT_OF_MEMORY:
            return "out-of-memory";
        case XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE:
            return "generation-state";
        case XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY:
            return "generation-busy";
        default:
            return "unknown";
    }
}
