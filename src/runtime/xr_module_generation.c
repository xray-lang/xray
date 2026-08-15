/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_generation.c - Runtime-owned loaded generation lifecycle
 *
 * KEY CONCEPT:
 *   Every mutation is serialized by the owning runtime authority. This makes
 *   generation and pin budgets transactional while preserving immutable plan
 *   identity across activation, drain, rollback, and unload decisions.
 */

#include "xr_module_generation_internal.h"
#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../plan/semantic/xr_semantic_ids.h"
#include "../plan/target/xr_target_instruction_verify.h"
#include "../plan/target/xr_target_plan.h"
#include "../plan/target/xr_target_profile_internal.h"
#include "../plan/target/xr_target_verify.h"
#include "../vm/xr_typed_dispatch.h"
#include "../vm/xr_typed_frame.h"
#include "../vm/xr_vm_decoded_cache.h"
#include "abi/xr_runtime_target_authority.h"
#include "xr_dynamic_entry_runtime.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *diagnostic, size_t diagnostic_size, const char *code,
                 const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void compute_generation_fingerprint(XrModuleGenerationIdentity *identity) {
    static const uint8_t domain[] = "xray-module-generation-v1\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, identity->schema_version);
    hash_u32(&context, identity->target_plan_schema_version);
    hash_u64(&context, identity->generation_number);
    hash_u64(&context, identity->completed_family_mask);
    hash_u64(&context, identity->required_capability_mask);
    xr_sha256_update(&context, identity->semantic_fingerprint,
                     XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->target_profile_fingerprint,
                     XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->target_plan_fingerprint,
                     XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->runtime_abi_fingerprint,
                     XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->provider_set_fingerprint,
                     XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->object_header_fingerprint,
                     XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    xr_sha256_final(&context, identity->generation_fingerprint);
}

static bool plan_capability_mask(const XrTargetPlan *plan, uint64_t *out) {
    uint32_t count = 0;
    const XrTargetCapabilityRecord *records =
        xr_target_plan_capabilities(plan, &count);
    uint64_t mask = 0;
    for (uint32_t i = 0; i < count; i++) {
        const XrTargetCapabilityRecord *record = &records[i];
        if (record->id != i ||
            record->provider != record->capability ||
            record->provider <= XR_TARGET_PROVIDER_INVALID ||
            record->provider >= XR_TARGET_PROVIDER_KIND_COUNT ||
            record->flags != XR_TARGET_CAPABILITY_REQUIRED)
            return false;
        uint64_t bit = XR_TARGET_PROVIDER_MASK(record->provider);
        if ((mask & bit) != 0)
            return false;
        mask |= bit;
    }
    *out = mask;
    return true;
}

static bool populate_identity(const XrTargetPlan *plan, uint64_t generation_number,
                              XrModuleGenerationIdentity *identity,
                              char *diagnostic, size_t diagnostic_size) {
    char nested[512] = {0};
    if (!xr_target_plan_is_verified(plan) ||
        !xr_target_plan_verify(plan, nested, sizeof(nested)))
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5000",
                    "module generation requires an independently verified TargetPlan");

    const XrTargetProfile *profile = xr_target_plan_profile(plan);
    const XrTargetProfileDraft *facts = xr_target_profile_facts(profile);
    XrRuntimeTargetAuthority runtime;
    XrFingerprint runtime_fingerprint;
    XrFingerprint provider_fingerprint;
    XrFingerprint object_fingerprint;
    XrRuntimeObjectHeaderAbi object_header;
    uint64_t provider_mask = 0;
    uint64_t capability_mask = 0;
    if (!facts ||
        xr_runtime_target_authority_native_hosted(&runtime) !=
            XR_RUNTIME_ABI_OK ||
        !xr_runtime_target_authority_machine_matches(&runtime,
                                                     &facts->machine) ||
        xr_runtime_abi_contract_fingerprint(&runtime.runtime_abi,
                                            &runtime_fingerprint) !=
            XR_RUNTIME_ABI_OK ||
        xr_target_provider_set_fingerprint(
            runtime.providers, runtime.provider_count, &provider_mask,
            &provider_fingerprint) != XR_RUNTIME_ABI_OK ||
        xr_runtime_object_header_abi_materialize(
            &runtime.object_header_materialization, &object_header) !=
            XR_RUNTIME_ABI_OK ||
        xr_runtime_object_header_abi_fingerprint(&object_header,
                                                 &object_fingerprint) !=
            XR_RUNTIME_ABI_OK ||
        !plan_capability_mask(plan, &capability_mask))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1000",
                    "TargetPlan does not bind the canonical native runtime identity");
    if (capability_mask != XR_TARGET_FOUNDATION_CAPABILITY_MASK ||
        (capability_mask & ~provider_mask) != 0 ||
        !xr_fingerprint_equal(facts->runtime_abi_fingerprint,
                              runtime_fingerprint) ||
        !xr_fingerprint_equal(facts->provider_set_fingerprint,
                              provider_fingerprint) ||
        !xr_fingerprint_equal(facts->object_header_fingerprint,
                              object_fingerprint))
        return fail(diagnostic, diagnostic_size, "XR_TARGET_1004",
                    "TargetPlan capability closure is not runtime-owned and exact");

    memset(identity, 0, sizeof(*identity));
    identity->schema_version = XR_RUNTIME_GENERATION_SCHEMA_VERSION;
    identity->target_plan_schema_version =
        xr_target_plan_schema_version(plan);
    identity->generation_number = generation_number;
    identity->completed_family_mask =
        xr_target_plan_completed_family_mask(plan);
    identity->required_capability_mask = capability_mask;
    memcpy(identity->semantic_fingerprint,
           xr_target_plan_semantic_fingerprint(plan).bytes,
           XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    memcpy(identity->target_profile_fingerprint,
           xr_target_profile_fingerprint(profile).bytes,
           XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    memcpy(identity->target_plan_fingerprint,
           xr_target_plan_fingerprint(plan).bytes,
           XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    memcpy(identity->runtime_abi_fingerprint, runtime_fingerprint.bytes,
           XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    memcpy(identity->provider_set_fingerprint, provider_fingerprint.bytes,
           XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    memcpy(identity->object_header_fingerprint, object_fingerprint.bytes,
           XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    compute_generation_fingerprint(identity);
    return true;
}

static bool budget_valid(const XrRuntimeGenerationBudget *budget) {
    if (!budget ||
        budget->schema_version != XR_RUNTIME_GENERATION_SCHEMA_VERSION ||
        budget->max_loaded_generations == 0 ||
        budget->max_total_pins == 0 ||
        budget->max_pins_per_generation == 0 ||
        budget->max_pins_per_generation > budget->max_total_pins)
        return false;
    for (uint32_t i = 0; i < XR_MODULE_GENERATION_PIN_KIND_COUNT; i++) {
        if (budget->max_pins_by_kind[i] == 0 ||
            budget->max_pins_by_kind[i] >
                budget->max_pins_per_generation)
            return false;
    }
    return true;
}

XRAY_API bool xr_runtime_generation_authority_create(
    const XrRuntimeGenerationBudget *budget,
    XrRuntimeGenerationAuthority **authority, char *diagnostic,
    size_t diagnostic_size) {
    if (authority)
        *authority = NULL;
    if (!authority || !budget_valid(budget))
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "generation authority requires a complete nonzero hard budget");
    XrRuntimeGenerationAuthority *created =
        (XrRuntimeGenerationAuthority *) xr_calloc(1, sizeof(*created));
    if (!created)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "generation authority allocation failed");
    xr_mutex_init(&created->gate);
    xr_mutex_init(&created->dynamic_entry_lease_gate);
    created->budget = *budget;
    created->next_generation = 1;
    if (!xr_runtime_entry_registry_create(&created->entry_registry,
                                          diagnostic, diagnostic_size)) {
        xr_mutex_destroy(&created->dynamic_entry_lease_gate);
        xr_mutex_destroy(&created->gate);
        xr_free(created);
        return false;
    }
    *authority = created;
    return true;
}

XRAY_API bool xr_runtime_generation_authority_destroy(
    XrRuntimeGenerationAuthority **authority, char *diagnostic,
    size_t diagnostic_size) {
    if (!authority || !*authority)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "generation authority is missing");
    XrRuntimeGenerationAuthority *owned = *authority;
    xr_mutex_lock(&owned->gate);
    bool empty = owned->live_generations == 0 && owned->total_pins == 0;
    xr_mutex_unlock(&owned->gate);
    xr_mutex_lock(&owned->dynamic_entry_lease_gate);
    bool leases_empty = owned->dynamic_entry_lease_count == 0 &&
                        owned->pending_dynamic_entry_lease_count == 0 &&
                        owned->dynamic_entry_leases == NULL;
    xr_mutex_unlock(&owned->dynamic_entry_lease_gate);
    if (!empty || !leases_empty)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "generation authority still owns loaded modules, pins, or dynamic leases");
    if (!xr_runtime_entry_registry_destroy(&owned->entry_registry, diagnostic,
                                           diagnostic_size))
        return false;
    xr_mutex_destroy(&owned->dynamic_entry_lease_gate);
    xr_mutex_destroy(&owned->gate);
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
    *authority = NULL;
    return true;
}

XRAY_API bool xr_runtime_generation_activation_available(void) {
    return true;
}

XRAY_API bool xr_module_generation_load_verified_target_plan(
    XrRuntimeGenerationAuthority *authority,
    const XrTargetPlan *verified_target_plan,
    XrLoadedModuleGeneration **generation, char *diagnostic,
    size_t diagnostic_size) {
    if (generation)
        *generation = NULL;
    if (!authority || !verified_target_plan || !generation)
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "generation load requires a runtime authority and verified plan");

    XrLoadedModuleGeneration *created =
        (XrLoadedModuleGeneration *) xr_calloc(1, sizeof(*created));
    if (!created)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "module generation allocation failed");

    xr_mutex_lock(&authority->gate);
    if (authority->live_generations >=
            authority->budget.max_loaded_generations ||
        authority->next_generation == 0 ||
        authority->next_generation == UINT64_MAX) {
        xr_mutex_unlock(&authority->gate);
        xr_free(created);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "loaded generation budget is exhausted");
    }
    uint64_t number = authority->next_generation;
    authority->next_generation++;
    authority->live_generations++;
    xr_mutex_unlock(&authority->gate);

    if (!populate_identity(verified_target_plan, number, &created->identity,
                           diagnostic, diagnostic_size)) {
        xr_mutex_lock(&authority->gate);
        authority->live_generations--;
        xr_mutex_unlock(&authority->gate);
        xr_free(created);
        return false;
    }

    xr_mutex_lock(&authority->gate);
    created->authority = authority;
    created->plan = xr_target_plan_retain((XrTargetPlan *) verified_target_plan);
    created->state = XR_MODULE_GENERATION_LOADING;
    created->revision = 1;
    created->state = XR_MODULE_GENERATION_VERIFIED;
    created->revision++;
    xr_mutex_unlock(&authority->gate);
    *generation = created;
    return true;
}

