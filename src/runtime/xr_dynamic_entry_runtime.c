/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_dynamic_entry_runtime.c - Generation-owned dynamic entry registry/cache
 */

#include "xr_dynamic_entry_runtime.h"
#include "xr_module_generation_internal.h"
#include "../base/xmalloc.h"
#include "../plan/target/xr_target_instruction_verify.h"
#include "../plan/target/xr_target_verify.h"
#include "abi/xr_runtime_target_authority.h"
#include "../vm/xr_vm_decoded_cache.h"
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct XrRuntimeEntryRegistryRow {
    XrFingerprint semantic_fingerprint;
    XrStableId export_identity;
    XrStableId callee_identity;
    XrRuntimeEntryHandle *handle;
    atomic_uint_least64_t revision;
} XrRuntimeEntryRegistryRow;

typedef enum XrRuntimeActivationRegistrationRole {
    XR_RUNTIME_ACTIVATION_PROVIDER = 1,
    XR_RUNTIME_ACTIVATION_FINALIZER = 2,
} XrRuntimeActivationRegistrationRole;

typedef struct XrRuntimeActivationRegistration {
    XrStableId provider_contract;
    XrStableId operation;
    uint16_t provider_kind;
    uint8_t role;
    uint8_t reserved;
} XrRuntimeActivationRegistration;

struct XrRuntimeModuleActivation {
    XrRuntimeEntryRegistry *registry;
    XrRuntimeGenerationAuthority *authority;
    XrLoadedModuleGeneration *generation;
    struct XrRuntimeModuleActivation *next;
    XrRuntimeDeallocateFinalizer deallocate;
    void *deallocate_context;
    size_t allocation_size;
    size_t allocation_alignment;
    uint32_t registration_count;
    uint32_t provider_count;
    uint32_t finalizer_count;
    bool published;
    XrRuntimeActivationRegistration registrations[];
};

typedef struct XrRuntimeDynamicEntrySlot {
    xr_mutex_t gate;
    const XrRuntimeEntryRegistryRow *row;
    XrRuntimeEntryHandle *handle;
    uint64_t row_revision;
} XrRuntimeDynamicEntrySlot;

struct XrRuntimeEntryHandle {
    atomic_uint_least32_t references;
    xr_mutex_t gate;
    XrEntryCell cell;
    XrEntryCellExpectation expectation;
    XrLoadedModuleGeneration *generation;
    const XrTargetPlan *bound_plan;
    XrRuntimeGenerationAuthority *authority;
    XrFingerprint semantic_fingerprint;
    XrFingerprint plan_fingerprint;
    XrModuleGenerationIdentity generation_identity;
    XrStableId function_identity;
    uint32_t function;
    uint32_t executor_kind;
    bool configured;
    bool frozen;
};

struct XrRuntimeEntryRegistry {
    xr_mutex_t mutation_gate;
    XrRuntimeEntryRegistryRow **rows;
    uint32_t count;
    uint32_t active_count;
    uint32_t capacity;
    XrRuntimeActivationBudget activation_budget;
    XrRuntimeProviderBindings provider_bindings;
    XrRuntimeModuleActivation *activations;
    uint32_t active_modules;
    uint32_t active_provider_registrations;
    uint32_t active_finalizer_registrations;
    bool activation_configured;
    atomic_uint_least64_t mutations;
};

struct XrRuntimeDynamicEntryCache {
    XrLoadedModuleGeneration *generation;
    const XrTargetPlan *plan;
    XrFingerprint plan_fingerprint;
    XrRuntimeDynamicEntrySlot *slots;
    uint32_t slot_count;
    size_t bytes;
    atomic_uint_least64_t hits;
    atomic_uint_least64_t misses;
    atomic_uint_least64_t registry_scans;
    atomic_uint_least64_t replacements;
};

typedef enum XrRuntimeDynamicEntryLeaseState {
    XR_RUNTIME_DYNAMIC_ENTRY_LEASE_RESERVED = 0,
    XR_RUNTIME_DYNAMIC_ENTRY_LEASE_ACTIVE,
    XR_RUNTIME_DYNAMIC_ENTRY_LEASE_RETIRING,
    XR_RUNTIME_DYNAMIC_ENTRY_LEASE_PENDING,
} XrRuntimeDynamicEntryLeaseState;

struct XrVmDynamicEntryLease {
    XrEntryCallToken token;
    XrRuntimeGenerationAuthority *authority;
    XrVmDynamicEntryLease *next;
    XrRuntimeDynamicEntryLeaseState state;
};

typedef struct XrRuntimeEntryBindingSnapshot {
    XrEntryCellExpectation expectation;
    XrLoadedModuleGeneration *generation;
    const XrTargetPlan *plan;
    XrRuntimeGenerationAuthority *authority;
    XrFingerprint semantic_fingerprint;
    XrFingerprint plan_fingerprint;
    XrModuleGenerationIdentity generation_identity;
    XrStableId function_identity;
    uint32_t function;
    uint32_t executor_kind;
} XrRuntimeEntryBindingSnapshot;

typedef struct XrRuntimeEntryPublicationItem {
    XrRuntimeEntryRegistryRow *row;
    XrRuntimeEntryRegistryRow *allocated_row;
    XrRuntimeEntryHandle *handle;
    XrRuntimeEntryHandle *retained;
    bool was_frozen;
    bool restore_frozen;
} XrRuntimeEntryPublicationItem;

