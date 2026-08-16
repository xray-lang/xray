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
#include "../plan/target/xr_target_verify.h"
#include <string.h>

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
    uint32_t plan_schema_version;
    uint32_t function_count;
    uint32_t instruction_count;
    uint32_t block_count;
    size_t total_bytes;
    XrVmDecodedFunction *functions;
    XrVmDecodedInstruction *instructions;
    XrVmDecodedBlock *blocks;
};

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
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint,
    XrVmDecodedCache **cache) {
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
    XrVmDecodedCache *created =
        (XrVmDecodedCache *) xr_calloc(1, sizeof(*created));
    if (!created)
        return XR_VM_DECODED_CACHE_ALLOCATION_FAILED;
    created->function_count = function_count;
    created->instruction_count = instruction_count;
    created->plan_schema_version = xr_target_plan_schema_version(verified_plan);
    created->plan_fingerprint = xr_target_plan_fingerprint(verified_plan);
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

XrVmDecodedCacheStatus xr_typed_decoded_cache_require_exact(
    const XrVmDecodedCache *cache, const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint) {
    if (!cache || !verified_plan || !required_plan_fingerprint)
        return XR_VM_DECODED_CACHE_INVALID_ARGUMENT;
    if (!xr_target_plan_is_verified(verified_plan))
        return XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED;
    if (cache->plan != verified_plan ||
        cache->plan_schema_version != xr_target_plan_schema_version(verified_plan) ||
        !xr_fingerprint_equal(cache->plan_fingerprint,
                              xr_target_plan_fingerprint(verified_plan)) ||
        !xr_fingerprint_equal(cache->plan_fingerprint,
                              *required_plan_fingerprint))
        return XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH;
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
    };
    return true;
}