static bool transition_locked(XrLoadedModuleGeneration *generation,
                              XrModuleGenerationState expected,
                              XrModuleGenerationState next, const char *detail,
                              char *diagnostic, size_t diagnostic_size) {
    if (generation->state != expected)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005", detail);
    generation->state = next;
    generation->revision++;
    return true;
}

static bool plan_has_no_non_scalar_execution_authority(
    const XrTargetPlan *plan) {
    uint32_t count = 0;
    xr_target_plan_storage(plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_allocations(plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_extent_operands(plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_calls(plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_call_arguments(plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_root_maps(plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_root_slots(plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_cleanups(plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_adapters(plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_coroutines(plan, &count);
    return count == 0;
}

static bool sole_scalar_generation_eligible(
    const XrLoadedModuleGeneration *generation) {
    char diagnostic[512] = {0};
    if (!generation || !generation->plan ||
        !xr_target_plan_is_verified(generation->plan) ||
        !xr_target_plan_fingerprint_is_intact(generation->plan) ||
        !xr_target_plan_verify(generation->plan, diagnostic,
                               sizeof(diagnostic)) ||
        xr_target_plan_schema_version(generation->plan) !=
            XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION ||
        xr_target_plan_completed_family_mask(generation->plan) !=
            XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK ||
        memcmp(generation->identity.target_plan_fingerprint,
               xr_target_plan_fingerprint(generation->plan).bytes,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0)
        return false;
    if (!xr_target_instruction_program_verify(
            generation->plan, diagnostic, sizeof(diagnostic)))
        return false;

    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(generation->plan, &function_count);
    uint32_t instruction_count = 0;
    const XrTargetInstructionRecord *instructions =
        xr_target_plan_instructions(generation->plan, &instruction_count);
    uint32_t sole_instruction_count = 0;
    const XrTargetInstructionRecord *sole_instructions =
        xr_target_plan_function_instructions(generation->plan, 0,
                                             &sole_instruction_count);
    if (!functions || function_count != 1 || functions[0].id != 0 ||
        functions[0].semantic_function != 0 || functions[0].root_count != 0 ||
        functions[0].cleanup_count != 0 ||
        functions[0].coroutine_count != 0 || !instructions ||
        !sole_instructions || instruction_count == 0 ||
        instruction_count != sole_instruction_count ||
        xr_target_plan_function_execution_family_mask(generation->plan, 0) !=
            XR_TARGET_EXECUTION_SCALAR_I64_CLOSED)
        return false;
    return plan_has_no_non_scalar_execution_authority(generation->plan);
}

static bool typed_generation_eligible(
    const XrLoadedModuleGeneration *generation) {
    char diagnostic[512] = {0};
    if (!generation || !generation->plan ||
        !xr_target_plan_is_verified(generation->plan) ||
        !xr_target_plan_fingerprint_is_intact(generation->plan) ||
        !xr_target_plan_verify(generation->plan, diagnostic,
                               sizeof(diagnostic)) ||
        xr_target_plan_schema_version(generation->plan) !=
            XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION ||
        xr_target_plan_completed_family_mask(generation->plan) !=
            XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK ||
        memcmp(generation->identity.target_plan_fingerprint,
               xr_target_plan_fingerprint(generation->plan).bytes,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0 ||
        !xr_target_instruction_program_verify(
            generation->plan, diagnostic, sizeof(diagnostic)))
        return false;

    uint32_t count = 0;
    xr_target_plan_storage(generation->plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_allocations(generation->plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_extent_operands(generation->plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_root_maps(generation->plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_root_slots(generation->plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_cleanups(generation->plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_adapters(generation->plan, &count);
    if (count != 0)
        return false;
    xr_target_plan_coroutines(generation->plan, &count);
    if (count != 0)
        return false;

    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(generation->plan, &count);
    if (!functions || count == 0)
        return false;
    bool executable = false;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t family = xr_target_plan_function_execution_family_mask(
            generation->plan, i);
        if (family == 0)
            continue;
        if (family != XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
            family != XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC)
            return false;
        executable = true;
    }
    return executable;
}

static bool fail_typed_dispatch(XrTypedDispatchStatus status,
                                char *diagnostic,
                                size_t diagnostic_size) {
    switch (status) {
        case XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                        "scalar generation plan identity changed");
        case XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                        "sole-function scalar i64 authority is unavailable");
        case XR_TYPED_DISPATCH_FRAME_ERROR:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5001",
                        "scalar generation frame rejected a slot representation");
        case XR_TYPED_DISPATCH_ARGUMENT_MISMATCH:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                        "sole-function scalar i64 route executes only a parameterless signature");
        /* A program fault, not an authority failure: the generation was
         * eligible and the row was verified, and the executed program divided
         * by zero. It keeps its own code so an operator cannot read it as a
         * verification or identity problem. */
        case XR_TYPED_DISPATCH_DIVIDE_BY_ZERO:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5009",
                        "scalar generation divided by zero");
        case XR_TYPED_DISPATCH_MODULO_BY_ZERO:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5009",
                        "scalar generation took a modulo by zero");
        /* Also a program fault: the generation was eligible and every row was
         * verified, and the program did not reach a return inside the step
         * budget. It is reported rather than waited out. */
        case XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5009",
                        "scalar generation exceeded the executor step budget");
        case XR_TYPED_DISPATCH_CALL_DEPTH_EXCEEDED:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5009",
                        "scalar generation exceeded the executor call-depth budget");
        case XR_TYPED_DISPATCH_ENTRY_UNAVAILABLE:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                        "scalar generation dynamic entry is unavailable");
        case XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                        "scalar generation dynamic entry authority changed");
        case XR_TYPED_DISPATCH_ENTRY_BUDGET_EXCEEDED:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                        "scalar generation dynamic entry budget is exhausted");
        case XR_TYPED_DISPATCH_ENTRY_RETIRE_DEFERRED:
            return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                        "scalar generation dynamic entry retirement was deferred");
        case XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH:
        case XR_TYPED_DISPATCH_DEBUG_CONTROL_ERROR:
        case XR_TYPED_DISPATCH_DEBUG_TERMINATED:
        case XR_TYPED_DISPATCH_DEBUG_STOP_REJECTED:
        case XR_TYPED_DISPATCH_TRACE_REJECTED:
        case XR_TYPED_DISPATCH_INVALID_ARGUMENT:
        case XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED:
        case XR_TYPED_DISPATCH_PROGRAM_INVALID:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5000",
                        "verified sole-function scalar i64 execution failed");
        case XR_TYPED_DISPATCH_OK:
            return true;
    }
    return fail(diagnostic, diagnostic_size, "XR_EXEC_5000",
                "scalar generation dispatcher returned an unknown status");
}

static XrFingerprint generation_plan_fingerprint(
    const XrLoadedModuleGeneration *generation) {
    XrFingerprint fingerprint = {{0}};
    if (generation)
        memcpy(fingerprint.bytes,
               generation->identity.target_plan_fingerprint,
               sizeof(fingerprint.bytes));
    return fingerprint;
}

static bool fail_decoded_cache(XrVmDecodedCacheStatus status,
                               char *diagnostic,
                               size_t diagnostic_size) {
    switch (status) {
        case XR_VM_DECODED_CACHE_BUDGET_EXCEEDED:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                        "decoded instruction cache exceeds its hard budget");
        case XR_VM_DECODED_CACHE_ALLOCATION_FAILED:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                        "decoded instruction cache allocation failed");
        case XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5008",
                        "decoded instruction cache plan identity changed");
        case XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED:
        case XR_VM_DECODED_CACHE_PROGRAM_INVALID:
        case XR_VM_DECODED_CACHE_INVALID_ARGUMENT:
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5000",
                        "decoded instruction cache refused the verified plan");
        case XR_VM_DECODED_CACHE_OK:
            return true;
    }
    return fail(diagnostic, diagnostic_size, "XR_EXEC_5000",
                "decoded instruction cache returned an unknown status");
}

