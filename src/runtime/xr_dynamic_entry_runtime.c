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
#include "../vm/xr_vm_decoded_cache.h"
#include <stdio.h>
#include <string.h>

typedef struct XrRuntimeEntryRegistryRow {
    XrFingerprint semantic_fingerprint;
    XrStableId export_identity;
    XrStableId callee_identity;
    XrRuntimeEntryHandle *handle;
    atomic_uint_least64_t revision;
} XrRuntimeEntryRegistryRow;

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

typedef struct XrRuntimeDynamicEntryLease {
    XrEntryCallToken token;
} XrRuntimeDynamicEntryLease;

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

static bool fail(char *diagnostic, size_t diagnostic_size, const char *code,
                 const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
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
    if (owned->active_count != 0)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "entry registry still publishes handles");
    for (uint32_t i = 0; i < owned->count; i++)
        xr_free(owned->rows[i]);
    xr_free(owned->rows);
    xr_mutex_destroy(&owned->mutation_gate);
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
    *registry = NULL;
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
            if (!was_frozen)
                handle_restore_frozen(handle, false);
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                        "entry registry row budget is exhausted");
        }
        row = (XrRuntimeEntryRegistryRow *) xr_calloc(1, sizeof(*row));
        if (!row) {
            xr_mutex_unlock(&authority->gate);
            xr_mutex_unlock(&registry->mutation_gate);
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
        return true;
    }
    XrRuntimeEntryHandle *retained = NULL;
    if (!xr_runtime_entry_handle_retain(
            handle, &retained, diagnostic, diagnostic_size)) {
        xr_mutex_unlock(&authority->gate);
        xr_mutex_unlock(&registry->mutation_gate);
        if (!was_frozen)
            handle_restore_frozen(handle, false);
        return false;
    }
    XrRuntimeEntryHandle *replaced = row->handle;
    XrRuntimeEntryHandle *replaced_guard = NULL;
    uint32_t replaced_rows = 0;
    if (replaced) {
        replaced_rows = registry_handle_row_count_locked(registry, replaced);
        if (!xr_runtime_entry_handle_retain(
                replaced, &replaced_guard, diagnostic, diagnostic_size)) {
            xr_runtime_entry_handle_release(&retained, verified,
                                            sizeof(verified));
            xr_mutex_unlock(&authority->gate);
            xr_mutex_unlock(&registry->mutation_gate);
            if (!was_frozen)
                handle_restore_frozen(handle, false);
            return false;
        }
    }
    xr_mutex_unlock(&authority->gate);

    if (replaced_rows == 1u &&
        !xr_entry_cell_clear(&replaced->cell, diagnostic,
                             diagnostic_size)) {
        xr_runtime_entry_handle_release(&retained, verified,
                                        sizeof(verified));
        xr_runtime_entry_handle_release(&replaced_guard, verified,
                                        sizeof(verified));
        if (!was_frozen)
            handle_restore_frozen(handle, false);
        xr_mutex_unlock(&registry->mutation_gate);
        return false;
    }

    xr_mutex_lock(&authority->gate);
    row->handle = retained;
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
    uint32_t released_count = 0;
    XrRuntimeEntryRegistry *registry = authority->entry_registry;
    xr_mutex_lock(&registry->mutation_gate);
    xr_mutex_lock(&authority->gate);
    for (uint32_t i = 0; i < registry->count; i++)
        released_count += registry->rows[i]->handle == handle;
    if (!released_count) {
        xr_mutex_unlock(&authority->gate);
        xr_mutex_unlock(&registry->mutation_gate);
        return true;
    }
    XrRuntimeEntryHandle *guard = NULL;
    if (!xr_runtime_entry_handle_retain(
            handle, &guard, diagnostic, diagnostic_size)) {
        xr_mutex_unlock(&authority->gate);
        xr_mutex_unlock(&registry->mutation_gate);
        return false;
    }
    xr_mutex_unlock(&authority->gate);

    if (!xr_entry_cell_clear(&handle->cell, diagnostic, diagnostic_size)) {
        char nested[256] = {0};
        xr_runtime_entry_handle_release(&guard, nested, sizeof(nested));
        xr_mutex_unlock(&registry->mutation_gate);
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
    xr_mutex_lock(&authority->gate);
    stats->allocated_rows = authority->entry_registry->count;
    stats->active_rows = authority->entry_registry->active_count;
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
    XrRuntimeEntryHandle *handle = NULL;
    xr_mutex_lock(&authority->gate);
    XrRuntimeEntryRegistry *registry = authority->entry_registry;
    for (uint32_t i = 0; i < registry->count; i++) {
        XrRuntimeEntryRegistryRow *row = registry->rows[i];
        if (registry_key_equal(row, dependency->semantic_fingerprint,
                               call->source_export_identity,
                               call->source_callee_identity) && row->handle) {
            char diagnostic[128] = {0};
            if (!xr_runtime_entry_handle_retain(
                    row->handle, &handle, diagnostic,
                    sizeof(diagnostic)) && budget_exhausted)
                *budget_exhausted = true;
            if (handle) {
                if (matched_row)
                    *matched_row = row;
                if (row_revision)
                    *row_revision = atomic_load_explicit(
                        &row->revision, memory_order_acquire);
            }
            break;
        }
    }
    xr_mutex_unlock(&authority->gate);
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
    XrRuntimeDynamicEntryLease *lease = NULL;
    char diagnostic[512] = {0};
    bool exact = expectation_matches_handle(expectation, actual);
    if (exact) {
        lease = (XrRuntimeDynamicEntryLease *) xr_calloc(1, sizeof(*lease));
        exact = lease && xr_entry_cell_acquire(
                             &handle->cell, actual, &lease->token,
                             diagnostic, sizeof(diagnostic));
    }
    if (exact)
        exact = xr_target_plan_fingerprint_is_intact(lease->token.plan) &&
                xr_target_instruction_program_verify(
                    lease->token.plan, diagnostic, sizeof(diagnostic));
    if (!exact) {
        if (lease && lease->token.generation)
            xr_entry_call_release(&lease->token, diagnostic,
                                  sizeof(diagnostic));
        xr_free(lease);
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
                xr_entry_call_release(&lease->token, diagnostic,
                                      sizeof(diagnostic));
                xr_free(lease);
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
                    xr_entry_call_release(&lease->token, diagnostic,
                                          sizeof(diagnostic));
                    xr_free(lease);
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

static XrVmDynamicEntryStatus release_dynamic_entry(
    const XrVmDynamicEntryContext *context,
    XrVmDynamicEntryResolution *resolution) {
    (void) context;
    XrRuntimeDynamicEntryLease *lease =
        resolution ? (XrRuntimeDynamicEntryLease *) resolution->lease : NULL;
    if (!lease)
        return XR_VM_DYNAMIC_ENTRY_INVALID_ARGUMENT;
    char diagnostic[256] = {0};
    if (!xr_entry_call_release(&lease->token, diagnostic,
                               sizeof(diagnostic)))
        return XR_VM_DYNAMIC_ENTRY_RELEASE_FAILED;
    memset(resolution, 0, sizeof(*resolution));
    memset(lease, 0, sizeof(*lease));
    xr_free(lease);
    return XR_VM_DYNAMIC_ENTRY_OK;
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
        .release = release_dynamic_entry,
    };
}
