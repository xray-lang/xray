/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_decoded_cache.c - Immutable typed instruction decode cache
 *
 * KEY CONCEPT:
 *   Verification precedes every allocation and publication. Once built, all
 *   storage is read-only and owns a plan retain, so concurrent executors can
 *   share decoded rows until the generation that published them is unloaded.
 */

#include "xr_vm_decoded_cache.h"
#include "../base/xmalloc.h"
#include "../plan/target/xr_target_instruction_verify.h"
#include "../plan/target/xr_target_plan_internal.h"
#include "../plan/target/xr_target_verify.h"
#include <string.h>

XR_STATIC_ASSERT(XR_RUNTIME_GENERATION_CLOSURE_ID_SIZE == XR_STABLE_ID_BYTES,
                 "decoded cache GCI width must match stable identities");

typedef struct XrVmDecodedFunction {
    uint32_t instruction_begin;
    uint32_t instruction_count;
    uint32_t block_begin;
    uint32_t block_count;
    uint32_t parameter_count;
} XrVmDecodedFunction;

struct XrVmDecodedCache {
    XrTargetPlan *plan;
    XrFingerprint plan_fingerprint;
    XrFingerprint program_fingerprint;
    XrFingerprint program_module_set_fingerprint;
    XrStableId generation_closure_id;
    XrModuleGenerationIdentity generation_identity;
    XrTargetProgramGraphRecord program_graph;
    uint32_t plan_schema_version;
    uint32_t program_module_count;
    uint32_t function_count;
    uint32_t instruction_count;
    uint32_t block_count;
    size_t total_bytes;
    XrVmDecodedFunction *functions;
    XrVmDecodedInstruction *instructions;
    XrVmDecodedBlock *blocks;
    bool generation_bound;
};