static XrVmDecodedCacheStatus generation_cache_require_exact(
    const XrLoadedModuleGeneration *generation) {
    if (!generation)
        return XR_VM_DECODED_CACHE_INVALID_ARGUMENT;
    XrFingerprint fingerprint = generation_plan_fingerprint(generation);
    return xr_typed_decoded_cache_require_exact(
        generation->decoded_cache, generation->plan, &fingerprint);
}

XRAY_API bool xr_module_generation_prepare(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size) {
    if (!generation || !generation->authority)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "generation is missing");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    bool eligible = generation->state == XR_MODULE_GENERATION_VERIFIED &&
                    !generation->poisoned &&
                    !generation->rollback_requested;
    if (!eligible) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "only a healthy verified generation may become ready");
    }
    if (!typed_generation_eligible(generation)) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "generation lacks exact installed scalar i64 authority");
    }
    XrFingerprint fingerprint = generation_plan_fingerprint(generation);
    XrVmDecodedCache *decoded_cache = NULL;
    XrVmDecodedCacheStatus cache_status = xr_typed_decoded_cache_create(
        generation->plan, &fingerprint, &decoded_cache);
    if (cache_status != XR_VM_DECODED_CACHE_OK) {
        xr_mutex_unlock(&authority->gate);
        return fail_decoded_cache(cache_status, diagnostic, diagnostic_size);
    }
    generation->decoded_cache = decoded_cache;
    XrRuntimeDynamicEntryCache *entry_cache = NULL;
    if (!xr_runtime_dynamic_entry_cache_create(
            generation, &entry_cache, diagnostic, diagnostic_size)) {
        generation->decoded_cache = NULL;
        xr_typed_decoded_cache_free(decoded_cache);
        xr_mutex_unlock(&authority->gate);
        return false;
    }
    generation->entry_cache = entry_cache;
    xr_runtime_dynamic_entry_context_init(generation,
                                          &generation->dynamic_entries);
    bool ok = transition_locked(generation, XR_MODULE_GENERATION_VERIFIED,
                                XR_MODULE_GENERATION_READY,
                                "only a verified generation may become ready",
                                diagnostic, diagnostic_size);
    if (!ok) {
        generation->decoded_cache = NULL;
        generation->entry_cache = NULL;
        memset(&generation->dynamic_entries, 0,
               sizeof(generation->dynamic_entries));
        xr_runtime_dynamic_entry_cache_free(
            &entry_cache, diagnostic, diagnostic_size);
        xr_typed_decoded_cache_free(decoded_cache);
    }
    xr_mutex_unlock(&authority->gate);
    return ok;
}

