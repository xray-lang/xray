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

#include "../base/xchecks.h"
#include "../base/xmalloc.h"

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

typedef struct XrExecutionLeaseTicket {
    uint64_t id;
    uint32_t in_flight_calls;
} XrExecutionLeaseTicket;

struct XrInstance {
    XrValidatedProgram *program;
    XrTargetProfile *profile;
    XrExecutionId execution_id;
    uint64_t generation;
    XrBoundProvider *providers;
    size_t provider_count;
    atomic_uint_least32_t state;
    atomic_uint_least64_t leases;
    atomic_bool lease_lock;
    XrExecutionLeaseTicket *lease_tickets;
    size_t lease_ticket_capacity;
    uint64_t next_lease_ticket;
};

static void lease_lock(XrInstance *instance) {
    bool expected = false;
    while (!atomic_compare_exchange_weak_explicit(&instance->lease_lock, &expected, true,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
        expected = false;
    }
}

static void lease_unlock(XrInstance *instance) {
    atomic_store_explicit(&instance->lease_lock, false, memory_order_release);
}

static XrExecutionLeaseTicket *find_lease_ticket_locked(XrInstance *instance, uint64_t ticket) {
    if (ticket == 0u)
        return NULL;
    for (size_t index = 0; index < instance->lease_ticket_capacity; ++index) {
        if (instance->lease_tickets[index].id == ticket)
            return &instance->lease_tickets[index];
    }
    return NULL;
}

static bool lease_ticket_is_active_locked(XrInstance *instance, uint64_t ticket) {
    return find_lease_ticket_locked(instance, ticket) != NULL;
}

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

static const XrTargetProviderOperationContract *find_profile_operation(
    const XrTargetProviderContract *contract, XrStableId operation_id) {
    for (uint16_t index = 0; contract && index < contract->operation_count; ++index) {
        if (stable_id_equal(contract->operations[index].stable_id, operation_id))
            return &contract->operations[index];
    }
    return NULL;
}

static const XrTargetProviderContract *find_profile_provider(const XrTargetProfile *profile,
                                                             XrStableId contract_id) {
    size_t count = xr_target_profile_provider_count(profile);
    for (size_t index = 0; index < count; ++index) {
        const XrTargetProviderContract *contract = xr_target_profile_provider(profile, index);
        if (contract && stable_id_equal(contract->contract_id, contract_id))
            return contract;
    }
    return NULL;
}

static uint32_t required_provider_behavior(
    const XrTargetProfile *profile, const XrTargetProviderContract *contract,
    const XrProgramProviderRequirementView *requirement) {
    uint32_t required = XR_PROVIDER_BEHAVIOR_REENTRANT;
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    if (machine && machine->runtime_profile == XR_TARGET_RUNTIME_PROFILE_HOSTED)
        required |= XR_PROVIDER_BEHAVIOR_THREAD_SAFE;
    for (uint32_t index = 0; requirement && index < requirement->operation_count; ++index) {
        const XrTargetProviderOperationContract *operation =
            find_profile_operation(contract, requirement->operation_ids[index]);
        if (operation &&
            (operation->lifetime_flags & XR_TARGET_PROVIDER_LIFETIME_CALLBACK) != 0u)
            required |= XR_PROVIDER_BEHAVIOR_CALLBACK_SAFE;
    }
    return required;
}

static bool provider_slot_is_scalar_i64(const XrTargetProviderCallSlotAbi *slot) {
    return slot && slot->value_kind == XR_TARGET_PROVIDER_CALL_VALUE_SIGNED_INTEGER &&
           slot->width == 8u && slot->alignment == 8u &&
           slot->ownership == XR_TARGET_PROVIDER_CALL_OWNERSHIP_NONE && slot->flags == 0u;
}

static bool provider_operation_uses_scalar_i64_trampoline(
    const XrTargetProviderOperationContract *operation) {
    const XrTargetProviderCallAbiContract *abi = operation ? &operation->call_abi : NULL;
    return abi && abi->schema_version == XR_RUNTIME_ABI_SCHEMA_VERSION &&
           abi->calling_convention == XR_TARGET_PROVIDER_CALLING_CONVENTION_C &&
           abi->variadic == 0u && abi->parameter_count == 1u &&
           provider_slot_is_scalar_i64(&abi->parameters[0]) &&
           provider_slot_is_scalar_i64(&abi->result) && operation->lifetime_flags == 0u &&
           operation->failure_flags == 0u;
}

static XrExecutionStatus validate_bindings(const XrExecutionBindingInput *input,
                                           XrExecutionDiagnostic *diagnostic) {
    uint32_t expected_count = xr_validated_program_provider_requirement_count(input->program);
    if (input->provider_count != expected_count ||
        (expected_count == 0u ? input->providers != NULL : input->providers == NULL))
        return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_COUNT, 0, 0, (XrStableId) {{0}},
                      (XrStableId) {{0}}, XR_EXECUTION_PROVIDER_REJECTED);
    for (uint32_t provider = 0; provider < expected_count; ++provider) {
        XrProgramProviderRequirementView requirement = {0};
        if (!xr_validated_program_provider_requirement(input->program, provider, &requirement))
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, provider, 0,
                          (XrStableId) {{0}}, (XrStableId) {{0}},
                          XR_EXECUTION_INVALID_INPUT);
        const XrTargetProviderContract *expected =
            find_profile_provider(input->profile, requirement.contract_id);
        if (!expected)
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROFILE, provider, 0,
                          requirement.contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROFILE_REJECTED);
        const XrProviderBinding *actual = &input->providers[provider];
        XrFingerprint expected_fingerprint = {{0}};
        if (xr_target_provider_contract_fingerprint(expected, &expected_fingerprint) !=
                XR_RUNTIME_ABI_OK ||
            !stable_id_equal(requirement.contract_id, actual->contract_id) ||
            !xr_fingerprint_equal(expected_fingerprint, actual->contract_fingerprint))
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_CONTRACT,
                          provider, 0, requirement.contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROVIDER_REJECTED);
        uint32_t required_behavior =
            required_provider_behavior(input->profile, expected, &requirement);
        if (actual->reserved16 != 0 ||
            (actual->behavior_flags & ~XR_PROVIDER_BEHAVIOR_FLAGS_ALL) != 0u ||
            (actual->behavior_flags & required_behavior) != required_behavior)
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_BEHAVIOR,
                          provider, 0, expected->contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROVIDER_REJECTED);
        if (actual->operation_count != requirement.operation_count || !actual->operations)
            return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION,
                          provider, 0, expected->contract_id, (XrStableId) {{0}},
                          XR_EXECUTION_PROVIDER_REJECTED);
        for (uint16_t operation = 0; operation < actual->operation_count; ++operation) {
            const XrTargetProviderOperationContract *expected_operation =
                find_profile_operation(expected, requirement.operation_ids[operation]);
            const XrProviderOperationBinding *actual_operation = &actual->operations[operation];
            if (!expected_operation)
                return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROFILE, provider, operation,
                              expected->contract_id, requirement.operation_ids[operation],
                              XR_EXECUTION_PROFILE_REJECTED);
            if (!provider_operation_uses_scalar_i64_trampoline(expected_operation))
                return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_ABI, provider,
                              operation, expected->contract_id, expected_operation->stable_id,
                              XR_EXECUTION_PROVIDER_REJECTED);
            if (!stable_id_equal(requirement.operation_ids[operation],
                                 actual_operation->operation_id) ||
                !actual_operation->entry)
                return reject(diagnostic, XR_EXECUTION_DIAGNOSTIC_PROVIDER_OPERATION,
                              provider, operation, expected->contract_id,
                              expected_operation->stable_id, XR_EXECUTION_PROVIDER_REJECTED);
        }
    }
    return XR_EXECUTION_OK;
}

