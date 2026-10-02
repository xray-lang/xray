/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_resources.h - Shared cumulative compiler allocation and work limits
 *
 * KEY CONCEPT:
 *   Every allocation retains its ledger until physical release, including
 *   allocations transferred beyond the lifetime of the producing scope.
 */
#ifndef XCOMPILE_RESOURCES_H
#define XCOMPILE_RESOURCES_H

#include "xdefs.h"

typedef struct XrCompileResources XrCompileResources;

typedef enum XrCompileResourceStatus {
    XR_COMPILE_RESOURCE_OK,
    XR_COMPILE_RESOURCE_BAD_ARGUMENT,
    XR_COMPILE_RESOURCE_BUDGET,
    XR_COMPILE_RESOURCE_OUT_OF_MEMORY
} XrCompileResourceStatus;

typedef struct XrCompileResourceLimits {
    uint64_t allocated_bytes, live_bytes, work;
} XrCompileResourceLimits;

typedef struct XrCompileResourceStats {
    uint64_t allocation_count, allocated_bytes, live_bytes, peak_bytes, work;
} XrCompileResourceStats;

/* Zero limits grant no resources. The ledger allocation itself is charged.
 * Concurrent calls require a live reference throughout each call, including
 * while waiting. Publish references with caller synchronization; retain cannot
 * revive a pointer racing its final release. Blocks also retain their ledger.
 * Payload access, a block's resize/free, and output slots require caller exclusion.
 * Accounting and physical allocation/free are serialized internally; allocator
 * callbacks must not re-enter this ledger. No fairness or lock-free guarantee. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(
    const XrCompileResourceLimits *limits, XrCompileResources **output);
XR_FUNC XrCompileResourceStatus xr_compile_resources_retain(XrCompileResources *resources);
XR_FUNC void xr_compile_resources_release(XrCompileResources *resources);
XR_FUNC XrCompileResourceStatus xr_compile_resources_stats(
    const XrCompileResources *resources, XrCompileResourceStats *output);
/* Charge actual operations before starting them. Failed stages never refund work. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources, uint64_t units);

/* Outputs must be empty and payload sizes nonzero. Failure preserves output.
 * Actual allocator calls consume one work unit, including failed calls.
 * Zeroing additionally consumes payload bytes. Headers and alignment padding
 * count as allocation bytes, but are not fictitious algorithm work. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(
    XrCompileResources *resources, size_t bytes, void **output);
XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(
    XrCompileResources *resources, size_t count, size_t size, void **output);
/* A non-NULL block must belong to this ledger. A size change allocates the new
 * block before freeing the old block and charges actual copied bytes as work.
 * Zero frees the block; equal sizes do nothing. Failure preserves the old block. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_resize(
    XrCompileResources *resources, void **memory, size_t bytes);
/* Only this allocator's starting pointers are valid; NULL is a no-op.
 * Blocks retain the ledger without requiring an additional caller retain.
 * The payload supports max_align_t alignment, not over-aligned objects. */
XR_FUNC void xr_compile_resources_free(void *memory);

#endif // XCOMPILE_RESOURCES_H