XRAY_API bool xr_module_generation_activate(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size) {
    if (!generation || !generation->authority)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "generation is missing");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    bool ready = generation->state == XR_MODULE_GENERATION_READY &&
                 !generation->poisoned &&
                 !generation->rollback_requested;
    if (!ready) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_ARTIFACT_2004",
                    "generation activation requires the exact READY state");
    }
    XrVmDecodedCacheStatus cache_status =
        generation_cache_require_exact(generation);
    if (cache_status != XR_VM_DECODED_CACHE_OK) {
        xr_mutex_unlock(&authority->gate);
        return fail_decoded_cache(cache_status, diagnostic, diagnostic_size);
    }
    bool ok = transition_locked(generation, XR_MODULE_GENERATION_READY,
                                XR_MODULE_GENERATION_ACTIVE,
                                "generation activation requires the exact READY state",
                                diagnostic, diagnostic_size);
    xr_mutex_unlock(&authority->gate);
    return ok;
}

XRAY_API bool xr_module_generation_execute_sole_scalar_i64(
    XrLoadedModuleGeneration *generation, int64_t *result,
    char *diagnostic, size_t diagnostic_size) {
    if (result)
        *result = 0;
    if (!generation || !result)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "sole-function scalar execution requires a generation and result");
    if (!xr_module_generation_pin_acquire(
            generation, XR_MODULE_GENERATION_INFLIGHT_CALL,
            diagnostic, diagnostic_size))
        return false;

    if (!sole_scalar_generation_eligible(generation)) {
        xr_module_generation_pin_release(
            generation, XR_MODULE_GENERATION_INFLIGHT_CALL,
            diagnostic, diagnostic_size);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5004",
                    "sole-function scalar route requires one closed parameterless function");
    }

    XrVmDecodedCacheStatus cache_status =
        generation_cache_require_exact(generation);
    XrFingerprint required_fingerprint =
        generation_plan_fingerprint(generation);
    int64_t executed_result = 0;
    /* This product route carries no argument vector, so a plan whose sole
     * function declares parameters fails closed instead of being executed
     * against implicit zeros. */
    XrTypedDispatchI64Request request = {
        .verified_plan = generation->plan,
        .required_plan_fingerprint = &required_fingerprint,
        .result = &executed_result,
        .decoded_cache = generation->decoded_cache,
        .dynamic_entries = NULL,
        .generation_identity = &generation->identity,
        .provider = XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
        .use_dynamic_entry_cache = false,
    };
    XrTypedDispatchStatus status =
        cache_status == XR_VM_DECODED_CACHE_OK
            ? xr_typed_dispatch_execute_i64(&request)
            : XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE;
    if (!xr_module_generation_pin_release(
            generation, XR_MODULE_GENERATION_INFLIGHT_CALL,
            diagnostic, diagnostic_size))
        return false;
    if (cache_status != XR_VM_DECODED_CACHE_OK)
        return fail_decoded_cache(cache_status, diagnostic, diagnostic_size);
    if (status != XR_TYPED_DISPATCH_OK)
        return fail_typed_dispatch(status, diagnostic, diagnostic_size);
    *result = executed_result;
    return true;
}