static bool fail(char *diagnostic, size_t diagnostic_size, const char *code,
                 const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

static XrVmDynamicEntryLease *reserve_dynamic_entry_lease(
    XrRuntimeGenerationAuthority *authority) {
    if (!authority)
        return NULL;
    XrVmDynamicEntryLease *lease =
        (XrVmDynamicEntryLease *) xr_calloc(1, sizeof(*lease));
    if (!lease)
        return NULL;
    lease->authority = authority;
    lease->state = XR_RUNTIME_DYNAMIC_ENTRY_LEASE_RESERVED;
    xr_mutex_lock(&authority->dynamic_entry_lease_gate);
    if (authority->dynamic_entry_lease_count >=
        authority->budget.max_total_pins) {
        xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
        xr_free(lease);
        return NULL;
    }
    lease->next = authority->dynamic_entry_leases;
    authority->dynamic_entry_leases = lease;
    authority->dynamic_entry_lease_count++;
    xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
    return lease;
}

static void remove_dynamic_entry_lease_locked(
    XrRuntimeGenerationAuthority *authority, XrVmDynamicEntryLease *lease) {
    XrVmDynamicEntryLease **cursor = &authority->dynamic_entry_leases;
    while (*cursor && *cursor != lease)
        cursor = &(*cursor)->next;
    if (*cursor == lease) {
        *cursor = lease->next;
        authority->dynamic_entry_lease_count--;
    }
}

static void discard_reserved_dynamic_entry_lease(
    XrVmDynamicEntryLease *lease) {
    if (!lease || !lease->authority)
        return;
    XrRuntimeGenerationAuthority *authority = lease->authority;
    bool discarded = false;
    xr_mutex_lock(&authority->dynamic_entry_lease_gate);
    if (lease->state == XR_RUNTIME_DYNAMIC_ENTRY_LEASE_RESERVED) {
        remove_dynamic_entry_lease_locked(authority, lease);
        discarded = true;
    }
    xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
    if (!discarded)
        return;
    memset(lease, 0, sizeof(*lease));
    xr_free(lease);
}

static void activate_dynamic_entry_lease(XrVmDynamicEntryLease *lease) {
    XrRuntimeGenerationAuthority *authority = lease->authority;
    xr_mutex_lock(&authority->dynamic_entry_lease_gate);
    lease->state = XR_RUNTIME_DYNAMIC_ENTRY_LEASE_ACTIVE;
    xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
}

static const uint8_t dynamic_entry_retire_poison_fingerprint
    [XR_RUNTIME_GENERATION_FINGERPRINT_SIZE] = {
        0x7d, 0x25, 0x96, 0x32, 0x74, 0x55, 0x0b, 0xce,
        0x3d, 0x72, 0x14, 0xf1, 0x9a, 0x05, 0x8c, 0xa3,
        0xe8, 0x61, 0x47, 0x29, 0xbc, 0x0f, 0xd4, 0x52,
        0x31, 0xee, 0x68, 0x9b, 0x06, 0xaf, 0xc7, 0x44,
};

static XrVmDynamicEntryStatus retire_dynamic_entry_lease(
    XrVmDynamicEntryLease *lease, XrVmDynamicEntryResolution *resolution) {
    if (!lease || !lease->authority)
        return XR_VM_DYNAMIC_ENTRY_INVALID_ARGUMENT;
    XrRuntimeGenerationAuthority *authority = lease->authority;
    xr_mutex_lock(&authority->dynamic_entry_lease_gate);
    if (lease->state != XR_RUNTIME_DYNAMIC_ENTRY_LEASE_ACTIVE) {
        bool pending = lease->state == XR_RUNTIME_DYNAMIC_ENTRY_LEASE_PENDING ||
                       lease->state == XR_RUNTIME_DYNAMIC_ENTRY_LEASE_RETIRING;
        xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
        if (resolution)
            memset(resolution, 0, sizeof(*resolution));
        return pending ? XR_VM_DYNAMIC_ENTRY_RETIRE_DEFERRED
                       : XR_VM_DYNAMIC_ENTRY_INVALID_ARGUMENT;
    }
    lease->state = XR_RUNTIME_DYNAMIC_ENTRY_LEASE_RETIRING;
    xr_mutex_unlock(&authority->dynamic_entry_lease_gate);

    XrLoadedModuleGeneration *generation = lease->token.generation;
    char diagnostic[256] = {0};
    bool released = xr_entry_call_release(&lease->token, diagnostic,
                                          sizeof(diagnostic));
    xr_mutex_lock(&authority->dynamic_entry_lease_gate);
    if (released) {
        remove_dynamic_entry_lease_locked(authority, lease);
    } else {
        lease->state = XR_RUNTIME_DYNAMIC_ENTRY_LEASE_PENDING;
        authority->pending_dynamic_entry_lease_count++;
    }
    xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
    if (resolution)
        memset(resolution, 0, sizeof(*resolution));
    if (released) {
        memset(lease, 0, sizeof(*lease));
        xr_free(lease);
        return XR_VM_DYNAMIC_ENTRY_OK;
    }
    char poison_diagnostic[256] = {0};
    xr_module_generation_poison(
        generation, dynamic_entry_retire_poison_fingerprint,
        poison_diagnostic, sizeof(poison_diagnostic));
    return XR_VM_DYNAMIC_ENTRY_RETIRE_DEFERRED;
}

static bool fingerprint_matches(XrFingerprint left, XrFingerprint right) {
    return xr_fingerprint_equal(left, right);
}

static bool generation_identity_equal(
    const XrModuleGenerationIdentity *left,
    const XrModuleGenerationIdentity *right) {
    return left && right && left->schema_version == right->schema_version &&
           left->target_plan_schema_version ==
               right->target_plan_schema_version &&
           left->generation_number == right->generation_number &&
           left->completed_family_mask == right->completed_family_mask &&
           left->required_capability_mask == right->required_capability_mask &&
           memcmp(left->semantic_fingerprint, right->semantic_fingerprint,
                  sizeof(left->semantic_fingerprint)) == 0 &&
           memcmp(left->target_profile_fingerprint,
                  right->target_profile_fingerprint,
                  sizeof(left->target_profile_fingerprint)) == 0 &&
           memcmp(left->target_plan_fingerprint,
                  right->target_plan_fingerprint,
                  sizeof(left->target_plan_fingerprint)) == 0 &&
           memcmp(left->runtime_abi_fingerprint,
                  right->runtime_abi_fingerprint,
                  sizeof(left->runtime_abi_fingerprint)) == 0 &&
           memcmp(left->provider_set_fingerprint,
                  right->provider_set_fingerprint,
                  sizeof(left->provider_set_fingerprint)) == 0 &&
           memcmp(left->object_header_fingerprint,
                  right->object_header_fingerprint,
                  sizeof(left->object_header_fingerprint)) == 0 &&
           memcmp(left->generation_fingerprint,
                  right->generation_fingerprint,
                  sizeof(left->generation_fingerprint)) == 0;
}

bool xr_runtime_entry_registry_create(XrRuntimeEntryRegistry **registry,
                                      char *diagnostic,
                                      size_t diagnostic_size) {
    if (registry)
        *registry = NULL;
    if (!registry)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "entry registry owner is missing");
    XrRuntimeEntryRegistry *created =
        (XrRuntimeEntryRegistry *) xr_calloc(1, sizeof(*created));
    if (!created)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "entry registry allocation failed");
    xr_mutex_init(&created->mutation_gate);
    atomic_init(&created->mutations, 0u);
    *registry = created;
    return true;
}

bool xr_runtime_entry_registry_destroy(XrRuntimeEntryRegistry **registry,
                                       char *diagnostic,
                                       size_t diagnostic_size) {
    if (!registry || !*registry)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "entry registry is missing");
    XrRuntimeEntryRegistry *owned = *registry;
    if (owned->active_count != 0 || owned->active_modules != 0 ||
        owned->active_provider_registrations != 0 ||
        owned->active_finalizer_registrations != 0 || owned->activations)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "activation registry still publishes module authority");
    for (uint32_t i = 0; i < owned->count; i++)
        xr_free(owned->rows[i]);
    xr_free(owned->rows);
    xr_mutex_destroy(&owned->mutation_gate);
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
    *registry = NULL;
    return true;
}

bool xr_runtime_activation_registry_configure(
    XrRuntimeGenerationAuthority *authority,
    const XrRuntimeActivationBudget *budget,
    const XrRuntimeProviderBindings *bindings, char *diagnostic,
    size_t diagnostic_size) {
    if (!authority || !authority->entry_registry || !budget || !bindings ||
        budget->max_active_entries == 0 ||
        budget->max_active_entries > XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES ||
        budget->max_active_provider_registrations == 0 ||
        budget->max_active_provider_registrations >
            XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES ||
        budget->max_active_finalizer_registrations == 0 ||
        budget->max_active_finalizer_registrations >
            XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES ||
        budget->reserved != 0)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "activation registry requires a complete bounded configuration");
    XrRuntimeEntryRegistry *registry = authority->entry_registry;
    xr_mutex_lock(&registry->mutation_gate);
    xr_mutex_lock(&authority->gate);
    bool configurable = !registry->activation_configured &&
                        registry->active_count == 0 &&
                        registry->active_modules == 0 &&
                        authority->live_generations == 0;
    if (configurable) {
        registry->activation_budget = *budget;
        registry->provider_bindings = *bindings;
        registry->activation_configured = true;
    }
    xr_mutex_unlock(&authority->gate);
    xr_mutex_unlock(&registry->mutation_gate);
    if (!configurable)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "activation registry configuration is already frozen");
    return true;
}

bool xr_runtime_entry_handle_create(XrRuntimeEntryHandle **handle,
                                    char *diagnostic,
                                    size_t diagnostic_size) {
    if (handle)
        *handle = NULL;
    if (!handle)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "entry handle owner is missing");
    XrRuntimeEntryHandle *created =
        (XrRuntimeEntryHandle *) xr_calloc(1, sizeof(*created));
    if (!created || !xr_entry_cell_init(&created->cell)) {
        xr_free(created);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "entry handle allocation failed");
    }
    xr_mutex_init(&created->gate);
    atomic_init(&created->references, 1);
    *handle = created;
    return true;
}

bool xr_runtime_entry_handle_retain(
    XrRuntimeEntryHandle *handle, XrRuntimeEntryHandle **retained,
    char *diagnostic, size_t diagnostic_size) {
    if (retained)
        *retained = NULL;
    if (!handle || !retained)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "entry handle retain is incomplete");
    uint_least32_t observed = atomic_load_explicit(
        &handle->references, memory_order_acquire);
    for (;;) {
        if (observed == 0 || observed >= UINT32_MAX - 1u)
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                        "entry handle reference budget is exhausted");
        if (atomic_compare_exchange_weak_explicit(
                &handle->references, &observed, observed + 1u,
                memory_order_acq_rel, memory_order_acquire)) {
            *retained = handle;
            return true;
        }
    }
}

bool xr_runtime_entry_handle_release(XrRuntimeEntryHandle **handle,
                                     char *diagnostic,
                                     size_t diagnostic_size) {
    if (!handle || !*handle)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "entry handle release is unmatched");
    XrRuntimeEntryHandle *owned = *handle;
    uint_least32_t observed = atomic_load_explicit(
        &owned->references, memory_order_acquire);
    for (;;) {
        if (observed == 0 || observed == UINT32_MAX)
            return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                        "entry handle release has no live ownership");
        if (observed > 1u) {
            if (!atomic_compare_exchange_weak_explicit(
                    &owned->references, &observed, observed - 1u,
                    memory_order_acq_rel, memory_order_acquire))
                continue;
            *handle = NULL;
            return true;
        }
        if (atomic_compare_exchange_weak_explicit(
                &owned->references, &observed, UINT32_MAX,
                memory_order_acq_rel, memory_order_acquire))
            break;
    }
    if (!xr_entry_cell_dispose(&owned->cell, diagnostic, diagnostic_size)) {
        atomic_store_explicit(&owned->references, 1u, memory_order_release);
        return false;
    }
    xr_mutex_destroy(&owned->gate);
    atomic_store_explicit(&owned->references, 0u, memory_order_release);
    *handle = NULL;
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
    return true;
}

XrEntryCell *xr_runtime_entry_handle_cell(XrRuntimeEntryHandle *handle) {
    return handle ? &handle->cell : NULL;
}

const XrEntryCellExpectation *xr_runtime_entry_handle_expectation(
    const XrRuntimeEntryHandle *handle) {
    return handle ? &handle->expectation : NULL;
}

