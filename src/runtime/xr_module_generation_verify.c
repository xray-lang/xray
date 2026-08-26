/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_generation_verify.c - Independent generation state verifier
 *
 * KEY CONCEPT:
 *   The verifier re-derives plan and native-runtime identity and separately
 *   models each legal state mutation. It does not call the production
 *   transition helper, so a shared state-machine defect fails closed.
 */

#include "xr_module_generation_internal.h"
#include "../base/xsha256.h"
#include "../plan/semantic/xr_semantic_ids.h"
#include "../plan/target/xr_target_instruction_verify.h"
#include "../plan/target/xr_target_capability.h"
#include "../plan/target/xr_target_plan.h"
#include "../plan/target/xr_target_plan_internal.h"
#include "../plan/target/xr_target_profile_internal.h"
#include "../plan/target/xr_target_verify.h"
#include "../vm/xr_typed_frame.h"
#include "abi/xr_runtime_target_authority.h"
#include <stdio.h>
#include <string.h>

XR_STATIC_ASSERT(XR_RUNTIME_GENERATION_CLOSURE_ID_SIZE == XR_STABLE_ID_BYTES,
                 "runtime verifier GCI width must match stable identities");

static bool reject(char *diagnostic, size_t diagnostic_size,
                   const char *code, const char *detail) {
    if (diagnostic && diagnostic_size)
        snprintf(diagnostic, diagnostic_size, "%s: %s", code, detail);
    return false;
}

static bool nonzero_bytes(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined != 0;
}

static void verifier_hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void verifier_hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void verifier_generation_fingerprint(
    const XrModuleGenerationIdentity *identity,
    uint8_t out[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE]) {
    static const uint8_t domain[] = "xray-module-generation-v3\0";
    XrSHA256Context context;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    verifier_hash_u32(&context, identity->schema_version);
    verifier_hash_u32(&context, identity->target_plan_schema_version);
    verifier_hash_u64(&context, identity->generation_number);
    verifier_hash_u64(&context, identity->completed_family_mask);
    verifier_hash_u64(&context, identity->required_capability_mask);
    xr_sha256_update(&context, identity->semantic_fingerprint,
                     XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->program_fingerprint,
                     XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->program_module_set_fingerprint,
                     XR_RUNTIME_GENERATION_FINGERPRINT_SIZE);
    xr_sha256_update(&context, identity->generation_closure_id,
                     sizeof(identity->generation_closure_id));
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
    xr_sha256_final(&context, out);
}

static bool snapshot_shape_valid(const XrModuleGenerationSnapshot *snapshot) {
    if (!snapshot || snapshot->state >= XR_MODULE_GENERATION_STATE_COUNT ||
        snapshot->poisoned > 1 || snapshot->rollback_requested > 1 ||
        snapshot->reserved != 0 || snapshot->revision == 0 ||
        snapshot->identity.schema_version !=
            XR_RUNTIME_GENERATION_SCHEMA_VERSION ||
        snapshot->identity.generation_number == 0 ||
        !nonzero_bytes(snapshot->identity.generation_fingerprint,
                       XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) ||
        snapshot->poisoned !=
            (nonzero_bytes(snapshot->poison_fingerprint,
                           XR_RUNTIME_GENERATION_FINGERPRINT_SIZE)
                 ? 1u
                 : 0u))
        return false;
    uint64_t total = 0;
    for (uint32_t i = 0; i < XR_MODULE_GENERATION_PIN_KIND_COUNT; i++)
        total += snapshot->pins_by_kind[i];
    if (total != snapshot->total_pins)
        return false;
    if ((snapshot->state < XR_MODULE_GENERATION_ACTIVE ||
         snapshot->state >= XR_MODULE_GENERATION_RETIRED) &&
        snapshot->total_pins != 0)
        return false;
    if (snapshot->rollback_requested &&
        (snapshot->state == XR_MODULE_GENERATION_READY ||
         snapshot->state == XR_MODULE_GENERATION_ACTIVE))
        return false;
    return true;
}