XRAY_API bool xr_module_generation_begin_drain(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size) {
    if (!generation || !generation->authority)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "generation is missing");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    bool ok = transition_locked(generation, XR_MODULE_GENERATION_ACTIVE,
                                XR_MODULE_GENERATION_DRAINING,
                                "only an active generation may begin draining",
                                diagnostic, diagnostic_size);
    xr_mutex_unlock(&authority->gate);
    return ok && xr_runtime_dynamic_entry_retry_pending(
                     generation, diagnostic, diagnostic_size);
}

XRAY_API bool xr_module_generation_retire(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size) {
    if (!generation || !generation->authority)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "generation is missing");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    bool draining = generation->state == XR_MODULE_GENERATION_DRAINING;
    xr_mutex_unlock(&authority->gate);
    if (draining && !xr_runtime_dynamic_entry_retry_pending(
                        generation, diagnostic, diagnostic_size))
        return false;
    xr_mutex_lock(&authority->gate);
    if (generation->total_pins != 0) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "generation cannot retire while pins remain");
    }
    bool ok = transition_locked(generation, XR_MODULE_GENERATION_DRAINING,
                                XR_MODULE_GENERATION_RETIRED,
                                "only a drained generation may retire",
                                diagnostic, diagnostic_size);
    xr_mutex_unlock(&authority->gate);
    return ok;
}