bool xr_runtime_entry_handle_bind(
    XrRuntimeEntryHandle *handle,
    const XrEntryCellRegistration *registration, bool *already_bound,
    char *diagnostic, size_t diagnostic_size) {
    if (already_bound)
        *already_bound = false;
    if (!handle || !registration)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "entry handle configuration is incomplete");
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(
        registration->verified_plan);
    const XrSemanticFunctionRecord *function =
        semantic ? xr_semantic_plan_function(
                       semantic, registration->function)
                 : NULL;
    if (!registration->generation || !function ||
        registration->generation->plan != registration->verified_plan ||
        registration->executor_kind != XR_ENTRY_EXECUTOR_TYPED_VM ||
        registration->native_entry || registration->native_context)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "entry handle binding lacks exact typed generation authority");
    xr_mutex_lock(&handle->gate);
    if (handle->configured) {
        bool exact =
            handle->generation == registration->generation &&
            handle->bound_plan == registration->verified_plan &&
            handle->function == registration->function &&
            handle->executor_kind == registration->executor_kind &&
            registration->native_entry == NULL &&
            registration->native_context == NULL;
        xr_mutex_unlock(&handle->gate);
        if (!exact)
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                        "entry handle configuration conflicts with its frozen binding");
        if (already_bound)
            *already_bound = true;
        return true;
    }
    if (handle->frozen) {
        xr_mutex_unlock(&handle->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "entry handle configuration is already frozen");
    }
    bool bound = xr_entry_cell_bind(&handle->cell, registration,
                                    &handle->expectation, diagnostic,
                                    diagnostic_size);
    if (bound) {
        handle->generation = registration->generation;
        handle->bound_plan = registration->verified_plan;
        handle->authority = registration->generation->authority;
        handle->semantic_fingerprint =
            xr_semantic_plan_fingerprint(semantic);
        handle->plan_fingerprint = xr_target_plan_fingerprint(
            registration->verified_plan);
        handle->generation_identity = registration->generation->identity;
        handle->function_identity = function->id;
        handle->function = registration->function;
        handle->executor_kind = registration->executor_kind;
        handle->configured = true;
    }
    xr_mutex_unlock(&handle->gate);
    return bound;
}

static bool registry_reserve(XrRuntimeEntryRegistry *registry,
                             uint32_t required) {
    if (required <= registry->capacity)
        return true;
    uint32_t capacity = registry->capacity ? registry->capacity : 8u;
    while (capacity < required) {
        if (capacity > XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES / 2u) {
            capacity = XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < required ||
        capacity > SIZE_MAX / sizeof(*registry->rows))
        return false;
    void *rows = xr_realloc(registry->rows,
                            (size_t) capacity * sizeof(*registry->rows));
    if (!rows)
        return false;
    registry->rows = (XrRuntimeEntryRegistryRow **) rows;
    registry->capacity = capacity;
    return true;
}

static bool registry_key_equal(const XrRuntimeEntryRegistryRow *row,
                               XrFingerprint semantic_fingerprint,
                               XrStableId export_identity,
                               XrStableId callee_identity) {
    return fingerprint_matches(row->semantic_fingerprint,
                               semantic_fingerprint) &&
           xr_stable_id_equal(row->export_identity, export_identity) &&
           xr_stable_id_equal(row->callee_identity, callee_identity);
}

static XrRuntimeEntryRegistryRow *registry_find_locked(
    XrRuntimeEntryRegistry *registry, XrFingerprint semantic_fingerprint,
    XrStableId export_identity, XrStableId callee_identity) {
    for (uint32_t i = 0; i < registry->count; i++) {
        XrRuntimeEntryRegistryRow *row = registry->rows[i];
        if (registry_key_equal(row, semantic_fingerprint, export_identity,
                               callee_identity))
            return row;
    }
    return NULL;
}

static uint32_t registry_handle_row_count_locked(
    const XrRuntimeEntryRegistry *registry,
    const XrRuntimeEntryHandle *handle) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < registry->count; i++)
        count += registry->rows[i]->handle == handle;
    return count;
}

static void handle_restore_frozen(XrRuntimeEntryHandle *handle,
                                   bool frozen) {
    xr_mutex_lock(&handle->gate);
    handle->frozen = frozen;
    xr_mutex_unlock(&handle->gate);
}

static bool handle_freeze_exact(
    XrRuntimeEntryHandle *handle, XrRuntimeGenerationAuthority *authority,
    XrLoadedModuleGeneration *generation, const XrTargetPlan *plan,
    const XrSemanticFunctionRecord *function, uint32_t function_index,
    XrFingerprint semantic_fingerprint, XrFingerprint plan_fingerprint,
    bool *was_frozen) {
    if (was_frozen)
        *was_frozen = false;
    if (!handle || !authority || !generation || !plan || !function ||
        !was_frozen)
        return false;
    xr_mutex_lock(&handle->gate);
    bool exact =
        handle->configured && handle->generation == generation &&
        handle->generation->authority == authority &&
        handle->authority == authority && handle->bound_plan == plan &&
        handle->function == function_index &&
        handle->executor_kind == XR_ENTRY_EXECUTOR_TYPED_VM &&
        xr_stable_id_equal(handle->function_identity, function->id) &&
        fingerprint_matches(handle->semantic_fingerprint,
                            semantic_fingerprint) &&
        fingerprint_matches(handle->plan_fingerprint, plan_fingerprint) &&
        fingerprint_matches(handle->expectation.target_plan_fingerprint,
                            plan_fingerprint) &&
        memcmp(handle->generation_identity.target_plan_fingerprint,
               plan_fingerprint.bytes, sizeof(plan_fingerprint.bytes)) == 0 &&
        generation_identity_equal(&handle->generation_identity,
                                  &generation->identity);
    if (exact) {
        *was_frozen = handle->frozen;
        handle->frozen = true;
    }
    xr_mutex_unlock(&handle->gate);
    return exact;
}

static void publication_items_release(
    XrRuntimeEntryPublicationItem *items, uint32_t count) {
    if (!items)
        return;
    char ignored[1] = {0};
    for (uint32_t i = 0; i < count; i++) {
        if (items[i].retained)
            xr_runtime_entry_handle_release(&items[i].retained, ignored,
                                            sizeof(ignored));
        xr_free(items[i].allocated_row);
    }
    for (uint32_t i = count; i > 0; i--) {
        XrRuntimeEntryPublicationItem *item = &items[i - 1u];
        if (item->restore_frozen)
            handle_restore_frozen(item->handle, item->was_frozen);
    }
    xr_free(items);
}

static const XrTargetProviderContract *activation_provider_contract(
    const XrRuntimeTargetAuthority *runtime, uint16_t provider_kind) {
    if (!runtime)
        return NULL;
    for (size_t i = 0; i < runtime->provider_count; i++)
        if (runtime->providers[i].provider_kind == provider_kind)
            return &runtime->providers[i];
    return NULL;
}

static bool activation_operation_role(
    const XrTargetProviderOperationContract *operation,
    const XrRuntimeProviderBindings *bindings, uint8_t *role) {
    if (!operation || !bindings || !role)
        return false;
    if (operation->effect_flags == XR_TARGET_PROVIDER_EFFECT_ALLOCATES &&
        operation->lifetime_flags == XR_TARGET_PROVIDER_LIFETIME_RETURNS_OWNED &&
        operation->failure_flags == 0) {
        *role = XR_RUNTIME_ACTIVATION_PROVIDER;
        return bindings->allocate != NULL;
    }
    if (operation->effect_flags == XR_TARGET_PROVIDER_EFFECT_DEALLOCATES &&
        operation->lifetime_flags == XR_TARGET_PROVIDER_LIFETIME_CONSUMES_OWNED &&
        operation->failure_flags == 0) {
        *role = XR_RUNTIME_ACTIVATION_FINALIZER;
        return bindings->deallocate != NULL;
    }
    if (operation->effect_flags == XR_TARGET_PROVIDER_EFFECT_PANICS &&
        operation->lifetime_flags == XR_TARGET_PROVIDER_LIFETIME_BORROWS &&
        operation->failure_flags == XR_TARGET_PROVIDER_FAILURE_NO_RETURN) {
        *role = XR_RUNTIME_ACTIVATION_PROVIDER;
        return bindings->panic != NULL;
    }
    return false;
}

