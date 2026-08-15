/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_vm_decoded_cache.h - Immutable typed instruction decode cache
 *
 * KEY CONCEPT:
 *   A cache is derived only after the complete instruction verifier accepts an
 *   immutable TargetPlan. It records generated opcode contract facts and the
 *   verified block partition; it never selects representations, calls, or
 *   ownership policy.
 */

#ifndef XR_VM_DECODED_CACHE_H
#define XR_VM_DECODED_CACHE_H

#include "../plan/target/xr_target_plan.h"

#define XR_VM_DECODED_CACHE_MAX_FUNCTIONS UINT32_C(100000)
#define XR_VM_DECODED_CACHE_MAX_ROWS UINT32_C(4194304)
#define XR_VM_DECODED_CACHE_MAX_BLOCKS UINT32_C(4194304)
#define XR_VM_DECODED_CACHE_MAX_BYTES ((size_t) 256u * 1024u * 1024u)
#define XR_VM_DECODED_INDEX_NONE UINT32_MAX

typedef enum XrVmDecodedCacheStatus {
    XR_VM_DECODED_CACHE_OK = 0,
    XR_VM_DECODED_CACHE_INVALID_ARGUMENT,
    XR_VM_DECODED_CACHE_PLAN_NOT_VERIFIED,
    XR_VM_DECODED_CACHE_PLAN_IDENTITY_MISMATCH,
    XR_VM_DECODED_CACHE_PROGRAM_INVALID,
    XR_VM_DECODED_CACHE_BUDGET_EXCEEDED,
    XR_VM_DECODED_CACHE_ALLOCATION_FAILED,
} XrVmDecodedCacheStatus;

typedef struct XrVmDecodedInstruction {
    XrTargetInstructionRecord row;
    const XrTargetInstructionContract *contract;
    uint32_t block;
    uint32_t target_if_zero;
    uint32_t target_if_nonzero;
} XrVmDecodedInstruction;

typedef struct XrVmDecodedBlock {
    uint32_t first_row;
    uint32_t row_count;
    uint32_t successors[2];
    uint8_t successor_count;
    uint8_t reserved[3];
} XrVmDecodedBlock;

typedef struct XrVmDecodedFunctionView {
    const XrVmDecodedInstruction *instructions;
    const XrVmDecodedBlock *blocks;
    uint32_t instruction_count;
    uint32_t block_count;
    uint32_t parameter_count;
} XrVmDecodedFunctionView;

typedef struct XrVmDecodedCacheStats {
    uint32_t plan_schema_version;
    uint32_t function_count;
    uint32_t instruction_count;
    uint32_t block_count;
    size_t total_bytes;
    XrFingerprint plan_fingerprint;
} XrVmDecodedCacheStats;

typedef struct XrVmDecodedCache XrVmDecodedCache;

XR_FUNC XrVmDecodedCacheStatus xr_typed_decoded_cache_create(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint,
    XrVmDecodedCache **cache);
XR_FUNC void xr_typed_decoded_cache_free(XrVmDecodedCache *cache);
XR_FUNC XrVmDecodedCacheStatus xr_typed_decoded_cache_require_exact(
    const XrVmDecodedCache *cache, const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint);
XR_FUNC bool xr_typed_decoded_cache_function(
    const XrVmDecodedCache *cache, uint32_t function,
    XrVmDecodedFunctionView *view);
XR_FUNC bool xr_typed_decoded_cache_stats(const XrVmDecodedCache *cache,
                                          XrVmDecodedCacheStats *stats);
XR_FUNC bool xr_typed_decoded_cache_size_within_budget(
    uint32_t function_count, uint32_t instruction_count,
    uint32_t block_count, size_t *total_bytes);

#endif  // XR_VM_DECODED_CACHE_H