XRAY_API bool xr_module_generation_poison(
    XrLoadedModuleGeneration *generation,
    const uint8_t diagnostic_fingerprint[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE],
    char *diagnostic, size_t diagnostic_size) {
    if (!generation || !generation->authority || !diagnostic_fingerprint ||
        bytes_are_zero(diagnostic_fingerprint,
                       XR_RUNTIME_GENERATION_FINGERPRINT_SIZE))
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5007",
                    "poisoning requires a nonzero stable diagnostic fingerprint");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    if (generation->state >= XR_MODULE_GENERATION_RETIRED ||
        (generation->poisoned &&
         memcmp(generation->poison_fingerprint, diagnostic_fingerprint,
                XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0)) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5007",
                    "generation poison identity is late or conflicting");
    }
    if (!generation->poisoned) {
        generation->poisoned = true;
        memcpy(generation->poison_fingerprint, diagnostic_fingerprint,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
        generation->revision++;
    }
    xr_mutex_unlock(&authority->gate);
    return true;
}

XRAY_API bool xr_module_generation_rollback(
    XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size) {
    if (!generation || !generation->authority)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "generation is missing");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    if (generation->state >= XR_MODULE_GENERATION_RETIRED) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "retired generation cannot roll back again");
    }
    generation->rollback_requested = true;
    if (generation->state == XR_MODULE_GENERATION_ACTIVE) {
        generation->state = XR_MODULE_GENERATION_DRAINING;
    } else if (generation->state < XR_MODULE_GENERATION_ACTIVE) {
        if (generation->total_pins != 0) {
            xr_mutex_unlock(&authority->gate);
            return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                        "pre-activation rollback found unexpected pins");
        }
        generation->state = XR_MODULE_GENERATION_RETIRED;
    }
    generation->revision++;
    xr_mutex_unlock(&authority->gate);
    return true;
}