static bool activation_requirements_build(
    const XrTargetPlan *plan, const XrRuntimeProviderBindings *bindings,
    XrRuntimeActivationRegistration *requirements, uint32_t capacity,
    uint32_t *requirement_count, uint32_t *provider_count,
    uint32_t *finalizer_count) {
    if (requirement_count)
        *requirement_count = 0;
    if (provider_count)
        *provider_count = 0;
    if (finalizer_count)
        *finalizer_count = 0;
    if (!plan || !bindings || !requirements || !requirement_count ||
        !provider_count || !finalizer_count)
        return false;
    XrRuntimeTargetAuthority runtime;
    if (xr_runtime_target_authority_native_hosted(&runtime) !=
        XR_RUNTIME_ABI_OK)
        return false;
    uint32_t capability_count = 0;
    const XrTargetCapabilityRecord *capabilities =
        xr_target_plan_capabilities(plan, &capability_count);
    for (uint32_t i = 0; i < capability_count; i++) {
        const XrTargetCapabilityRecord *capability = &capabilities[i];
        const XrTargetProviderContract *contract =
            activation_provider_contract(&runtime, capability->provider);
        if (!contract || contract->provider_kind != capability->capability)
            return false;
        for (uint16_t operation_index = 0;
             operation_index < contract->operation_count; operation_index++) {
            if (*requirement_count >= capacity)
                return false;
            const XrTargetProviderOperationContract *operation =
                &contract->operations[operation_index];
            uint8_t role = 0;
            if (!activation_operation_role(operation, bindings, &role))
                return false;
            XrRuntimeActivationRegistration *registration =
                &requirements[*requirement_count];
            *registration = (XrRuntimeActivationRegistration) {
                .provider_contract = contract->contract_id,
                .operation = operation->stable_id,
                .provider_kind = contract->provider_kind,
                .role = role,
            };
            (*requirement_count)++;
            if (role == XR_RUNTIME_ACTIVATION_FINALIZER)
                (*finalizer_count)++;
            else
                (*provider_count)++;
        }
    }
    return *provider_count != 0 && *finalizer_count != 0;
}

static void activation_dispose_unpublished(
    XrRuntimeModuleActivation *activation) {
    if (!activation)
        return;
    XrRuntimeDeallocateFinalizer deallocate = activation->deallocate;
    void *context = activation->deallocate_context;
    size_t size = activation->allocation_size;
    size_t alignment = activation->allocation_alignment;
    memset(activation, 0, size);
    deallocate(context, activation, size, alignment);
}

bool xr_runtime_activation_publish_module(
    XrRuntimeGenerationAuthority *authority,
    XrLoadedModuleGeneration *generation, const XrTargetPlan *plan,
    XrRuntimeEntryHandle *const *function_handles,
    uint32_t function_handle_count,
    XrRuntimeModuleActivation **activation, char *diagnostic,
    size_t diagnostic_size) {
    if (activation)
        *activation = NULL;
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    char verified[512] = {0};
    size_t raw_export_count = semantic
                                  ? xr_semantic_plan_source_export_count(
                                        semantic)
                                  : 0;
    if (!activation || !authority || !authority->entry_registry ||
        !generation || !plan || generation->authority != authority ||
        generation->plan != plan || !semantic ||
        raw_export_count > XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES ||
        (raw_export_count && !function_handles) ||
        !xr_target_plan_is_verified(plan) ||
        !xr_target_plan_fingerprint_is_intact(plan) ||
        !xr_target_plan_verify(plan, verified, sizeof(verified)) ||
        !xr_target_instruction_program_verify(plan, verified,
                                              sizeof(verified)))
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "module activation publication authority is incomplete");
    uint32_t export_count = (uint32_t) raw_export_count;
    XrFingerprint semantic_fingerprint =
        xr_semantic_plan_fingerprint(semantic);
    XrFingerprint plan_fingerprint = xr_target_plan_fingerprint(plan);

    XrRuntimeEntryRegistry *registry = authority->entry_registry;
    XrRuntimeProviderBindings bindings;
    XrRuntimeActivationBudget activation_budget;
    xr_mutex_lock(&registry->mutation_gate);
    bool configured = registry->activation_configured;
    bindings = registry->provider_bindings;
    activation_budget = registry->activation_budget;
    xr_mutex_unlock(&registry->mutation_gate);
    if (!configured)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "module activation provider bindings are not configured");

    xr_mutex_lock(&authority->gate);
    bool active = generation->state == XR_MODULE_GENERATION_ACTIVE &&
                  !generation->poisoned &&
                  !generation->rollback_requested;
    xr_mutex_unlock(&authority->gate);
    if (!active)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "module activation publication requires a healthy active generation");

    enum {
        XR_RUNTIME_ACTIVATION_MAX_REGISTRATIONS =
            XR_RUNTIME_ABI_MAX_PROVIDERS *
            XR_RUNTIME_ABI_MAX_PROVIDER_OPERATIONS
    };
    XrRuntimeActivationRegistration
        requirements[XR_RUNTIME_ACTIVATION_MAX_REGISTRATIONS];
    uint32_t requirement_count = 0;
    uint32_t provider_count = 0;
    uint32_t finalizer_count = 0;
    if (!activation_requirements_build(
            plan, &bindings, requirements,
            XR_RUNTIME_ACTIVATION_MAX_REGISTRATIONS, &requirement_count,
            &provider_count, &finalizer_count))
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "module activation lacks an exact provider or finalizer binding");
    if (requirement_count >
        (SIZE_MAX - offsetof(XrRuntimeModuleActivation, registrations)) /
            sizeof(requirements[0]))
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "module activation registration size overflows");
    size_t activation_size =
        offsetof(XrRuntimeModuleActivation, registrations) +
        (size_t) requirement_count * sizeof(requirements[0]);
    size_t activation_alignment = _Alignof(XrRuntimeModuleActivation);
    void *activation_storage = bindings.allocate(
        bindings.allocate_context, activation_size, activation_alignment);
    if (!activation_storage)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "module activation provider allocation failed");
    if ((uintptr_t) activation_storage % activation_alignment != 0) {
        bindings.deallocate(bindings.deallocate_context, activation_storage,
                            activation_size, activation_alignment);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "module activation provider returned misaligned storage");
    }
    XrRuntimeModuleActivation *created =
        (XrRuntimeModuleActivation *) activation_storage;
    memset(created, 0, activation_size);
    created->registry = registry;
    created->authority = authority;
    created->generation = generation;
    created->deallocate = bindings.deallocate;
    created->deallocate_context = bindings.deallocate_context;
    created->allocation_size = activation_size;
    created->allocation_alignment = activation_alignment;
    created->registration_count = requirement_count;
    created->provider_count = provider_count;
    created->finalizer_count = finalizer_count;
    memcpy(created->registrations, requirements,
           (size_t) requirement_count * sizeof(requirements[0]));

    XrRuntimeEntryPublicationItem *items =
        export_count ? (XrRuntimeEntryPublicationItem *) xr_calloc(
                           export_count, sizeof(*items))
                     : NULL;
    if (export_count && !items) {
        activation_dispose_unpublished(created);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "module activation entry allocation failed");
    }
    for (uint32_t i = 0; i < export_count; i++) {
        const XrSemanticSourceExportRecord *export_record =
            xr_semantic_plan_source_export(semantic, i);
        const XrSemanticFunctionRecord *function =
            export_record
                ? xr_semantic_plan_function(semantic,
                                            export_record->function)
                : NULL;
        XrRuntimeEntryHandle *handle =
            export_record && export_record->function < function_handle_count
                ? function_handles[export_record->function]
                : NULL;
        items[i].handle = handle;
        if (!export_record || !function || !handle ||
            !handle_freeze_exact(handle, authority, generation, plan,
                                 function, export_record->function,
                                 semantic_fingerprint, plan_fingerprint,
                                 &items[i].was_frozen)) {
            publication_items_release(items, export_count);
            activation_dispose_unpublished(created);
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                        "module entry binding does not match its verified export");
        }
        items[i].restore_frozen = true;
        items[i].allocated_row =
            (XrRuntimeEntryRegistryRow *) xr_calloc(
                1, sizeof(*items[i].allocated_row));
        if (!items[i].allocated_row) {
            publication_items_release(items, export_count);
            activation_dispose_unpublished(created);
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                        "module entry publication allocation failed");
        }
        items[i].allocated_row->semantic_fingerprint = semantic_fingerprint;
        items[i].allocated_row->export_identity = export_record->id;
        items[i].allocated_row->callee_identity = function->id;
        atomic_init(&items[i].allocated_row->revision, 1u);
        if (!xr_runtime_entry_handle_retain(
                items[i].handle, &items[i].retained, diagnostic,
                diagnostic_size)) {
            publication_items_release(items, export_count);
            activation_dispose_unpublished(created);
            return false;
        }
    }

    xr_mutex_lock(&registry->mutation_gate);
    xr_mutex_lock(&authority->gate);
    active = generation->state == XR_MODULE_GENERATION_ACTIVE &&
             !generation->poisoned && !generation->rollback_requested &&
             generation->plan == plan && registry->activation_configured;
    for (XrRuntimeModuleActivation *cursor = registry->activations;
         active && cursor; cursor = cursor->next)
        if (cursor->generation == generation)
            active = false;
    uint32_t new_rows = 0;
    for (uint32_t i = 0; active && i < export_count; i++) {
        XrRuntimeEntryRegistryRow *candidate = items[i].allocated_row;
        items[i].row = registry_find_locked(
            registry, candidate->semantic_fingerprint,
            candidate->export_identity, candidate->callee_identity);
        if (items[i].row && items[i].row->handle)
            active = false;
        else if (!items[i].row)
            new_rows++;
    }
    bool reserved = active &&
                    registry->active_count <=
                        activation_budget.max_active_entries &&
                    export_count <= activation_budget.max_active_entries -
                                        registry->active_count &&
                    registry->active_provider_registrations <=
                        activation_budget.max_active_provider_registrations &&
                    provider_count <=
                        activation_budget.max_active_provider_registrations -
                            registry->active_provider_registrations &&
                    registry->active_finalizer_registrations <=
                        activation_budget.max_active_finalizer_registrations &&
                    finalizer_count <=
                        activation_budget.max_active_finalizer_registrations -
                            registry->active_finalizer_registrations &&
                    registry->count <= XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES &&
                    new_rows <= XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES -
                                    registry->count &&
                    registry_reserve(registry, registry->count + new_rows);
    if (reserved) {
        for (uint32_t i = 0; i < export_count; i++) {
            XrRuntimeEntryRegistryRow *row = items[i].row;
            if (!row) {
                row = items[i].allocated_row;
                items[i].allocated_row = NULL;
                registry->rows[registry->count++] = row;
            }
            row->handle = items[i].retained;
            items[i].retained = NULL;
            registry->active_count++;
            atomic_fetch_add_explicit(&row->revision, 1u,
                                      memory_order_release);
        }
        created->published = true;
        created->next = registry->activations;
        registry->activations = created;
        registry->active_modules++;
        registry->active_provider_registrations += provider_count;
        registry->active_finalizer_registrations += finalizer_count;
        atomic_fetch_add_explicit(&registry->mutations, 1u,
                                  memory_order_relaxed);
    }
    xr_mutex_unlock(&authority->gate);
    xr_mutex_unlock(&registry->mutation_gate);
    if (!reserved) {
        publication_items_release(items, export_count);
        activation_dispose_unpublished(created);
        return fail(diagnostic, diagnostic_size,
                    active ? "XR_EXEC_5003" : "XR_EXEC_5005",
                    active ? "module activation publication budget is exhausted"
                           : "module activation publication conflicts with active authority");
    }
    for (uint32_t i = 0; i < export_count; i++)
        xr_free(items[i].allocated_row);
    xr_free(items);
    *activation = created;
    return true;
}

