/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_resources.c - Physical compiler allocation accounting
 *
 * KEY CONCEPT:
 *   Cumulative allocation and work never decrease. Live bytes decrease only
 *   after free, while allocation headers keep the shared ledger alive.
 */
#include "xcompile_resources.h"
#include "xchecks.h"
#include "xmalloc.h"
#include <stdatomic.h>
#include <string.h>

struct XrCompileResources {
    XrCompileResourceLimits limits;
    XrCompileResourceStats stats;
    uint64_t references;
    atomic_bool locked;
};

/* Every caller already owns a reference while waiting for this lock. */
static void compile_lock(const XrCompileResources *resources) {
    atomic_bool *locked = &((XrCompileResources *) resources)->locked;
    for (;;) {
        if (!atomic_exchange_explicit(locked, true, memory_order_acquire)) return;
        while (atomic_load_explicit(locked, memory_order_relaxed)) XR_CPU_PAUSE();
    }
}

static void compile_unlock(XrCompileResources *resources) {
    atomic_store_explicit(&resources->locked, false, memory_order_release);
}

static bool compile_release_locked(XrCompileResources *resources) {
    XR_CHECK(resources->references, "A live compiler ledger must have an owner");
    if (--resources->references) return false;
    XR_CHECK(resources->stats.live_bytes == sizeof(*resources), "Outstanding compiler allocations retain their ledger");
    return true;
}

/* Microsoft's C headers omit max_align_t; these scalar types cover its
 * fundamental alignments without depending on an OS allocation API. */
#if defined(XR_COMPILER_MSVC)
typedef union CompileMaxAlignment {
    long double floating;
    uint64_t integer;
    void *pointer;
} CompileMaxAlignment;
#else
typedef max_align_t CompileMaxAlignment;
#endif

typedef union CompileAllocation {
    struct {
        XrCompileResources *resources;
        size_t bytes;
    } info;
    CompileMaxAlignment alignment;
} CompileAllocation;

_Static_assert(sizeof(size_t) <= sizeof(uint64_t), "Allocation sizes must fit the ledger");
_Static_assert(sizeof(CompileAllocation) % _Alignof(CompileMaxAlignment) == 0,
    "Allocation payload must preserve fundamental alignment");