static bool bytes_are_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static bool generation_identity_equal(const XrModuleGenerationIdentity *left,
                                      const XrModuleGenerationIdentity *right) {
    return left && right && left->schema_version == right->schema_version &&
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

static bool generation_identity_matches_plan(const XrModuleGenerationIdentity *identity,
                                             const XrTargetPlan *plan, XrFingerprint fingerprint,
                                             const XrTargetProgramGraphRecord *graph,
                                             uint32_t module_count) {
    const XrTargetProfile *profile = xr_target_plan_profile(plan);
    XrFingerprint semantic = xr_target_plan_semantic_fingerprint(plan);
    XrFingerprint profile_fingerprint =
        profile ? xr_target_profile_fingerprint(profile) : (XrFingerprint) {{0}};
    if (!identity || !profile || identity->schema_version != XR_RUNTIME_GENERATION_SCHEMA_VERSION ||
        identity->target_plan_schema_version != xr_target_plan_schema_version(plan) ||
        identity->generation_number == 0 ||
        identity->completed_family_mask != xr_target_plan_completed_family_mask(plan) ||
        memcmp(identity->semantic_fingerprint, semantic.bytes, sizeof(semantic.bytes)) != 0 ||
        memcmp(identity->target_profile_fingerprint, profile_fingerprint.bytes,
               sizeof(profile_fingerprint.bytes)) != 0 ||
        memcmp(identity->target_plan_fingerprint, fingerprint.bytes, sizeof(fingerprint.bytes)) !=
            0 ||
        bytes_are_zero(identity->generation_fingerprint, sizeof(identity->generation_fingerprint)))
        return false;
    if (!graph)
        return bytes_are_zero(identity->program_fingerprint,
                              sizeof(identity->program_fingerprint)) &&
               bytes_are_zero(identity->program_module_set_fingerprint,
                              sizeof(identity->program_module_set_fingerprint)) &&
               bytes_are_zero(identity->generation_closure_id,
                              sizeof(identity->generation_closure_id));
    XrFingerprint module_set_fingerprint = {{0}};
    return module_count == graph->module_count &&
           xr_target_plan_program_module_set_fingerprint(plan, &module_set_fingerprint) &&
           memcmp(identity->program_fingerprint, graph->program_fingerprint.bytes,
                  sizeof(identity->program_fingerprint)) == 0 &&
           memcmp(identity->program_module_set_fingerprint, module_set_fingerprint.bytes,
                  sizeof(identity->program_module_set_fingerprint)) == 0 &&
           memcmp(identity->generation_closure_id, graph->generation_identity.bytes,
                  sizeof(identity->generation_closure_id)) == 0;
}

static bool add_bytes(size_t *total, uint32_t count, size_t item_size) {
    if (count > (XR_VM_DECODED_CACHE_MAX_BYTES - *total) / item_size)
        return false;
    *total += (size_t) count * item_size;
    return true;
}

bool xr_typed_decoded_cache_size_within_budget(uint32_t function_count,
                                                uint32_t instruction_count,
                                                uint32_t block_count,
                                                size_t *total_bytes) {
    if (total_bytes)
        *total_bytes = 0;
    if (!total_bytes ||
        function_count > XR_VM_DECODED_CACHE_MAX_FUNCTIONS ||
        instruction_count > XR_VM_DECODED_CACHE_MAX_ROWS ||
        block_count > XR_VM_DECODED_CACHE_MAX_BLOCKS)
        return false;
    size_t bytes = sizeof(XrVmDecodedCache);
    if (!add_bytes(&bytes, function_count, sizeof(XrVmDecodedFunction)) ||
        !add_bytes(&bytes, instruction_count,
                   sizeof(XrVmDecodedInstruction)) ||
        !add_bytes(&bytes, block_count, sizeof(XrVmDecodedBlock)))
        return false;
    *total_bytes = bytes;
    return true;
}

static void dispose_storage(XrVmDecodedCache *cache) {
    if (!cache)
        return;
    xr_free(cache->blocks);
    xr_free(cache->instructions);
    xr_free(cache->functions);
    xr_target_plan_free(cache->plan);
    memset(cache, 0, sizeof(*cache));
    xr_free(cache);
}

static bool initialize_rows(XrVmDecodedCache *cache,
                            const XrTargetInstructionRecord *rows) {
    for (uint32_t i = 0; i < cache->instruction_count; i++) {
        XrVmDecodedInstruction *decoded = &cache->instructions[i];
        const XrTargetInstructionContract *contract =
            xr_target_instruction_contract(rows[i].opcode);
        if (!contract)
            return false;
        decoded->row = rows[i];
        decoded->contract = contract;
        decoded->block = XR_VM_DECODED_INDEX_NONE;
        decoded->target_if_zero = XR_VM_DECODED_INDEX_NONE;
        decoded->target_if_nonzero = XR_VM_DECODED_INDEX_NONE;
        switch ((XrTargetInstructionControlKind) contract->control_kind) {
            case XR_TARGET_INSTRUCTION_CONTROL_JUMP:
                decoded->target_if_nonzero =
                    XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(rows[i].immediate_bits);
                break;
            case XR_TARGET_INSTRUCTION_CONTROL_BRANCH:
                decoded->target_if_zero =
                    XR_TARGET_INSTRUCTION_TARGET_IF_ZERO(rows[i].immediate_bits);
                decoded->target_if_nonzero =
                    XR_TARGET_INSTRUCTION_TARGET_IF_NONZERO(rows[i].immediate_bits);
                break;
            case XR_TARGET_INSTRUCTION_CONTROL_NONE:
            case XR_TARGET_INSTRUCTION_CONTROL_RETURN:
            case XR_TARGET_INSTRUCTION_CONTROL_SUSPEND:
                break;
            default:
                return false;
        }
    }
    return true;
}

static uint32_t count_parameters(const XrVmDecodedInstruction *rows,
                                 uint32_t count) {
    uint32_t parameters = 0;
    for (uint32_t i = 0; i < count; i++)
        parameters += rows[i].contract->immediate_kind ==
                      XR_TARGET_INSTRUCTION_IMMEDIATE_PARAMETER_ORDINAL;
    return parameters;
}

static uint32_t count_blocks(const XrVmDecodedInstruction *rows,
                             uint32_t count) {
    if (!count)
        return 0;
    uint32_t blocks = 1;
    for (uint32_t i = 0; i + 1u < count; i++)
        blocks += rows[i].contract->terminator != 0;
    return blocks;
}

static bool index_functions(XrVmDecodedCache *cache) {
    uint32_t cursor = 0;
    uint32_t block_total = 0;
    for (uint32_t function = 0; function < cache->function_count; function++) {
        XrVmDecodedFunction *decoded = &cache->functions[function];
        decoded->instruction_begin = cursor;
        while (cursor < cache->instruction_count &&
               cache->instructions[cursor].row.function == function)
            cursor++;
        decoded->instruction_count = cursor - decoded->instruction_begin;
        const XrVmDecodedInstruction *rows =
            decoded->instruction_count
                ? &cache->instructions[decoded->instruction_begin]
                : NULL;
        decoded->parameter_count =
            count_parameters(rows, decoded->instruction_count);
        decoded->block_begin = block_total;
        decoded->block_count = count_blocks(rows, decoded->instruction_count);
        if (decoded->block_count > XR_TARGET_INSTRUCTION_MAX_BLOCKS ||
            decoded->block_count > XR_VM_DECODED_CACHE_MAX_BLOCKS - block_total)
            return false;
        block_total += decoded->block_count;
    }
    if (cursor != cache->instruction_count)
        return false;
    cache->block_count = block_total;
    return true;
}

static bool block_for_target(const XrVmDecodedBlock *blocks,
                             uint32_t block_count, uint32_t target,
                             uint32_t *out) {
    uint32_t low = 0;
    uint32_t high = block_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (blocks[middle].first_row < target)
            low = middle + 1u;
        else
            high = middle;
    }
    if (low >= block_count || blocks[low].first_row != target)
        return false;
    *out = low;
    return true;
}