bool xr_runtime_activation_provider_acquire(
    XrRuntimeModuleActivation *activation, uint16_t provider_kind,
    char *diagnostic, size_t diagnostic_size) {
    if (!activation || !activation->generation ||
        provider_kind <= XR_TARGET_PROVIDER_INVALID ||
        provider_kind >= XR_TARGET_PROVIDER_KIND_COUNT)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "activation provider acquisition is incomplete");
    if (!xr_module_generation_pin_acquire(
            activation->generation, XR_MODULE_GENERATION_CALLBACK,
            diagnostic, diagnostic_size))
        return false;
    XrRuntimeEntryRegistry *registry = activation->registry;
    xr_mutex_lock(&registry->mutation_gate);
    bool published = activation->published;
    bool present = false;
    for (uint32_t i = 0; published && i < activation->registration_count; i++)
        if (activation->registrations[i].role ==
                XR_RUNTIME_ACTIVATION_PROVIDER &&
            activation->registrations[i].provider_kind == provider_kind) {
            present = true;
            break;
        }
    xr_mutex_unlock(&registry->mutation_gate);
    if (present)
        return true;
    char nested[256] = {0};
    xr_module_generation_pin_release(
        activation->generation, XR_MODULE_GENERATION_CALLBACK,
        nested, sizeof(nested));
    return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                "activation does not publish the required provider");
}

bool xr_runtime_activation_provider_release(
    XrRuntimeModuleActivation *activation, char *diagnostic,
    size_t diagnostic_size) {
    if (!activation || !activation->generation)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "activation provider release is unmatched");
    return xr_module_generation_pin_release(
        activation->generation, XR_MODULE_GENERATION_CALLBACK,
        diagnostic, diagnostic_size);
}

bool xr_runtime_activation_unpublish(
    XrRuntimeModuleActivation **activation, char *diagnostic,
    size_t diagnostic_size) {
    if (!activation || !*activation)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "module activation owner is missing");
    XrRuntimeModuleActivation *owned = *activation;
    XrRuntimeEntryRegistry *registry = owned->registry;
    XrRuntimeGenerationAuthority *authority = owned->authority;
    if (!registry || !authority || !owned->generation)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "module activation identity is incomplete");
    xr_mutex_lock(&registry->mutation_gate);
    xr_mutex_lock(&authority->gate);
    XrRuntimeModuleActivation **cursor = &registry->activations;
    while (*cursor && *cursor != owned)
        cursor = &(*cursor)->next;
    bool removable = *cursor == owned && owned->published &&
                     owned->generation->state == XR_MODULE_GENERATION_RETIRED &&
                     owned->generation->total_pins == 0 &&
                     registry->active_modules != 0 &&
                     registry->active_provider_registrations >=
                         owned->provider_count &&
                     registry->active_finalizer_registrations >=
                         owned->finalizer_count;
    if (removable) {
        *cursor = owned->next;
        registry->active_modules--;
        registry->active_provider_registrations -= owned->provider_count;
        registry->active_finalizer_registrations -= owned->finalizer_count;
        owned->published = false;
        owned->next = NULL;
        atomic_fetch_add_explicit(&registry->mutations, 1u,
                                  memory_order_relaxed);
    }
    xr_mutex_unlock(&authority->gate);
    xr_mutex_unlock(&registry->mutation_gate);
    if (!removable)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "module activation is still visible or in use");
    *activation = NULL;
    activation_dispose_unpublished(owned);
    return true;
}