static void destroy_instance(XrInstance *instance) {
    if (!instance)
        return;
    for (size_t provider = 0; provider < instance->provider_count; ++provider)
        xr_free(instance->providers[provider].operations);
    xr_free(instance->lease_tickets);
    xr_free(instance->providers);
    xr_target_profile_free(instance->profile);
    xr_validated_program_free(instance->program);
    xr_free(instance);
}

static bool copy_bindings(XrInstance *instance, const XrProviderBinding *bindings,
                          size_t binding_count) {
    if (binding_count == 0u) {
        instance->providers = NULL;
        instance->provider_count = 0u;
        return true;
    }
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
    if (!xr_execution_id_compute(input->program, input->profile, &instance->execution_id)) {
        destroy_instance(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_PROFILE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}},
                      XR_EXECUTION_PROFILE_REJECTED);
    }
    atomic_init(&instance->state, XR_INSTANCE_ACTIVE);
    atomic_init(&instance->leases, 0u);
    atomic_init(&instance->lease_lock, false);
    instance->next_lease_ticket = 1u;
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

bool xr_execution_instance_acquire(XrInstance *instance, XrExecutionLease *lease_out) {
    if (!instance || !lease_out || lease_out->instance || lease_out->ticket != 0u)
        return false;
    lease_lock(instance);
    if (atomic_load_explicit(&instance->state, memory_order_relaxed) != XR_INSTANCE_ACTIVE ||
        instance->next_lease_ticket == 0u) {
        lease_unlock(instance);
        return false;
    }
    size_t slot = 0u;
    while (slot < instance->lease_ticket_capacity && instance->lease_tickets[slot].id != 0u)
        ++slot;
    if (slot == instance->lease_ticket_capacity) {
        size_t old_capacity = instance->lease_ticket_capacity;
        size_t new_capacity = old_capacity < 8u ? 8u : old_capacity * 2u;
        if (new_capacity < old_capacity || new_capacity > SIZE_MAX / sizeof(XrExecutionLeaseTicket)) {
            lease_unlock(instance);
            return false;
        }
        XrExecutionLeaseTicket *grown =
            xr_realloc(instance->lease_tickets, new_capacity * sizeof(*grown));
        if (!grown) {
            lease_unlock(instance);
            return false;
        }
        memset(grown + old_capacity, 0,
               (new_capacity - old_capacity) * sizeof(*grown));
        instance->lease_tickets = grown;
        instance->lease_ticket_capacity = new_capacity;
        slot = old_capacity;
    }
    uint64_t ticket = instance->next_lease_ticket;
    instance->next_lease_ticket = ticket == UINT64_MAX ? 0u : ticket + 1u;
    instance->lease_tickets[slot].id = ticket;
    instance->lease_tickets[slot].in_flight_calls = 0u;
    atomic_fetch_add_explicit(&instance->leases, 1u, memory_order_relaxed);
    lease_out->instance = instance;
    lease_out->ticket = ticket;
    lease_unlock(instance);
    return true;
}