static bool initialize_function_blocks(XrVmDecodedCache *cache,
                                       XrVmDecodedFunction *function) {
    if (!function->instruction_count)
        return function->block_count == 0;
    XrVmDecodedInstruction *rows =
        &cache->instructions[function->instruction_begin];
    XrVmDecodedBlock *blocks = &cache->blocks[function->block_begin];
    uint32_t block = 0;
    uint32_t first = 0;
    for (uint32_t i = 0; i < function->instruction_count; i++) {
        rows[i].block = block;
        if (!rows[i].contract->terminator)
            continue;
        if (block >= function->block_count)
            return false;
        blocks[block].first_row = first;
        blocks[block].row_count = i - first + 1u;
        first = i + 1u;
        block++;
    }
    if (block != function->block_count || first != function->instruction_count)
        return false;

    for (block = 0; block < function->block_count; block++) {
        XrVmDecodedBlock *metadata = &blocks[block];
        XrVmDecodedInstruction *terminator =
            &rows[metadata->first_row + metadata->row_count - 1u];
        metadata->successors[0] = XR_VM_DECODED_INDEX_NONE;
        metadata->successors[1] = XR_VM_DECODED_INDEX_NONE;
        switch ((XrTargetInstructionControlKind)
                    terminator->contract->control_kind) {
            case XR_TARGET_INSTRUCTION_CONTROL_RETURN:
                metadata->successor_count = 0;
                break;
            case XR_TARGET_INSTRUCTION_CONTROL_JUMP:
                metadata->successor_count = 1;
                if (!block_for_target(blocks, function->block_count,
                                      terminator->target_if_nonzero,
                                      &metadata->successors[0]))
                    return false;
                break;
            case XR_TARGET_INSTRUCTION_CONTROL_BRANCH:
                metadata->successor_count = 2;
                if (!block_for_target(blocks, function->block_count,
                                      terminator->target_if_zero,
                                      &metadata->successors[0]) ||
                    !block_for_target(blocks, function->block_count,
                                      terminator->target_if_nonzero,
                                      &metadata->successors[1]))
                    return false;
                break;
            case XR_TARGET_INSTRUCTION_CONTROL_SUSPEND: {
                uint32_t target_row =
                    XR_TARGET_INSTRUCTION_SUSPEND_RESUME(
                        terminator->row.immediate_bits);
                uint32_t target = 0;
                if (!block_for_target(blocks, function->block_count,
                                      target_row, &target))
                    return false;
                metadata->successor_count = 1;
                metadata->successors[0] = target;
                terminator->target_if_nonzero = blocks[target].first_row;
                break;
            }
            case XR_TARGET_INSTRUCTION_CONTROL_NONE:
            default:
                return false;
        }
    }
    return true;
}