XRAY_API bool xr_module_generation_pin_acquire(
    XrLoadedModuleGeneration *generation, XrModuleGenerationPinKind kind,
    char *diagnostic, size_t diagnostic_size) {
    if (!generation || !generation->authority ||
        kind >= XR_MODULE_GENERATION_PIN_KIND_COUNT)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "generation pin kind or owner is invalid");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    const XrRuntimeGenerationBudget *budget = &authority->budget;
    if (generation->state != XR_MODULE_GENERATION_ACTIVE ||
        generation->poisoned || generation->rollback_requested) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "new pins require a healthy active generation");
    }
    if (generation->total_pins >= budget->max_pins_per_generation ||
        generation->pins_by_kind[kind] >= budget->max_pins_by_kind[kind] ||
        authority->total_pins >= budget->max_total_pins) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5003",
                    "generation pin budget is exhausted");
    }
    generation->pins_by_kind[kind]++;
    generation->total_pins++;
    authority->total_pins++;
    generation->revision++;
    xr_mutex_unlock(&authority->gate);
    return true;
}

XRAY_API bool xr_module_generation_pin_release(
    XrLoadedModuleGeneration *generation, XrModuleGenerationPinKind kind,
    char *diagnostic, size_t diagnostic_size) {
    if (!generation || !generation->authority ||
        kind >= XR_MODULE_GENERATION_PIN_KIND_COUNT)
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "generation pin kind or owner is invalid");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    if ((generation->state != XR_MODULE_GENERATION_ACTIVE &&
         generation->state != XR_MODULE_GENERATION_DRAINING) ||
        generation->pins_by_kind[kind] == 0 || generation->total_pins == 0 ||
        authority->total_pins == 0) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_OWN_3003",
                    "generation pin release is unmatched or too late");
    }
    generation->pins_by_kind[kind]--;
    generation->total_pins--;
    authority->total_pins--;
    generation->revision++;
    xr_mutex_unlock(&authority->gate);
    return true;
}