bool xr_runtime_entry_registry_publish(
    XrRuntimeGenerationAuthority *authority, const XrTargetPlan *plan,
    uint32_t source_export, XrRuntimeEntryHandle *handle, char *diagnostic,
    size_t diagnostic_size) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(plan);
    const XrSemanticSourceExportRecord *export_record =
        semantic ? xr_semantic_plan_source_export(semantic, source_export)
                 : NULL;
    const XrSemanticFunctionRecord *callee =
        export_record
            ? xr_semantic_plan_function(semantic, export_record->function)
            : NULL;
    char verified[512] = {0};
    if (!authority || !authority->entry_registry || !handle ||
        !export_record || !callee || !xr_target_plan_is_verified(plan) ||
        !xr_target_plan_fingerprint_is_intact(plan) ||
        !xr_target_plan_verify(plan, verified, sizeof(verified)) ||
        !xr_target_instruction_program_verify(plan, verified,
                                              sizeof(verified)))
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "entry publication authority is incomplete");
    XrFingerprint semantic_fingerprint =
        xr_semantic_plan_fingerprint(semantic);
    XrFingerprint plan_fingerprint = xr_target_plan_fingerprint(plan);
    XrRuntimeEntryBindingSnapshot binding;
    memset(&binding, 0, sizeof(binding));
    xr_mutex_lock(&handle->gate);
    bool exact_binding =
        handle->configured && handle->generation &&
        handle->generation->authority == authority &&
        handle->authority == authority && handle->bound_plan == plan &&
        handle->function == export_record->function &&
        handle->executor_kind == XR_ENTRY_EXECUTOR_TYPED_VM &&
        xr_stable_id_equal(handle->function_identity, callee->id) &&
        fingerprint_matches(handle->semantic_fingerprint,
                            semantic_fingerprint) &&
        fingerprint_matches(handle->plan_fingerprint, plan_fingerprint) &&
        fingerprint_matches(handle->expectation.target_plan_fingerprint,
                            handle->plan_fingerprint) &&
        memcmp(handle->generation_identity.target_plan_fingerprint,
               handle->plan_fingerprint.bytes,
               sizeof(handle->plan_fingerprint.bytes)) == 0 &&
        generation_identity_equal(&handle->generation_identity,
                                  &handle->generation->identity);
    bool was_frozen = handle->frozen;
    if (exact_binding) {
        binding.expectation = handle->expectation;
        binding.generation = handle->generation;
        binding.plan = handle->bound_plan;
        binding.authority = handle->authority;
        binding.semantic_fingerprint = handle->semantic_fingerprint;
        binding.plan_fingerprint = handle->plan_fingerprint;
        binding.generation_identity = handle->generation_identity;
        binding.function_identity = handle->function_identity;
        binding.function = handle->function;
        binding.executor_kind = handle->executor_kind;
        handle->frozen = true;
    }
    xr_mutex_unlock(&handle->gate);
    if (!exact_binding)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "entry publication binding does not match the export authority");
    XrRuntimeEntryHandle *retained = NULL;
    if (!xr_runtime_entry_handle_retain(
            handle, &retained, diagnostic, diagnostic_size)) {
        if (!was_frozen)
            handle_restore_frozen(handle, false);
        return false;
    }
    XrRuntimeEntryRegistry *registry = authority->entry_registry;
    xr_mutex_lock(&registry->mutation_gate);
    xr_mutex_lock(&authority->gate);
    bool live_binding =
        binding.generation->authority == authority &&
        binding.generation->plan == binding.plan && binding.plan == plan &&
        binding.generation->state == XR_MODULE_GENERATION_ACTIVE &&
        !binding.generation->poisoned &&
        !binding.generation->rollback_requested &&
        generation_identity_equal(&binding.generation_identity,
                                  &binding.generation->identity);
    if (!live_binding) {
        xr_mutex_unlock(&authority->gate);
        xr_mutex_unlock(&registry->mutation_gate);
        xr_runtime_entry_handle_release(&retained, verified,
                                        sizeof(verified));
        if (!was_frozen)
            handle_restore_frozen(handle, false);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                    "entry publication binding does not match the export authority");
    }
    XrRuntimeEntryRegistryRow *row = registry_find_locked(
        registry, semantic_fingerprint, export_record->id, callee->id);
    if (!row) {
        if (registry->count >= XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES ||
            !registry_reserve(registry, registry->count + 1u)) {
            xr_mutex_unlock(&authority->gate);
            xr_mutex_unlock(&registry->mutation_gate);
            xr_runtime_entry_handle_release(&retained, verified,
                                            sizeof(verified));
            if (!was_frozen)
                handle_restore_frozen(handle, false);
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                        "entry registry row budget is exhausted");
        }
        row = (XrRuntimeEntryRegistryRow *) xr_calloc(1, sizeof(*row));
        if (!row) {
            xr_mutex_unlock(&authority->gate);
            xr_mutex_unlock(&registry->mutation_gate);
            xr_runtime_entry_handle_release(&retained, verified,
                                            sizeof(verified));
            if (!was_frozen)
                handle_restore_frozen(handle, false);
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                        "entry registry row allocation failed");
        }
        row->semantic_fingerprint = semantic_fingerprint;
        row->export_identity = export_record->id;
        row->callee_identity = callee->id;
        atomic_init(&row->revision, 1u);
        registry->rows[registry->count++] = row;
    }
    if (row->handle == handle) {
        xr_mutex_unlock(&authority->gate);
        xr_mutex_unlock(&registry->mutation_gate);
        xr_runtime_entry_handle_release(&retained, verified,
                                        sizeof(verified));
        return true;
    }
    XrRuntimeEntryHandle *replaced = row->handle;
    XrRuntimeEntryHandle *replaced_guard = NULL;
    uint32_t replaced_rows = 0;
    if (replaced) {
        replaced_rows = registry_handle_row_count_locked(registry, replaced);
    }
    xr_mutex_unlock(&authority->gate);

    if (replaced && !xr_runtime_entry_handle_retain(
                        replaced, &replaced_guard, diagnostic,
                        diagnostic_size)) {
        xr_mutex_unlock(&registry->mutation_gate);
        xr_runtime_entry_handle_release(&retained, verified,
                                        sizeof(verified));
        if (!was_frozen)
            handle_restore_frozen(handle, false);
        return false;
    }

    if (replaced_rows == 1u &&
        !xr_entry_cell_clear(&replaced->cell, diagnostic,
                             diagnostic_size)) {
        xr_mutex_unlock(&registry->mutation_gate);
        xr_runtime_entry_handle_release(&retained, verified,
                                        sizeof(verified));
        xr_runtime_entry_handle_release(&replaced_guard, verified,
                                        sizeof(verified));
        if (!was_frozen)
            handle_restore_frozen(handle, false);
        return false;
    }

    xr_mutex_lock(&authority->gate);
    row->handle = retained;
    retained = NULL;
    if (!replaced)
        registry->active_count++;
    atomic_fetch_add_explicit(&row->revision, 1u, memory_order_release);
    atomic_fetch_add_explicit(&registry->mutations, 1u,
                              memory_order_relaxed);
    xr_mutex_unlock(&authority->gate);
    xr_mutex_unlock(&registry->mutation_gate);
    if (replaced_guard &&
        !xr_runtime_entry_handle_release(&replaced_guard, diagnostic,
                                         diagnostic_size))
        return false;
    if (replaced) {
        XrRuntimeEntryHandle *old_owner = replaced;
        if (!xr_runtime_entry_handle_release(&old_owner, diagnostic,
                                             diagnostic_size))
            return false;
    }
    return true;
}

bool xr_runtime_entry_registry_unpublish(
    XrRuntimeGenerationAuthority *authority, XrRuntimeEntryHandle *handle,
    char *diagnostic, size_t diagnostic_size) {
    if (!authority || !authority->entry_registry || !handle)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "entry unpublication authority is incomplete");
    XrRuntimeEntryHandle *guard = NULL;
    if (!xr_runtime_entry_handle_retain(
            handle, &guard, diagnostic, diagnostic_size))
        return false;
    uint32_t released_count = 0;
    XrRuntimeEntryRegistry *registry = authority->entry_registry;
    xr_mutex_lock(&registry->mutation_gate);
    xr_mutex_lock(&authority->gate);
    for (uint32_t i = 0; i < registry->count; i++)
        released_count += registry->rows[i]->handle == handle;
    if (!released_count) {
        xr_mutex_unlock(&authority->gate);
        xr_mutex_unlock(&registry->mutation_gate);
        return xr_runtime_entry_handle_release(&guard, diagnostic,
                                               diagnostic_size);
    }
    xr_mutex_unlock(&authority->gate);

    if (!xr_entry_cell_clear(&handle->cell, diagnostic, diagnostic_size)) {
        char nested[256] = {0};
        xr_mutex_unlock(&registry->mutation_gate);
        xr_runtime_entry_handle_release(&guard, nested, sizeof(nested));
        return false;
    }

    xr_mutex_lock(&authority->gate);
    for (uint32_t i = 0; i < registry->count; i++) {
        XrRuntimeEntryRegistryRow *row = registry->rows[i];
        if (row->handle != handle)
            continue;
        row->handle = NULL;
        registry->active_count--;
        atomic_fetch_add_explicit(&row->revision, 1u,
                                  memory_order_release);
    }
    if (released_count)
        atomic_fetch_add_explicit(&registry->mutations, 1u,
                                  memory_order_relaxed);
    xr_mutex_unlock(&authority->gate);
    xr_mutex_unlock(&registry->mutation_gate);
    if (!xr_runtime_entry_handle_release(&guard, diagnostic,
                                         diagnostic_size))
        return false;
    for (uint32_t i = 0; i < released_count; i++) {
        XrRuntimeEntryHandle *row_owner = handle;
        if (!xr_runtime_entry_handle_release(&row_owner, diagnostic,
                                             diagnostic_size))
            return false;
    }
    return true;
}

bool xr_runtime_entry_registry_stats(
    XrRuntimeGenerationAuthority *authority,
    XrRuntimeEntryRegistryStats *stats) {
    if (!authority || !authority->entry_registry || !stats)
        return false;
    memset(stats, 0, sizeof(*stats));
    xr_mutex_lock(&authority->gate);
    stats->allocated_rows = authority->entry_registry->count;
    stats->active_rows = authority->entry_registry->active_count;
    stats->active_modules = authority->entry_registry->active_modules;
    stats->active_provider_registrations =
        authority->entry_registry->active_provider_registrations;
    stats->active_finalizer_registrations =
        authority->entry_registry->active_finalizer_registrations;
    stats->mutations = atomic_load_explicit(
        &authority->entry_registry->mutations, memory_order_relaxed);
    xr_mutex_unlock(&authority->gate);
    return true;
}

bool xr_runtime_dynamic_entry_cache_create(
    XrLoadedModuleGeneration *generation,
    XrRuntimeDynamicEntryCache **cache, char *diagnostic,
    size_t diagnostic_size) {
    if (cache)
        *cache = NULL;
    uint32_t count = 0;
    xr_target_plan_entry_expectations(generation ? generation->plan : NULL,
                                      &count);
    size_t bytes = sizeof(XrRuntimeDynamicEntryCache) +
                   (size_t) count * sizeof(XrRuntimeDynamicEntrySlot);
    if (!generation || !cache || count > XR_RUNTIME_DYNAMIC_ENTRY_MAX_SITES ||
        bytes > XR_RUNTIME_DYNAMIC_ENTRY_MAX_CACHE_BYTES)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "dynamic entry cache exceeds its hard budget");
    XrRuntimeDynamicEntryCache *created =
        (XrRuntimeDynamicEntryCache *) xr_calloc(1, sizeof(*created));
    XrRuntimeDynamicEntrySlot *slots =
        count ? (XrRuntimeDynamicEntrySlot *) xr_calloc(
                    count, sizeof(*slots))
              : NULL;
    if (!created || (count && !slots)) {
        xr_free(slots);
        xr_free(created);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "dynamic entry cache allocation failed");
    }
    created->generation = generation;
    created->plan = generation->plan;
    created->plan_fingerprint = xr_target_plan_fingerprint(generation->plan);
    created->slots = slots;
    created->slot_count = count;
    created->bytes = bytes;
    for (uint32_t i = 0; i < count; i++)
        xr_mutex_init(&created->slots[i].gate);
    atomic_init(&created->hits, 0u);
    atomic_init(&created->misses, 0u);
    atomic_init(&created->registry_scans, 0u);
    atomic_init(&created->replacements, 0u);
    *cache = created;
    return true;
}

