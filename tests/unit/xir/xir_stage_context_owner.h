/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_stage_context_owner.h - Finite isolated compiler ledgers for stage witnesses
 *
 * KEY CONCEPT:
 *   Fresh rejection probes cannot consume another probe's work allowance.
 *   Retained ledgers account for physical allocation until all owners end.
 */
#ifndef XIR_STAGE_CONTEXT_OWNER_H
#define XIR_STAGE_CONTEXT_OWNER_H
#include "xir_source_fixture_owner.h"
#define STAGE_ALLOCATED_BYTES (UINT64_C(64) * 1024 * 1024)
#define STAGE_LIVE_BYTES (UINT64_C(8) * 1024 * 1024)
#define STAGE_WORK UINT64_C(128000000)
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
    CHECK(owner->baseline.allocated_bytes == stage_owner_baseline.allocated_bytes);
    CHECK(owner->baseline.live_bytes == stage_owner_baseline.live_bytes && owner->baseline.work == stage_owner_baseline.work);
    return owner->context;
}
static XrXirCompileContext stage_context_limited(uint64_t allocated, uint64_t live, uint64_t work) {
    CHECK(stage_owner_count && stage_owner_count < 1024);
    CHECK(allocated <= STAGE_ALLOCATED_BYTES && live <= STAGE_LIVE_BYTES && work <= STAGE_WORK);
    SourceFixtureOwner *owner = &stage_owners[stage_owner_count++];
    XrCompileResourceLimits limits = {
        allocated > STAGE_ALLOCATED_BYTES - stage_owner_baseline.allocated_bytes ? STAGE_ALLOCATED_BYTES : stage_owner_baseline.allocated_bytes + allocated,
        live > STAGE_LIVE_BYTES - stage_owner_baseline.live_bytes ? STAGE_LIVE_BYTES : stage_owner_baseline.live_bytes + live,
        work > STAGE_WORK - stage_owner_baseline.work ? STAGE_WORK : stage_owner_baseline.work + work};
    CHECK(xr_compile_resources_new(&limits, &owner->context.resources) == XR_COMPILE_RESOURCE_OK);
    owner->context.limits = xr_xir_compile_default_limits();
    owner->baseline = stage_stats(&owner->context);
    return owner->context;
}
static void stage_contexts_free(void) {
    uint64_t allocated = 0, work = 0, max_allocated = 0, max_work = 0, max_peak = 0;
    for (size_t i = 0; i < stage_owner_count; ++i) {
        SourceFixtureOwner *owner = &stage_owners[i];
        XrCompileResourceStats stats = stage_stats(&owner->context);
        CHECK(stats.live_bytes == owner->baseline.live_bytes);
        CHECK(stats.allocated_bytes >= owner->baseline.allocated_bytes && stats.work >= owner->baseline.work);
        CHECK(stats.allocated_bytes <= STAGE_ALLOCATED_BYTES && stats.work <= STAGE_WORK && stats.peak_bytes <= STAGE_LIVE_BYTES);
        if (stats.allocated_bytes > max_allocated) max_allocated = stats.allocated_bytes;
        if (stats.work > max_work) max_work = stats.work;
        if (stats.peak_bytes > max_peak) max_peak = stats.peak_bytes;
        allocated += stats.allocated_bytes; work += stats.work;
        xr_compile_resources_release(owner->context.resources);
        *owner = (SourceFixtureOwner){0};
    }
    CHECK(!stage_physical_count && !stage_physical_bytes);
    fprintf(stderr, "stage compiler ledgers: %zu owners, allocated=%llu work=%llu; physical blocks/bytes=0/0\n",
        stage_owner_count, (unsigned long long)allocated, (unsigned long long)work);
    fprintf(stderr, "stage compiler per-owner maxima: allocated=%llu work=%llu peak=%llu; limits=%llu/%llu/%llu\n",
        (unsigned long long)max_allocated, (unsigned long long)max_work, (unsigned long long)max_peak,
        (unsigned long long)STAGE_ALLOCATED_BYTES, (unsigned long long)STAGE_WORK, (unsigned long long)STAGE_LIVE_BYTES);
}
#endif // XIR_STAGE_CONTEXT_OWNER_H