XRAY_API bool xr_module_generation_snapshot(
    const XrLoadedModuleGeneration *generation,
    XrModuleGenerationSnapshot *snapshot) {
    if (!generation || !generation->authority || !snapshot)
        return false;
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->identity = generation->identity;
    snapshot->state = generation->state;
    snapshot->poisoned = generation->poisoned ? 1u : 0u;
    snapshot->rollback_requested =
        generation->rollback_requested ? 1u : 0u;
    snapshot->revision = generation->revision;
    snapshot->total_pins = generation->total_pins;
    memcpy(snapshot->pins_by_kind, generation->pins_by_kind,
           sizeof(snapshot->pins_by_kind));
    memcpy(snapshot->poison_fingerprint, generation->poison_fingerprint,
           sizeof(snapshot->poison_fingerprint));
    xr_mutex_unlock(&authority->gate);
    return true;
}

XRAY_API bool xr_module_generation_unload(
    XrLoadedModuleGeneration **generation, char *diagnostic,
    size_t diagnostic_size) {
    if (!generation || !*generation || !(*generation)->authority)
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5005",
                    "generation is missing");
    XrLoadedModuleGeneration *owned = *generation;
    XrRuntimeGenerationAuthority *authority = owned->authority;
    if (!xr_runtime_dynamic_entry_generation_is_quiescent(owned))
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "generation cannot unload while dynamic leases remain");
    xr_mutex_lock(&authority->gate);
    if (owned->state != XR_MODULE_GENERATION_RETIRED ||
        owned->total_pins != 0 || authority->live_generations == 0) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "only a zero-pin retired generation may unload");
    }
    xr_mutex_unlock(&authority->gate);
    if (owned->entry_cache &&
        !xr_runtime_dynamic_entry_cache_free(
            &owned->entry_cache, diagnostic, diagnostic_size))
        return false;
    xr_mutex_lock(&authority->gate);
    if (owned->state != XR_MODULE_GENERATION_RETIRED ||
        owned->total_pins != 0 || authority->live_generations == 0) {
        xr_mutex_unlock(&authority->gate);
        return fail(diagnostic, diagnostic_size, "XR_EXEC_5006",
                    "generation changed while its cache was released");
    }
    owned->state = XR_MODULE_GENERATION_UNLOADED;
    owned->revision++;
    authority->live_generations--;
    xr_mutex_unlock(&authority->gate);

    xr_typed_decoded_cache_free(owned->decoded_cache);
    xr_target_plan_free(owned->plan);
    memset(owned, 0, sizeof(*owned));
    xr_free(owned);
    *generation = NULL;
    return true;
}