bool xr_runtime_dynamic_entry_cache_free(
    XrRuntimeDynamicEntryCache **cache, char *diagnostic,
    size_t diagnostic_size) {
    if (!cache || !*cache)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "dynamic entry cache owner is missing");
    XrRuntimeDynamicEntryCache *owned = *cache;
    for (uint32_t i = 0; i < owned->slot_count; i++) {
        XrRuntimeDynamicEntrySlot *slot = &owned->slots[i];
        xr_mutex_lock(&slot->gate);
        XrRuntimeEntryHandle *handle = slot->handle;
        if (handle) {
            if (!xr_runtime_entry_handle_release(
                    &handle, diagnostic, diagnostic_size)) {
                xr_mutex_unlock(&slot->gate);
                return false;
            }
            slot->handle = NULL;
            slot->row = NULL;
            slot->row_revision = 0;
        }
        xr_mutex_unlock(&slot->gate);
        xr_mutex_destroy(&slot->gate);
    }
    xr_free(owned->slots);
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
    *cache = NULL;
    return true;
}

bool xr_runtime_dynamic_entry_cache_stats(
    const XrRuntimeDynamicEntryCache *cache,
    XrRuntimeDynamicEntryCacheStats *stats) {
    if (!cache || !stats)
        return false;
    stats->hits = atomic_load_explicit(&cache->hits, memory_order_relaxed);
    stats->misses = atomic_load_explicit(&cache->misses,
                                         memory_order_relaxed);
    stats->registry_scans = atomic_load_explicit(
        &cache->registry_scans, memory_order_relaxed);
    stats->replacements = atomic_load_explicit(
        &cache->replacements, memory_order_relaxed);
    return true;
}

bool xr_runtime_dynamic_entry_lease_stats(
    XrLoadedModuleGeneration *generation,
    XrRuntimeDynamicEntryLeaseStats *stats) {
    if (!generation || !generation->authority || !stats)
        return false;
    memset(stats, 0, sizeof(*stats));
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->dynamic_entry_lease_gate);
    for (XrVmDynamicEntryLease *lease = authority->dynamic_entry_leases;
         lease; lease = lease->next) {
        if (lease->token.generation != generation)
            continue;
        stats->live++;
        stats->pending +=
            lease->state == XR_RUNTIME_DYNAMIC_ENTRY_LEASE_PENDING;
    }
    xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
    return true;
}

bool xr_runtime_dynamic_entry_generation_is_quiescent(
    XrLoadedModuleGeneration *generation) {
    if (!generation || !generation->authority)
        return false;
    XrRuntimeGenerationAuthority *authority = generation->authority;
    bool quiescent = true;
    xr_mutex_lock(&authority->dynamic_entry_lease_gate);
    for (XrVmDynamicEntryLease *lease = authority->dynamic_entry_leases;
         lease; lease = lease->next) {
        if (lease->token.generation == generation) {
            quiescent = false;
            break;
        }
    }
    xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
    return quiescent;
}

bool xr_runtime_dynamic_entry_retry_pending(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size) {
    if (!generation || !generation->authority)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "dynamic entry retry owner is missing");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    for (;;) {
        XrVmDynamicEntryLease *pending = NULL;
        xr_mutex_lock(&authority->dynamic_entry_lease_gate);
        for (XrVmDynamicEntryLease *lease = authority->dynamic_entry_leases;
             lease; lease = lease->next) {
            if (lease->token.generation == generation &&
                lease->state == XR_RUNTIME_DYNAMIC_ENTRY_LEASE_PENDING) {
                pending = lease;
                lease->state = XR_RUNTIME_DYNAMIC_ENTRY_LEASE_RETIRING;
                authority->pending_dynamic_entry_lease_count--;
                break;
            }
        }
        xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
        if (!pending)
            return true;

        bool released = xr_entry_call_release(
            &pending->token, diagnostic, diagnostic_size);
        xr_mutex_lock(&authority->dynamic_entry_lease_gate);
        if (released) {
            remove_dynamic_entry_lease_locked(authority, pending);
        } else {
            pending->state = XR_RUNTIME_DYNAMIC_ENTRY_LEASE_PENDING;
            authority->pending_dynamic_entry_lease_count++;
        }
        xr_mutex_unlock(&authority->dynamic_entry_lease_gate);
        if (!released) {
            char poison_diagnostic[256] = {0};
            xr_module_generation_poison(
                generation, dynamic_entry_retire_poison_fingerprint,
                poison_diagnostic, sizeof(poison_diagnostic));
            return false;
        }
        memset(pending, 0, sizeof(*pending));
        xr_free(pending);
    }
}

static bool expectation_matches_handle(
    const XrTargetEntryExpectationRecord *expected,
    const XrEntryCellExpectation *actual) {
    return expected && actual &&
           expected->abi_schema_version == actual->abi.schema_version &&
           expected->parameter_count == actual->abi.parameter_count &&
           expected->native_abi == actual->abi.native_abi &&
           expected->value_kind == actual->abi.value_kind &&
           expected->target_data_layout == actual->abi.target_data_layout &&
           fingerprint_matches(expected->target_profile_fingerprint,
                               actual->abi.target_profile_fingerprint) &&
           fingerprint_matches(expected->entry_abi_fingerprint,
                               actual->abi.fingerprint) &&
           expected->adapter_kind == XR_TARGET_ENTRY_ADAPTER_IDENTITY &&
           actual->adapter_kind == XR_ENTRY_ADAPTER_IDENTITY &&
           fingerprint_matches(expected->adapter_fingerprint,
                               actual->adapter_fingerprint) &&
           actual->executor_kind == XR_ENTRY_EXECUTOR_TYPED_VM;
}

static XrRuntimeEntryHandle *registry_lookup(
    XrRuntimeGenerationAuthority *authority, const XrTargetPlan *caller_plan,
    const XrTargetEntryExpectationRecord *expectation,
    const XrRuntimeEntryRegistryRow **matched_row, uint64_t *row_revision,
    bool *budget_exhausted) {
    if (matched_row)
        *matched_row = NULL;
    if (row_revision)
        *row_revision = 0;
    if (budget_exhausted)
        *budget_exhausted = false;
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(caller_plan, &call_count);
    const XrTargetCallRecord *call =
        expectation->call < call_count ? &calls[expectation->call] : NULL;
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(caller_plan);
    const XrSemanticDependencyRecord *dependency =
        call && semantic
            ? xr_semantic_plan_dependency(semantic,
                                          call->source_dependency)
            : NULL;
    if (!call || !dependency)
        return NULL;
    XrRuntimeEntryRegistry *registry = authority->entry_registry;
    XrRuntimeEntryHandle *candidate = NULL;
    const XrRuntimeEntryRegistryRow *candidate_row = NULL;
    uint64_t candidate_revision = 0;
    xr_mutex_lock(&registry->mutation_gate);
    xr_mutex_lock(&authority->gate);
    for (uint32_t i = 0; i < registry->count; i++) {
        XrRuntimeEntryRegistryRow *row = registry->rows[i];
        if (registry_key_equal(row, dependency->semantic_fingerprint,
                               call->source_export_identity,
                               call->source_callee_identity) && row->handle) {
            candidate = row->handle;
            candidate_row = row;
            candidate_revision = atomic_load_explicit(
                &row->revision, memory_order_acquire);
            break;
        }
    }
    xr_mutex_unlock(&authority->gate);
    XrRuntimeEntryHandle *handle = NULL;
    if (candidate) {
        char diagnostic[128] = {0};
        if (!xr_runtime_entry_handle_retain(
                candidate, &handle, diagnostic,
                sizeof(diagnostic)) && budget_exhausted)
            *budget_exhausted = true;
        if (handle) {
            if (matched_row)
                *matched_row = candidate_row;
            if (row_revision)
                *row_revision = candidate_revision;
        }
    }
    xr_mutex_unlock(&registry->mutation_gate);
    return handle;
}