XrVmDecodedCacheStatus xr_typed_decoded_cache_create(
    const XrTargetPlan *verified_plan, const XrFingerprint *required_plan_fingerprint,
    const XrModuleGenerationIdentity *generation_identity, XrVmDecodedCache **cache) {
    if (cache)
        *cache = NULL;
    if (!verified_plan || !required_plan_fingerprint || !cache)
        return XR_VM_DECODED_CACHE_INVALID_ARGUMENT;
    if (!xr_target_plan_is_verified(verified_plan))
        return XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED;
    if (xr_target_plan_schema_version(verified_plan) !=
            XR_TARGET_PLAN_SCHEMA_VERSION ||
        !xr_fingerprint_equal(xr_target_plan_fingerprint(verified_plan),
                              *required_plan_fingerprint))
        return XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH;
    uint32_t function_count = 0;
    uint32_t instruction_count = 0;
    xr_target_plan_functions(verified_plan, &function_count);
    const XrTargetInstructionRecord *rows =
        xr_target_plan_instructions(verified_plan, &instruction_count);
    if ((instruction_count && !rows))
        return XR_VM_DECODED_CACHE_BUDGET_EXCEEDED;

    size_t bytes = 0;
    if (!xr_typed_decoded_cache_size_within_budget(
            function_count, instruction_count, instruction_count, &bytes))
        return XR_VM_DECODED_CACHE_BUDGET_EXCEEDED;
    if (!xr_target_plan_fingerprint_is_intact(verified_plan))
        return XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED;
    char error[512] = {0};
    if (!xr_target_plan_verify(verified_plan, error, sizeof(error)))
        return XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED;
    if (!xr_target_instruction_program_verify(verified_plan, error,
                                              sizeof(error)))
        return XR_VM_DECODED_CACHE_PROGRAM_INVALID;
    uint32_t graph_count = 0;
    uint32_t partition_count = 0;
    const XrTargetProgramGraphRecord *graphs =
        xr_target_plan_program_graphs(verified_plan, &graph_count);
    const XrTargetModulePartitionRecord *partitions =
        xr_target_plan_module_partitions(verified_plan, &partition_count);
    const XrTargetProgramGraphRecord *graph = NULL;
    XrFingerprint module_set_fingerprint = {{0}};
    if (graph_count || partition_count) {
        if (!graphs || graph_count != 1u || !partitions || partition_count == 0u ||
            graphs[0].module_count != partition_count ||
            !xr_target_plan_program_module_set_fingerprint(verified_plan, &module_set_fingerprint))
            return XR_VM_DECODED_CACHE_PROGRAM_INVALID;
        graph = &graphs[0];
        if (!generation_identity_matches_plan(generation_identity, verified_plan,
                                              *required_plan_fingerprint, graph, partition_count))
            return XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH;
    } else if (generation_identity &&
               !generation_identity_matches_plan(generation_identity, verified_plan,
                                                 *required_plan_fingerprint, NULL, 0)) {
        return XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH;
    }
    XrVmDecodedCache *created =
        (XrVmDecodedCache *) xr_calloc(1, sizeof(*created));
    if (!created)
        return XR_VM_DECODED_CACHE_ALLOCATION_FAILED;
    created->function_count = function_count;
    created->instruction_count = instruction_count;
    created->plan_schema_version = xr_target_plan_schema_version(verified_plan);
    created->plan_fingerprint = xr_target_plan_fingerprint(verified_plan);
    created->program_module_set_fingerprint = module_set_fingerprint;
    if (graph) {
        created->program_fingerprint = graph->program_fingerprint;
        created->generation_closure_id = graph->generation_identity;
        created->program_graph = *graph;
        created->program_module_count = partition_count;
    }
    if (generation_identity) {
        created->generation_identity = *generation_identity;
        created->generation_bound = true;
    }
    created->plan = xr_target_plan_retain((XrTargetPlan *) verified_plan);
    if (!created->plan)
        goto allocation_failed;
    if (function_count) {
        created->functions = (XrVmDecodedFunction *) xr_calloc(
            function_count, sizeof(*created->functions));
        if (!created->functions)
            goto allocation_failed;
    }
    if (instruction_count) {
        created->instructions = (XrVmDecodedInstruction *) xr_calloc(
            instruction_count, sizeof(*created->instructions));
        if (!created->instructions)
            goto allocation_failed;
    }
    if (!initialize_rows(created, rows) || !index_functions(created))
        goto invalid_program;
    if (!xr_typed_decoded_cache_size_within_budget(
            function_count, instruction_count, created->block_count, &bytes))
        goto budget_exceeded;
    if (created->block_count) {
        created->blocks = (XrVmDecodedBlock *) xr_calloc(
            created->block_count, sizeof(*created->blocks));
        if (!created->blocks)
            goto allocation_failed;
    }
    for (uint32_t function = 0; function < created->function_count; function++)
        if (!initialize_function_blocks(created,
                                        &created->functions[function]))
            goto invalid_program;
    created->total_bytes = bytes;
    *cache = created;
    return XR_VM_DECODED_CACHE_OK;

budget_exceeded:
    dispose_storage(created);
    return XR_VM_DECODED_CACHE_BUDGET_EXCEEDED;
invalid_program:
    dispose_storage(created);
    return XR_VM_DECODED_CACHE_PROGRAM_INVALID;
allocation_failed:
    dispose_storage(created);
    return XR_VM_DECODED_CACHE_ALLOCATION_FAILED;
}