XR_FUNC XrCompileResourceStatus xr_compile_resources_new(
    const XrCompileResourceLimits *limits, XrCompileResources **output) {
    if (!limits || !output || *output) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (sizeof(XrCompileResources) > limits->allocated_bytes ||
        sizeof(XrCompileResources) > limits->live_bytes || !limits->work)
        return XR_COMPILE_RESOURCE_BUDGET;
    XrCompileResources *resources = xr_malloc(sizeof(*resources));
    if (!resources) return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    resources->limits = *limits;
    resources->stats = (XrCompileResourceStats) {1, sizeof(*resources), sizeof(*resources), sizeof(*resources), 1};
    resources->references = 1;
    atomic_init(&resources->locked, false);
    *output = resources;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_retain(XrCompileResources *resources) {
    if (!resources) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    XrCompileResourceStatus status = XR_COMPILE_RESOURCE_BUDGET;
    if (resources->references != UINT64_MAX) {
        ++resources->references;
        status = XR_COMPILE_RESOURCE_OK;
    }
    compile_unlock(resources);
    return status;
}

XR_FUNC void xr_compile_resources_release(XrCompileResources *resources) {
    if (!resources) return;
    compile_lock(resources);
    bool last = compile_release_locked(resources);
    compile_unlock(resources);
    /* No legal caller can still be waiting without a retained reference. */
    if (last) xr_free(resources);
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_stats(
    const XrCompileResources *resources, XrCompileResourceStats *output) {
    if (!resources || !output) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    *output = resources->stats;
    compile_unlock((XrCompileResources *) resources);
    return XR_COMPILE_RESOURCE_OK;
}

static XrCompileResourceStatus compile_work_locked(XrCompileResources *resources, uint64_t units) {
    if (units > resources->limits.work - resources->stats.work) return XR_COMPILE_RESOURCE_BUDGET;
    resources->stats.work += units;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources, uint64_t units) {
    if (!resources) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    XrCompileResourceStatus status = compile_work_locked(resources, units);
    compile_unlock(resources);
    return status;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_scan_delimiter(XrCompileResources *resources,
    const char *source, size_t length, unsigned char delimiter, size_t *advanced, bool *found) {
    if (!resources || (!source && length) || length > 64 || !advanced || !found)
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    size_t position = 0;
    bool matched = false;
    XrCompileResourceStatus status = XR_COMPILE_RESOURCE_OK;
    compile_lock(resources);
    while (position < length) {
        status = compile_work_locked(resources, 1);
        if (status != XR_COMPILE_RESOURCE_OK) break;
        unsigned char value = (unsigned char) source[position];
        if (value == delimiter) { matched = true; break; }
        status = compile_work_locked(resources, 1);
        if (status != XR_COMPILE_RESOURCE_OK) break;
        ++position;
    }
    if (status == XR_COMPILE_RESOURCE_OK) {
        *advanced = position;
        *found = matched;
    }
    compile_unlock(resources);
    return status;
}

static XrCompileResourceStatus compile_allocate_locked(
    XrCompileResources *resources, size_t bytes, size_t payload_work, void **output) {
    if (bytes > SIZE_MAX - sizeof(CompileAllocation)) return XR_COMPILE_RESOURCE_BUDGET;
    size_t charged = sizeof(CompileAllocation) + bytes;
    uint64_t remaining_work = resources->limits.work - resources->stats.work;
    if (charged > resources->limits.allocated_bytes - resources->stats.allocated_bytes ||
        charged > resources->limits.live_bytes - resources->stats.live_bytes ||
        !remaining_work || payload_work > remaining_work - 1 ||
        resources->references == UINT64_MAX || resources->stats.allocation_count == UINT64_MAX)
        return XR_COMPILE_RESOURCE_BUDGET;
    ++resources->stats.work;
    CompileAllocation *allocation = xr_malloc(charged);
    if (!allocation) return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    allocation->info.resources = resources;
    allocation->info.bytes = bytes;
    ++resources->references;
    ++resources->stats.allocation_count;
    resources->stats.allocated_bytes += charged;
    resources->stats.live_bytes += charged;
    if (resources->stats.live_bytes > resources->stats.peak_bytes)
        resources->stats.peak_bytes = resources->stats.live_bytes;
    *output = allocation + 1;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(
    XrCompileResources *resources, size_t bytes, void **output) {
    if (!resources || !bytes || !output || *output) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    compile_lock(resources);
    XrCompileResourceStatus status = compile_allocate_locked(resources, bytes, 0, output);
    compile_unlock(resources);
    return status;
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(
    XrCompileResources *resources, size_t count, size_t size, void **output) {
    if (!resources || !count || !size || !output || *output) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (count > SIZE_MAX / size) return XR_COMPILE_RESOURCE_BUDGET;
    size_t bytes = count * size;
    void *memory = NULL;
    compile_lock(resources);
    XrCompileResourceStatus status = compile_allocate_locked(resources, bytes, bytes, &memory);
    if (status == XR_COMPILE_RESOURCE_OK) {
        resources->stats.work += bytes;
        memset(memory, 0, bytes);
        *output = memory;
    }
    compile_unlock(resources);
    return status;
}

static bool compile_free_locked(XrCompileResources *resources, CompileAllocation *allocation) {
    size_t charged = sizeof(*allocation) + allocation->info.bytes;
    XR_CHECK(resources->stats.live_bytes >= charged, "Compiler live bytes must cover the released allocation");
    xr_free(allocation);
    resources->stats.live_bytes -= charged;
    return compile_release_locked(resources);
}

XR_FUNC void xr_compile_resources_free(void *memory) {
    if (!memory) return;
    CompileAllocation *allocation = (CompileAllocation *) memory - 1;
    XrCompileResources *resources = allocation->info.resources;
    compile_lock(resources);
    bool last = compile_free_locked(resources, allocation);
    compile_unlock(resources);
    if (last) xr_free(resources);
}

XR_FUNC XrCompileResourceStatus xr_compile_resources_resize(
    XrCompileResources *resources, void **memory, size_t bytes) {
    if (!resources || !memory) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (!*memory) return bytes ? xr_compile_resources_alloc(resources, bytes, memory) : XR_COMPILE_RESOURCE_OK;
    CompileAllocation *allocation = (CompileAllocation *) *memory - 1;
    if (allocation->info.resources != resources) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (!bytes) {
        compile_lock(resources);
        bool last = compile_free_locked(resources, allocation);
        *memory = NULL;
        compile_unlock(resources);
        if (last) xr_free(resources);
        return XR_COMPILE_RESOURCE_OK;
    }
    size_t old_bytes = allocation->info.bytes;
    if (old_bytes == bytes) return XR_COMPILE_RESOURCE_OK;
    size_t copied = old_bytes < bytes ? old_bytes : bytes;
    void *replacement = NULL;
    compile_lock(resources);
    XrCompileResourceStatus status = compile_allocate_locked(resources, bytes, copied, &replacement);
    if (status == XR_COMPILE_RESOURCE_OK) {
        resources->stats.work += copied;
        memcpy(replacement, *memory, copied);
        bool last = compile_free_locked(resources, allocation);
        XR_CHECK(!last, "The resized allocation retains its compiler ledger");
        *memory = replacement;
    }
    compile_unlock(resources);
    return status;
}