static XrVmDynamicEntryStatus acquire_dynamic_entry(
    const XrVmDynamicEntryContext *context,
    const XrTargetPlan *caller_plan,
    const XrFingerprint *caller_fingerprint,
    const XrTargetEntryExpectationRecord *expectation, bool use_cache,
    XrVmDynamicEntryResolution *resolution) {
    if (resolution)
        memset(resolution, 0, sizeof(*resolution));
    XrLoadedModuleGeneration *caller =
        context ? (XrLoadedModuleGeneration *) context->owner : NULL;
    if (!caller || !resolution || !caller_plan || !caller_fingerprint ||
        !expectation || caller->plan != caller_plan ||
        !fingerprint_matches(*caller_fingerprint,
                             xr_target_plan_fingerprint(caller_plan)) ||
        !xr_target_plan_fingerprint_is_intact(caller_plan))
        return XR_VM_DYNAMIC_ENTRY_AUTHORITY_MISMATCH;
    XrRuntimeDynamicEntryCache *cache = caller->entry_cache;
    if (!cache || cache->plan != caller_plan ||
        !fingerprint_matches(cache->plan_fingerprint,
                             *caller_fingerprint) ||
        expectation->id >= cache->slot_count)
        return XR_VM_DYNAMIC_ENTRY_AUTHORITY_MISMATCH;

    XrRuntimeEntryHandle *handle = NULL;
    const XrRuntimeEntryRegistryRow *matched_row = NULL;
    bool retain_budget_exhausted = false;
    uint64_t row_revision = 0;
    if (use_cache) {
        XrRuntimeDynamicEntrySlot *slot = &cache->slots[expectation->id];
        xr_mutex_lock(&slot->gate);
        uint64_t current_revision =
            slot->row
                ? atomic_load_explicit(&slot->row->revision,
                                       memory_order_acquire)
                : 0;
        if (slot->handle && slot->row &&
            slot->row_revision == current_revision) {
            char diagnostic[128] = {0};
            if (!xr_runtime_entry_handle_retain(
                    slot->handle, &handle, diagnostic,
                    sizeof(diagnostic)))
                retain_budget_exhausted = true;
            else {
                matched_row = slot->row;
                row_revision = current_revision;
            }
        }
        xr_mutex_unlock(&slot->gate);
    }
    if (retain_budget_exhausted)
        return XR_VM_DYNAMIC_ENTRY_BUDGET_EXCEEDED;
    if (handle)
        atomic_fetch_add_explicit(&cache->hits, 1u, memory_order_relaxed);
    else {
        atomic_fetch_add_explicit(&cache->misses, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(&cache->registry_scans, 1u,
                                  memory_order_relaxed);
        handle = registry_lookup(caller->authority, caller_plan, expectation,
                                 &matched_row, &row_revision,
                                 &retain_budget_exhausted);
    }
    if (retain_budget_exhausted)
        return XR_VM_DYNAMIC_ENTRY_BUDGET_EXCEEDED;
    if (!handle)
        return XR_VM_DYNAMIC_ENTRY_NOT_FOUND;

    const XrEntryCellExpectation *actual = &handle->expectation;
    XrVmDynamicEntryLease *lease = NULL;
    char diagnostic[512] = {0};
    bool exact = expectation_matches_handle(expectation, actual);
    if (exact) {
        lease = reserve_dynamic_entry_lease(handle->authority);
        if (!lease) {
            xr_runtime_entry_handle_release(&handle, diagnostic,
                                            sizeof(diagnostic));
            return XR_VM_DYNAMIC_ENTRY_BUDGET_EXCEEDED;
        }
        exact = xr_entry_cell_acquire(&handle->cell, actual, &lease->token,
                                      diagnostic, sizeof(diagnostic));
        if (exact)
            activate_dynamic_entry_lease(lease);
    }
    if (exact)
        exact = xr_target_plan_fingerprint_is_intact(lease->token.plan) &&
                xr_target_instruction_program_verify(
                    lease->token.plan, diagnostic, sizeof(diagnostic));
    if (!exact) {
        if (lease && lease->token.generation) {
            XrVmDynamicEntryResolution rejected = {.lease = lease};
            retire_dynamic_entry_lease(lease, &rejected);
        } else {
            discard_reserved_dynamic_entry_lease(lease);
        }
        xr_runtime_entry_handle_release(&handle, diagnostic,
                                        sizeof(diagnostic));
        return XR_VM_DYNAMIC_ENTRY_AUTHORITY_MISMATCH;
    }

    if (use_cache) {
        XrRuntimeDynamicEntrySlot *slot = &cache->slots[expectation->id];
        xr_mutex_lock(&slot->gate);
        uint64_t current_revision = atomic_load_explicit(
            &matched_row->revision, memory_order_acquire);
        if (current_revision == row_revision &&
            (slot->row != matched_row || slot->handle != handle)) {
            XrRuntimeEntryHandle *replaced = slot->handle;
            XrRuntimeEntryHandle *retained = NULL;
            if (!xr_runtime_entry_handle_retain(
                    handle, &retained, diagnostic, sizeof(diagnostic))) {
                xr_mutex_unlock(&slot->gate);
                XrVmDynamicEntryResolution rejected = {.lease = lease};
                retire_dynamic_entry_lease(lease, &rejected);
                xr_runtime_entry_handle_release(
                    &handle, diagnostic, sizeof(diagnostic));
                return XR_VM_DYNAMIC_ENTRY_BUDGET_EXCEEDED;
            }
            if (replaced) {
                XrRuntimeEntryHandle *owned = replaced;
                if (!xr_runtime_entry_handle_release(
                        &owned, diagnostic, sizeof(diagnostic))) {
                    xr_runtime_entry_handle_release(
                        &retained, diagnostic, sizeof(diagnostic));
                    xr_mutex_unlock(&slot->gate);
                    XrVmDynamicEntryResolution rejected = {.lease = lease};
                    retire_dynamic_entry_lease(lease, &rejected);
                    xr_runtime_entry_handle_release(
                        &handle, diagnostic, sizeof(diagnostic));
                    return XR_VM_DYNAMIC_ENTRY_AUTHORITY_MISMATCH;
                }
            }
            slot->row = matched_row;
            slot->handle = retained;
            atomic_fetch_add_explicit(&cache->replacements, 1u,
                                      memory_order_relaxed);
        }
        if (current_revision == row_revision)
            slot->row_revision = row_revision;
        xr_mutex_unlock(&slot->gate);
    }

    resolution->plan = lease->token.plan;
    resolution->plan_fingerprint = lease->token.plan_fingerprint;
    resolution->function = lease->token.function;
    resolution->decoded_cache = lease->token.generation->decoded_cache;
    resolution->dynamic_entries =
        &lease->token.generation->dynamic_entries;
    resolution->generation_identity = lease->token.generation->identity;
    resolution->lease = lease;
    xr_runtime_entry_handle_release(&handle, diagnostic, sizeof(diagnostic));
    return XR_VM_DYNAMIC_ENTRY_OK;
}

static XrVmDynamicEntryStatus validate_dynamic_entry_context(
    const XrVmDynamicEntryContext *context,
    const XrTargetPlan *caller_plan,
    const XrFingerprint *caller_fingerprint,
    const XrModuleGenerationIdentity *caller_generation_identity) {
    XrLoadedModuleGeneration *caller =
        context ? (XrLoadedModuleGeneration *) context->owner : NULL;
    if (!caller || !caller->authority || !caller_plan ||
        !caller_fingerprint || !caller_generation_identity ||
        context->verified_plan != caller_plan || caller->plan != caller_plan ||
        !fingerprint_matches(*caller_fingerprint,
                             xr_target_plan_fingerprint(caller_plan)) ||
        !fingerprint_matches(context->plan_fingerprint,
                             *caller_fingerprint) ||
        !generation_identity_equal(&context->generation_identity,
                                   caller_generation_identity) ||
        !generation_identity_equal(caller_generation_identity,
                                   &caller->identity))
        return XR_VM_DYNAMIC_ENTRY_AUTHORITY_MISMATCH;
    return XR_VM_DYNAMIC_ENTRY_OK;
}

static XrVmDynamicEntryStatus retire_dynamic_entry(
    const XrVmDynamicEntryContext *context,
    XrVmDynamicEntryResolution *resolution) {
    XrVmDynamicEntryLease *lease = resolution ? resolution->lease : NULL;
    if (!lease)
        return XR_VM_DYNAMIC_ENTRY_INVALID_ARGUMENT;
    XrLoadedModuleGeneration *caller =
        context ? (XrLoadedModuleGeneration *) context->owner : NULL;
    bool same_authority = caller && caller->authority == lease->authority;
    XrVmDynamicEntryStatus status =
        retire_dynamic_entry_lease(lease, resolution);
    return same_authority ? status
                          : XR_VM_DYNAMIC_ENTRY_AUTHORITY_MISMATCH;
}

void xr_runtime_dynamic_entry_context_init(
    XrLoadedModuleGeneration *generation,
    XrVmDynamicEntryContext *context) {
    if (!context)
        return;
    *context = (XrVmDynamicEntryContext) {
        .schema_version = XR_VM_DYNAMIC_ENTRY_CONTEXT_SCHEMA_VERSION,
        .owner = generation,
        .verified_plan = generation ? generation->plan : NULL,
        .plan_fingerprint = generation
                                ? xr_target_plan_fingerprint(generation->plan)
                                : (XrFingerprint) {{0}},
        .generation_identity = generation
                                   ? generation->identity
                                   : (XrModuleGenerationIdentity) {0},
        .validate = validate_dynamic_entry_context,
        .acquire = acquire_dynamic_entry,
        .retire = retire_dynamic_entry,
    };
}