void xr_typed_decoded_cache_free(XrVmDecodedCache *cache) {
    dispose_storage(cache);
}

XrVmDecodedCacheStatus
xr_typed_decoded_cache_require_exact(const XrVmDecodedCache *cache,
                                     const XrTargetPlan *verified_plan,
                                     const XrFingerprint *required_plan_fingerprint,
                                     const XrModuleGenerationIdentity *generation_identity) {
    if (!cache || !verified_plan || !required_plan_fingerprint)
        return XR_VM_DECODED_CACHE_INVALID_ARGUMENT;
    if (!xr_target_plan_is_verified(verified_plan) ||
        !xr_target_plan_fingerprint_is_intact(verified_plan))
        return XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED;
    if (cache->plan != verified_plan ||
        cache->plan_schema_version != xr_target_plan_schema_version(verified_plan) ||
        !xr_fingerprint_equal(cache->plan_fingerprint,
                              xr_target_plan_fingerprint(verified_plan)) ||
        !xr_fingerprint_equal(cache->plan_fingerprint,
                              *required_plan_fingerprint))
        return XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH;
    if (cache->generation_bound != (generation_identity != NULL) ||
        (cache->generation_bound &&
         !generation_identity_equal(&cache->generation_identity, generation_identity)))
        return XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH;
    if (cache->program_module_count) {
        uint32_t graph_count = 0;
        uint32_t partition_count = 0;
        const XrTargetProgramGraphRecord *graphs =
            xr_target_plan_program_graphs(verified_plan, &graph_count);
        const XrTargetModulePartitionRecord *partitions =
            xr_target_plan_module_partitions(verified_plan, &partition_count);
        XrFingerprint module_set_fingerprint = {{0}};
        if (!graphs || graph_count != 1u || !partitions ||
            partition_count != cache->program_module_count ||
            graphs[0].module_count != partition_count ||
            !xr_target_plan_program_module_set_fingerprint(verified_plan,
                                                           &module_set_fingerprint) ||
            memcmp(&cache->program_graph, graphs, sizeof(cache->program_graph)) != 0 ||
            !xr_fingerprint_equal(cache->program_fingerprint, graphs[0].program_fingerprint) ||
            !xr_fingerprint_equal(cache->program_module_set_fingerprint, module_set_fingerprint) ||
            !xr_stable_id_equal(cache->generation_closure_id, graphs[0].generation_identity) ||
            !generation_identity_matches_plan(generation_identity, verified_plan,
                                              *required_plan_fingerprint, graphs, partition_count))
            return XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH;
    }
    return XR_VM_DECODED_CACHE_OK;
}

bool xr_typed_decoded_cache_function(const XrVmDecodedCache *cache,
                                     uint32_t function,
                                     XrVmDecodedFunctionView *view) {
    if (view)
        memset(view, 0, sizeof(*view));
    if (!cache || !view || function >= cache->function_count)
        return false;
    const XrVmDecodedFunction *decoded = &cache->functions[function];
    view->instructions = decoded->instruction_count
                             ? &cache->instructions[decoded->instruction_begin]
                             : NULL;
    view->blocks = decoded->block_count
                       ? &cache->blocks[decoded->block_begin]
                       : NULL;
    view->instruction_count = decoded->instruction_count;
    view->block_count = decoded->block_count;
    view->parameter_count = decoded->parameter_count;
    return true;
}

bool xr_typed_decoded_cache_stats(const XrVmDecodedCache *cache,
                                  XrVmDecodedCacheStats *stats) {
    if (!cache || !stats)
        return false;
    *stats = (XrVmDecodedCacheStats) {
        .plan_schema_version = cache->plan_schema_version,
        .function_count = cache->function_count,
        .instruction_count = cache->instruction_count,
        .block_count = cache->block_count,
        .total_bytes = cache->total_bytes,
        .plan_fingerprint = cache->plan_fingerprint,
        .program_fingerprint = cache->program_fingerprint,
        .program_module_set_fingerprint = cache->program_module_set_fingerprint,
        .generation_closure_id = cache->generation_closure_id,
        .runtime_generation_number =
            cache->generation_bound ? cache->generation_identity.generation_number : 0,
        .program_module_count = cache->program_module_count,
        .generation_bound = cache->generation_bound ? 1u : 0u,
    };
    return true;
}
