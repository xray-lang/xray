/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_stage_context_owner.h - Owned compiler memory for stage witnesses
 *
 * KEY CONCEPT:
 *   Independent probes own ordinary or explicitly finite held memory.
 *   Physical allocation remains accounted until every owner ends.
 */
#ifndef XIR_STAGE_CONTEXT_OWNER_H
#define XIR_STAGE_CONTEXT_OWNER_H
#include "xir_source_fixture_owner.h"
typedef struct StagePhysicalAllocation { void *pointer; size_t bytes; } StagePhysicalAllocation;
static StagePhysicalAllocation stage_physical[32768];
static size_t stage_physical_count, stage_physical_bytes;
static void *stage_physical_malloc(size_t bytes) {
    void *pointer = xr_malloc(bytes);
    if (pointer) {
        CHECK(stage_physical_count < 32768 && bytes <= SIZE_MAX - stage_physical_bytes);
        stage_physical[stage_physical_count++] = (StagePhysicalAllocation){pointer, bytes};
        stage_physical_bytes += bytes;
    }
    return pointer;
}
static void stage_physical_free(void *pointer) {
    if (pointer) {
        size_t i = 0;
        while (i < stage_physical_count && stage_physical[i].pointer != pointer) ++i;
        CHECK(i < stage_physical_count && stage_physical[i].bytes <= stage_physical_bytes);
        stage_physical_bytes -= stage_physical[i].bytes;
        stage_physical[i] = stage_physical[--stage_physical_count];
    }
    xr_free(pointer);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) stage_physical_malloc(bytes)
#define xr_free(pointer) stage_physical_free(pointer)
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
static SourceFixtureOwner stage_owners[1024];
static size_t stage_owner_count;
static XrCompileResourceStats stage_owner_baseline;
static XrXirCompileContext stage_context;
static XrCompileResourceStats stage_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}
static XrXirCompileContext stage_context_default(void) {
    CHECK(stage_owner_count < 1024);
    SourceFixtureOwner *owner = &stage_owners[stage_owner_count++];
    source_fixture_owner_new(owner);
    if (stage_owner_count == 1) stage_owner_baseline = owner->baseline;
    CHECK(owner->baseline.held_bytes == stage_owner_baseline.held_bytes);
    return owner->context;
}
static XrXirCompileContext stage_context_limited(uint64_t held) {
    CHECK(stage_owner_count && stage_owner_count < 1024);
    CHECK(held <= UINT64_MAX - stage_owner_baseline.held_bytes);
    SourceFixtureOwner *owner = &stage_owners[stage_owner_count++];
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE,
        stage_owner_baseline.held_bytes + held,true};
    CHECK(xr_compile_resources_new(&limits, &owner->context.resources) == XR_COMPILE_RESOURCE_OK);
    owner->context.limits = xr_xir_compile_default_limits();
    owner->baseline = stage_stats(&owner->context);
    return owner->context;
}
static void stage_contexts_free(void) {
    uint64_t allocated = 0, max_allocated = 0, max_peak = 0;
    for (size_t i = 0; i < stage_owner_count; ++i) {
        SourceFixtureOwner *owner = &stage_owners[i];
        XrCompileResourceStats stats = stage_stats(&owner->context);
        CHECK(stats.held_bytes == owner->baseline.held_bytes);
        if (stats.allocated_bytes > max_allocated) max_allocated = stats.allocated_bytes;
        if (stats.peak_bytes > max_peak) max_peak = stats.peak_bytes;
        allocated = stats.allocated_bytes > UINT64_MAX - allocated ? UINT64_MAX : allocated + stats.allocated_bytes;
        xr_compile_resources_release(owner->context.resources);
        *owner = (SourceFixtureOwner){0};
    }
    CHECK(!stage_physical_count && !stage_physical_bytes);
    fprintf(stderr, "stage compiler memory: %zu owners, allocated=%llu; physical blocks/bytes=0/0\n",
        stage_owner_count, (unsigned long long)allocated);
    fprintf(stderr, "stage compiler observed maxima: allocated=%llu peak=%llu\n",
        (unsigned long long)max_allocated, (unsigned long long)max_peak);
}
#endif // XIR_STAGE_CONTEXT_OWNER_H