bool xr_execution_lease_release(XrExecutionLease *lease) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return false;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_lease_ticket_locked(instance, lease->ticket);
    if (!ticket || ticket->in_flight_calls != 0u) {
        lease_unlock(instance);
        return false;
    }
    ticket->id = 0u;
    atomic_fetch_sub_explicit(&instance->leases, 1u, memory_order_relaxed);
    lease_unlock(instance);
    lease->instance = NULL;
    lease->ticket = 0u;
    return true;
}

bool xr_execution_lease_is_valid(const XrExecutionLease *lease) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return false;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    bool valid = lease_ticket_is_active_locked(instance, lease->ticket);
    lease_unlock(instance);
    return valid;
}

XrValidatedProgram *xr_execution_lease_retain_program(const XrExecutionLease *lease) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return NULL;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    XrValidatedProgram *program =
        lease_ticket_is_active_locked(instance, lease->ticket)
            ? xr_validated_program_retain(instance->program)
            : NULL;
    lease_unlock(instance);
    return program;
}

XrTargetProfile *xr_execution_lease_retain_profile(const XrExecutionLease *lease) {
    if (!lease || !lease->instance || lease->ticket == 0u)
        return NULL;
    XrInstance *instance = lease->instance;
    lease_lock(instance);
    XrTargetProfile *profile = lease_ticket_is_active_locked(instance, lease->ticket)
                                   ? xr_target_profile_retain(instance->profile)
                                   : NULL;
    lease_unlock(instance);
    return profile;
}

