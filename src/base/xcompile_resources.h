/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_resources.h - Owned compiler memory and explicit finite admission
 *
 * KEY CONCEPT:
 *   Live blocks retain their memory owner independently of request state.
 *   Limits cover held allocation bytes; optional turnover statistics saturate.
 */
#ifndef XCOMPILE_RESOURCES_H
#define XCOMPILE_RESOURCES_H

#include "xdefs.h"

typedef struct XrCompileResources XrCompileResources;

typedef enum XrCompileResourceStatus {
    XR_COMPILE_RESOURCE_OK,
    XR_COMPILE_RESOURCE_BAD_ARGUMENT,
    XR_COMPILE_RESOURCE_MEMORY_LIMIT,
    XR_COMPILE_RESOURCE_OUT_OF_MEMORY,
    XR_COMPILE_RESOURCE_OVERFLOW,
    XR_COMPILE_RESOURCE_CANCELLED,
    XR_COMPILE_RESOURCE_DEADLINE,
    XR_COMPILE_RESOURCE_CLOCK_FAILURE
} XrCompileResourceStatus;

typedef enum XrCompileMemoryMode {
    XR_COMPILE_MEMORY_ORDINARY,
    XR_COMPILE_MEMORY_FINITE
} XrCompileMemoryMode;

typedef struct XrCompileResourceLimits {
    XrCompileMemoryMode memory_mode;
    uint64_t memory_limit;
    bool profile_allocations;
} XrCompileResourceLimits;

typedef struct XrCompileResourceStats {
    uint64_t held_bytes, peak_bytes;
    uint64_t allocation_count, allocated_bytes;
} XrCompileResourceStats;

/* Ordinary mode requires memory_limit == 0 and imposes no memory quota.
 * Finite mode counts this owner, headers, alignment padding and all live blocks;
 * zero cannot create an owner. Statistics describe requested controlled blocks,
 * not allocator internals, stack storage or process RSS. Turnover counters are
 * zero when profiling is disabled and saturate without refusing allocations.
 * Concurrent calls require a live reference, including while waiting. Retain
 * cannot revive a pointer racing final release. Publish with caller exclusion.
 * Payload access, resize/free and output slots require caller exclusion.
 * Physical allocation/free is serialized; allocator callbacks cannot re-enter. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(
    const XrCompileResourceLimits *limits, XrCompileResources **output);
XR_FUNC XrCompileResourceStatus xr_compile_resources_retain(XrCompileResources *resources);
XR_FUNC void xr_compile_resources_release(XrCompileResources *resources);
XR_FUNC XrCompileResourceStatus xr_compile_resources_stats(
    const XrCompileResources *resources, XrCompileResourceStats *output);

/* Inspect only registered live headers, never a proposed pointer or its prefix.
 * Extent accepts an interior nonempty range; allocation requires its start.
 * Foreign, freed and out-of-range addresses fail. Success is not a pin or an
 * allocation generation: exclude mutation, resize/free throughout later access. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_extent(XrCompileResources *resources,
    const void *memory, size_t bytes);
XR_FUNC XrCompileResourceStatus xr_compile_resources_allocation(XrCompileResources *resources,
    const void *memory, size_t bytes);
/* Advisory memory admission reserves nothing and performs no allocation. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_admit(XrCompileResources *resources, size_t bytes);

/* Scan at most 64 caller-owned readable bytes. Empty input permits NULL.
 * A matching delimiter is not advanced over. Failure preserves both outputs. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_scan_delimiter(XrCompileResources *resources,
    const char *source, size_t length, unsigned char delimiter, size_t *advanced, bool *found);
/* Sizes are nonzero and outputs empty. Failure preserves output. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(
    XrCompileResources *resources, size_t bytes, void **output);
XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(
    XrCompileResources *resources, size_t count, size_t size, void **output);
/* A non-NULL block must start in this owner's live registry. Replacement is
 * allocated before copying/freeing, so finite admission includes both blocks.
 * Zero frees; equal sizes do nothing. Failure preserves the original block. */
XR_FUNC XrCompileResourceStatus xr_compile_resources_resize(
    XrCompileResources *resources, void **memory, size_t bytes);
/* This is not a pointer probe: only a live starting pointer returned by this
 * allocator is valid. NULL is a no-op. Blocks retain their owner, and payload
 * alignment supports fundamental types, not over-aligned objects. */
XR_FUNC void xr_compile_resources_free(void *memory);

#endif // XCOMPILE_RESOURCES_H