static void locked_snapshot(const XrLoadedModuleGeneration *generation,
                            XrModuleGenerationSnapshot *snapshot) {
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
}

static bool verify_native_identity(const XrLoadedModuleGeneration *generation,
                                   char *diagnostic,
                                   size_t diagnostic_size) {
    const XrTargetPlan *plan = generation->plan;
    const XrModuleGenerationIdentity *identity = &generation->identity;
    char nested[512] = {0};
    if (!plan || !xr_target_plan_is_verified(plan) ||
        !xr_target_plan_verify(plan, nested, sizeof(nested)))
        return reject(diagnostic, diagnostic_size, "XR_EXEC_5000",
                      "generation retained an unverified TargetPlan");
    const XrTargetProfile *profile = xr_target_plan_profile(plan);
    const XrTargetProfileDraft *facts = xr_target_profile_facts(profile);
    XrRuntimeTargetAuthority runtime;
    XrRuntimeObjectHeaderAbi object_header;
    XrFingerprint runtime_fingerprint;
    XrFingerprint provider_fingerprint;
    XrFingerprint object_fingerprint;
    uint64_t provider_mask = 0;
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
            XR_RUNTIME_ABI_OK)
        return reject(diagnostic, diagnostic_size, "XR_TARGET_1000",
                      "generation native runtime identity is unavailable");
    uint8_t actual_generation[XR_RUNTIME_GENERATION_FINGERPRINT_SIZE];
    verifier_generation_fingerprint(identity, actual_generation);
    uint32_t graph_count = 0;
    uint32_t partition_count = 0;
    const XrTargetProgramGraphRecord *graphs = xr_target_plan_program_graphs(plan, &graph_count);
    (void) xr_target_plan_module_partitions(plan, &partition_count);
    bool program_identity_exact = false;
    if (graph_count == 0 && partition_count == 0) {
        program_identity_exact =
            !nonzero_bytes(identity->program_fingerprint, sizeof(identity->program_fingerprint)) &&
            !nonzero_bytes(identity->program_module_set_fingerprint,
                           sizeof(identity->program_module_set_fingerprint)) &&
            !nonzero_bytes(identity->generation_closure_id,
                           sizeof(identity->generation_closure_id));
    } else if (graphs && graph_count == 1u && partition_count == 2u &&
               graphs[0].module_count == partition_count) {
        XrFingerprint module_set_fingerprint = {{0}};
        program_identity_exact =
            xr_target_plan_program_module_set_fingerprint(plan, &module_set_fingerprint) &&
            memcmp(identity->program_fingerprint, graphs[0].program_fingerprint.bytes,
                   sizeof(identity->program_fingerprint)) == 0 &&
            memcmp(identity->program_module_set_fingerprint, module_set_fingerprint.bytes,
                   sizeof(identity->program_module_set_fingerprint)) == 0 &&
            memcmp(identity->generation_closure_id, graphs[0].generation_identity.bytes,
                   sizeof(identity->generation_closure_id)) == 0;
    }
    uint64_t expected_capability_mask = 0;
    if (!xr_target_semantic_capability_mask(
            xr_target_plan_semantic_plan(plan),
            facts->machine.runtime_profile,
            &expected_capability_mask))
        return reject(diagnostic, diagnostic_size, "XR_TARGET_1004",
                      "generation semantic capability closure is invalid");
    if (!program_identity_exact ||
        identity->target_plan_schema_version != xr_target_plan_schema_version(plan) ||
        identity->completed_family_mask != xr_target_plan_completed_family_mask(plan) ||
        identity->required_capability_mask != expected_capability_mask ||
        !xr_target_capability_mask_is_backed(identity->required_capability_mask, provider_mask) ||
        memcmp(identity->semantic_fingerprint, xr_target_plan_semantic_fingerprint(plan).bytes,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0 ||
        memcmp(identity->target_profile_fingerprint, xr_target_profile_fingerprint(profile).bytes,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0 ||
        memcmp(identity->target_plan_fingerprint, xr_target_plan_fingerprint(plan).bytes,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0 ||
        memcmp(identity->runtime_abi_fingerprint, runtime_fingerprint.bytes,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0 ||
        memcmp(identity->provider_set_fingerprint, provider_fingerprint.bytes,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0 ||
        memcmp(identity->object_header_fingerprint, object_fingerprint.bytes,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0 ||
        memcmp(identity->generation_fingerprint, actual_generation,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0)
        return reject(diagnostic, diagnostic_size, "XR_EXEC_5008",
                      "generation identity does not exactly match its verified plan");
    return true;
}

static bool verifier_has_no_non_scalar_execution_authority(const XrTargetPlan *plan,
                                                           uint32_t expected_call_count,
                                                           uint32_t expected_argument_count) {
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
    if (count != expected_call_count)
        return false;
    xr_target_plan_call_arguments(plan, &count);
    if (count != expected_argument_count)
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

static bool verifier_typed_generation_eligible(const XrLoadedModuleGeneration *generation) {
    char nested[512] = {0};
    const XrTargetPlan *plan = generation ? generation->plan : NULL;
    if (!plan || !xr_target_plan_is_verified(plan) ||
        !xr_target_plan_fingerprint_is_intact(plan) ||
        !xr_target_plan_verify(plan, nested, sizeof(nested)) ||
        xr_target_plan_schema_version(plan) !=
            XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION ||
        xr_target_plan_completed_family_mask(plan) !=
            XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK ||
        memcmp(generation->identity.target_plan_fingerprint,
               xr_target_plan_fingerprint(plan).bytes,
               XR_RUNTIME_GENERATION_FINGERPRINT_SIZE) != 0 ||
        !xr_target_instruction_program_verify(plan, nested, sizeof(nested)))
        return false;

    uint32_t graph_count = 0;
    uint32_t partition_count = 0;
    const XrTargetProgramGraphRecord *graphs = xr_target_plan_program_graphs(plan, &graph_count);
    const XrTargetModulePartitionRecord *partitions =
        xr_target_plan_module_partitions(plan, &partition_count);
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(plan, &function_count);
    if (graph_count == 0 && partition_count == 0) {
        uint32_t instruction_count = 0;
        const XrTargetInstructionRecord *instructions =
            xr_target_plan_instructions(plan, &instruction_count);
        uint32_t sole_instruction_count = 0;
        const XrTargetInstructionRecord *sole_instructions =
            xr_target_plan_function_instructions(plan, 0u, &sole_instruction_count);
        return functions && function_count == 1u && functions[0].id == 0u &&
               functions[0].semantic_function == 0u && functions[0].root_count == 0u &&
               functions[0].cleanup_count == 0u && functions[0].coroutine_count == 0u &&
               instructions && sole_instructions && instruction_count != 0u &&
               instruction_count == sole_instruction_count &&
               xr_target_plan_function_execution_family_mask(plan, 0u) ==
                   XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
               verifier_has_no_non_scalar_execution_authority(plan, 0u, 0u);
    }
    if (!graphs || graph_count != 1u || !partitions || partition_count != 2u ||
        graphs[0].module_count != 2u || graphs[0].function_count != 2u ||
        graphs[0].call_count != 1u || graphs[0].argument_count != 1u ||
        graphs[0].flags !=
            (XR_TARGET_PROGRAM_GRAPH_SINGLE_PLAN | XR_TARGET_PROGRAM_GRAPH_DIRECT_I64) ||
        !functions || function_count < 2u || graphs[0].entry_target_function >= function_count ||
        graphs[0].producer_target_function >= function_count)
        return false;
    uint32_t call_count = 0;
    uint32_t expectation_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    (void) xr_target_plan_entry_expectations(plan, &expectation_count);
    bool only_graph_functions_execute = true;
    for (uint32_t i = 0; i < function_count; i++) {
        uint64_t family = xr_target_plan_function_execution_family_mask(plan, i);
        if (i == graphs[0].entry_target_function || i == graphs[0].producer_target_function)
            only_graph_functions_execute =
                only_graph_functions_execute && family == XR_TARGET_EXECUTION_SCALAR_I64_CLOSED;
        else
            only_graph_functions_execute = only_graph_functions_execute && family == 0u;
    }
    return only_graph_functions_execute && calls && call_count == 1u && expectation_count == 0u &&
           graphs[0].target_call == 0u &&
           calls[0].calling_convention == XR_TARGET_CALL_CONVENTION_PROGRAM_DIRECT &&
           calls[0].target_kind == XR_TARGET_CALL_TARGET_PROGRAM_DIRECT &&
           calls[0].callee_function == graphs[0].producer_target_function &&
           xr_target_plan_function_execution_family_mask(plan, graphs[0].entry_target_function) ==
               XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
           xr_target_plan_function_execution_family_mask(
               plan, graphs[0].producer_target_function) == XR_TARGET_EXECUTION_SCALAR_I64_CLOSED &&
           verifier_has_no_non_scalar_execution_authority(plan, 1u, 1u);
}

XRAY_API bool xr_module_generation_verify(
    const XrLoadedModuleGeneration *generation, char *diagnostic,
    size_t diagnostic_size) {
    if (!generation || !generation->authority)
        return reject(diagnostic, diagnostic_size, "XR_EXEC_5008",
                      "generation authority is missing");
    XrRuntimeGenerationAuthority *authority = generation->authority;
    xr_mutex_lock(&authority->gate);
    XrModuleGenerationSnapshot snapshot;
    locked_snapshot(generation, &snapshot);
    bool shape_ok = snapshot_shape_valid(&snapshot);
    bool budget_ok = snapshot.total_pins <=
                         authority->budget.max_pins_per_generation &&
                     authority->total_pins <=
                         authority->budget.max_total_pins &&
                     generation->identity.generation_number <
                         authority->next_generation;
    for (uint32_t i = 0; i < XR_MODULE_GENERATION_PIN_KIND_COUNT; i++)
        budget_ok = budget_ok &&
                    snapshot.pins_by_kind[i] <=
                        authority->budget.max_pins_by_kind[i];
    bool identity_ok = shape_ok && budget_ok &&
                       verify_native_identity(generation, diagnostic,
                                              diagnostic_size);
    bool unavailable_state =
        snapshot.state >= XR_MODULE_GENERATION_READY &&
        snapshot.state <= XR_MODULE_GENERATION_DRAINING &&
        (!verifier_typed_generation_eligible(generation) || !generation->decoded_cache);
    bool cache_identity_ok = true;
    if (generation->decoded_cache) {
        XrFingerprint fingerprint = {{0}};
        memcpy(fingerprint.bytes,
               generation->identity.target_plan_fingerprint,
               sizeof(fingerprint.bytes));
        cache_identity_ok = xr_typed_decoded_cache_require_exact(
                                generation->decoded_cache, generation->plan, &fingerprint,
                                &generation->identity) == XR_VM_DECODED_CACHE_OK;
    }
    xr_mutex_unlock(&authority->gate);
    if (!shape_ok || !budget_ok)
        return reject(diagnostic, diagnostic_size, "XR_EXEC_5008",
                      "generation state or counter invariant is invalid");
    if (!identity_ok)
        return false;
    if (!cache_identity_ok)
        return reject(diagnostic, diagnostic_size, "XR_EXEC_5008",
                      "generation decoded cache identity is not exact");
    if (unavailable_state)
        return reject(diagnostic, diagnostic_size, "XR_EXEC_5004",
                      "generation claims a state owned by an unavailable typed executor");
    return true;
}

static bool identity_equal(const XrModuleGenerationSnapshot *before,
                           const XrModuleGenerationSnapshot *after) {
    const XrModuleGenerationIdentity *left = &before->identity;
    const XrModuleGenerationIdentity *right = &after->identity;
    return left->schema_version == right->schema_version &&
           left->target_plan_schema_version == right->target_plan_schema_version &&
           left->generation_number == right->generation_number &&
           left->completed_family_mask == right->completed_family_mask &&
           left->required_capability_mask == right->required_capability_mask &&
           memcmp(left->semantic_fingerprint, right->semantic_fingerprint,
                  sizeof(left->semantic_fingerprint)) == 0 &&
           memcmp(left->program_fingerprint, right->program_fingerprint,
                  sizeof(left->program_fingerprint)) == 0 &&
           memcmp(left->program_module_set_fingerprint, right->program_module_set_fingerprint,
                  sizeof(left->program_module_set_fingerprint)) == 0 &&
           memcmp(left->generation_closure_id, right->generation_closure_id,
                  sizeof(left->generation_closure_id)) == 0 &&
           memcmp(left->target_profile_fingerprint, right->target_profile_fingerprint,
                  sizeof(left->target_profile_fingerprint)) == 0 &&
           memcmp(left->target_plan_fingerprint, right->target_plan_fingerprint,
                  sizeof(left->target_plan_fingerprint)) == 0 &&
           memcmp(left->runtime_abi_fingerprint, right->runtime_abi_fingerprint,
                  sizeof(left->runtime_abi_fingerprint)) == 0 &&
           memcmp(left->provider_set_fingerprint, right->provider_set_fingerprint,
                  sizeof(left->provider_set_fingerprint)) == 0 &&
           memcmp(left->object_header_fingerprint, right->object_header_fingerprint,
                  sizeof(left->object_header_fingerprint)) == 0 &&
           memcmp(left->generation_fingerprint, right->generation_fingerprint,
                  sizeof(left->generation_fingerprint)) == 0;
}

static bool pins_equal(const XrModuleGenerationSnapshot *before,
                       const XrModuleGenerationSnapshot *after) {
    return before->total_pins == after->total_pins &&
           memcmp(before->pins_by_kind, after->pins_by_kind,
                  sizeof(before->pins_by_kind)) == 0;
}

static bool flags_equal(const XrModuleGenerationSnapshot *before,
                        const XrModuleGenerationSnapshot *after) {
    return before->poisoned == after->poisoned &&
           before->rollback_requested == after->rollback_requested &&
           memcmp(before->poison_fingerprint, after->poison_fingerprint,
                  sizeof(before->poison_fingerprint)) == 0;
}

XR_FUNCDEF bool xr_module_generation_verify_transition(
    const XrModuleGenerationSnapshot *before,
    const XrModuleGenerationSnapshot *after,
    XrModuleGenerationMutation mutation, XrModuleGenerationPinKind pin_kind,
    char *diagnostic, size_t diagnostic_size) {
    if (!snapshot_shape_valid(before) || !snapshot_shape_valid(after) ||
        !identity_equal(before, after) ||
        before->revision == UINT64_MAX ||
        after->revision != before->revision + 1u)
        return reject(diagnostic, diagnostic_size, "XR_EXEC_5005",
                      "generation mutation changed immutable identity or revision");
    bool legal = false;
    switch (mutation) {
        case XR_MODULE_GENERATION_MUTATION_VERIFY:
            legal = before->state == XR_MODULE_GENERATION_LOADING &&
                    after->state == XR_MODULE_GENERATION_VERIFIED &&
                    pins_equal(before, after) && flags_equal(before, after);
            break;
        case XR_MODULE_GENERATION_MUTATION_PREPARE:
            legal = before->state == XR_MODULE_GENERATION_VERIFIED &&
                    after->state == XR_MODULE_GENERATION_READY &&
                    !before->poisoned && !before->rollback_requested &&
                    pins_equal(before, after) && flags_equal(before, after);
            break;
        case XR_MODULE_GENERATION_MUTATION_ACTIVATE:
            legal = before->state == XR_MODULE_GENERATION_READY &&
                    after->state == XR_MODULE_GENERATION_ACTIVE &&
                    !before->poisoned && !before->rollback_requested &&
                    pins_equal(before, after) && flags_equal(before, after);
            break;
        case XR_MODULE_GENERATION_MUTATION_BEGIN_DRAIN:
            legal = before->state == XR_MODULE_GENERATION_ACTIVE &&
                    after->state == XR_MODULE_GENERATION_DRAINING &&
                    pins_equal(before, after) && flags_equal(before, after);
            break;
        case XR_MODULE_GENERATION_MUTATION_RETIRE:
            legal = before->state == XR_MODULE_GENERATION_DRAINING &&
                    after->state == XR_MODULE_GENERATION_RETIRED &&
                    before->total_pins == 0 && pins_equal(before, after) &&
                    flags_equal(before, after);
            break;
        case XR_MODULE_GENERATION_MUTATION_POISON:
            legal = before->state < XR_MODULE_GENERATION_RETIRED &&
                    after->state == before->state && !before->poisoned &&
                    after->poisoned &&
                    after->rollback_requested == before->rollback_requested &&
                    nonzero_bytes(after->poison_fingerprint,
                                  sizeof(after->poison_fingerprint)) &&
                    pins_equal(before, after);
            break;
        case XR_MODULE_GENERATION_MUTATION_ROLLBACK: {
            uint32_t expected = before->state;
            if (before->state < XR_MODULE_GENERATION_ACTIVE)
                expected = XR_MODULE_GENERATION_RETIRED;
            else if (before->state == XR_MODULE_GENERATION_ACTIVE)
                expected = XR_MODULE_GENERATION_DRAINING;
            legal = before->state < XR_MODULE_GENERATION_RETIRED &&
                    after->state == expected &&
                    after->rollback_requested &&
                    after->poisoned == before->poisoned &&
                    memcmp(after->poison_fingerprint,
                           before->poison_fingerprint,
                           sizeof(after->poison_fingerprint)) == 0 &&
                    pins_equal(before, after) &&
                    (before->state >= XR_MODULE_GENERATION_ACTIVE ||
                     before->total_pins == 0);
            break;
        }
        case XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE:
        case XR_MODULE_GENERATION_MUTATION_PIN_RELEASE: {
            if (pin_kind >= XR_MODULE_GENERATION_PIN_KIND_COUNT)
                break;
            int delta = mutation == XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE
                            ? 1
                            : -1;
            bool other_equal = true;
            for (uint32_t i = 0; i < XR_MODULE_GENERATION_PIN_KIND_COUNT; i++) {
                if (i != (uint32_t) pin_kind &&
                    before->pins_by_kind[i] != after->pins_by_kind[i])
                    other_equal = false;
            }
            legal = after->state == before->state && flags_equal(before, after) &&
                    other_equal &&
                    (int64_t) after->total_pins ==
                        (int64_t) before->total_pins + delta &&
                    (int64_t) after->pins_by_kind[pin_kind] ==
                        (int64_t) before->pins_by_kind[pin_kind] + delta;
            if (mutation == XR_MODULE_GENERATION_MUTATION_PIN_ACQUIRE)
                legal = legal && before->state == XR_MODULE_GENERATION_ACTIVE &&
                        !before->poisoned && !before->rollback_requested;
            else
                legal = legal &&
                        (before->state == XR_MODULE_GENERATION_ACTIVE ||
                         before->state == XR_MODULE_GENERATION_DRAINING) &&
                        before->pins_by_kind[pin_kind] != 0;
            break;
        }
        case XR_MODULE_GENERATION_MUTATION_UNLOAD:
            legal = before->state == XR_MODULE_GENERATION_RETIRED &&
                    after->state == XR_MODULE_GENERATION_UNLOADED &&
                    before->total_pins == 0 && pins_equal(before, after) &&
                    flags_equal(before, after);
            break;
    }
    if (!legal)
        return reject(diagnostic, diagnostic_size, "XR_EXEC_5005",
                      "generation state-machine mutation is illegal");
    return true;
}