XrExecutionProviderCallResult xr_execution_lease_provider_call_i64(
    const XrExecutionLease *lease, uint32_t requirement_index, uint32_t operation_index,
    int64_t argument, int64_t *result_out) {
    if (result_out)
        *result_out = 0;
    if (!lease || !lease->instance || lease->ticket == 0u)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    if (!result_out)
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    XrInstance *instance = lease->instance;
    const uint64_t lease_ticket = lease->ticket;
    lease_lock(instance);
    XrExecutionLeaseTicket *ticket = find_lease_ticket_locked(instance, lease_ticket);
    if (!ticket) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_LEASE;
    }
    XrProgramProviderRequirementView requirement = {0};
    if (requirement_index >= instance->provider_count ||
        !xr_validated_program_provider_requirement(instance->program, requirement_index,
                                                   &requirement) ||
        operation_index >= requirement.operation_count) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrBoundProvider *provider = &instance->providers[requirement_index];
    if (operation_index >= provider->operation_count ||
        !stable_id_equal(provider->contract_id, requirement.contract_id) ||
        !stable_id_equal(provider->operations[operation_index].operation_id,
                         requirement.operation_ids[operation_index]) ||
        !provider->operations[operation_index].entry || ticket->in_flight_calls == UINT32_MAX) {
        lease_unlock(instance);
        return XR_EXECUTION_PROVIDER_CALL_INVALID_REFERENCE;
    }
    XrProviderOperationEntry entry = provider->operations[operation_index].entry;
    void *entry_context = provider->operations[operation_index].context;
    ++ticket->in_flight_calls;
    lease_unlock(instance);

    int64_t provider_result = 0;
    XrProviderCallStatus status = entry(entry_context, argument, &provider_result);

    lease_lock(instance);
    ticket = find_lease_ticket_locked(instance, lease_ticket);
    XR_CHECK(ticket && ticket->in_flight_calls != 0u,
             "provider call lost its execution lease pin");
    --ticket->in_flight_calls;
    lease_unlock(instance);
    if (status != XR_PROVIDER_CALL_OK)
        return XR_EXECUTION_PROVIDER_CALL_FAILED;
    *result_out = provider_result;
    return XR_EXECUTION_PROVIDER_CALL_OK;
}

XrExecutionStatus xr_execution_instance_begin_drain(XrInstance *instance,
                                                    XrExecutionDiagnostic *diagnostic_out) {
    clear_diagnostic(diagnostic_out);
    if (!instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    lease_lock(instance);
    if (atomic_load_explicit(&instance->state, memory_order_relaxed) != XR_INSTANCE_ACTIVE) {
        lease_unlock(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    }
    atomic_store_explicit(&instance->state, XR_INSTANCE_DRAINING, memory_order_release);
    lease_unlock(instance);
    return XR_EXECUTION_OK;
}

XrExecutionStatus xr_execution_instance_retire(XrInstance *instance,
                                               XrExecutionDiagnostic *diagnostic_out) {
    clear_diagnostic(diagnostic_out);
    if (!instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    lease_lock(instance);
    if (atomic_load_explicit(&instance->leases, memory_order_relaxed) != 0u) {
        lease_unlock(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_BUSY, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    }
    if (atomic_load_explicit(&instance->state, memory_order_relaxed) != XR_INSTANCE_DRAINING) {
        lease_unlock(instance);
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_GENERATION_STATE, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_GENERATION_REJECTED);
    }
    atomic_store_explicit(&instance->state, XR_INSTANCE_RETIRED, memory_order_release);
    lease_unlock(instance);
    return XR_EXECUTION_OK;
}

XrExecutionStatus xr_execution_instance_free(XrInstance **instance,
                                             XrExecutionDiagnostic *diagnostic_out) {
    clear_diagnostic(diagnostic_out);
    if (!instance || !*instance)
        return reject(diagnostic_out, XR_EXECUTION_DIAGNOSTIC_INVALID_INPUT, 0, 0,
                      (XrStableId) {{0}}, (XrStableId) {{0}}, XR_EXECUTION_INVALID_INPUT);
    if (xr_execution_instance_state(*instance) != XR_INSTANCE_RETIRED ||
        xr_execution_instance_lease_count(*instance) != 0u)
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

uint64_t xr_execution_instance_lease_count(const XrInstance *instance) {
    return instance ? atomic_load_explicit(&instance->leases, memory_order_acquire) : 0u;
}

uint64_t xr_execution_instance_generation(const XrInstance *instance) {
    return instance ? instance->generation : 0u;
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
        case XR_EXECUTION_DIAGNOSTIC_PROVIDER_ABI:
            return "provider-abi";
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
