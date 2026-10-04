/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_unit_locals_compile_owner.h - Finite Unit program compiler ownership
 *
 * KEY CONCEPT:
 *   The whole graph keeps one ledger; real compiler blocks are a separate
 *   physical domain from instance, Call and escaped runtime values.
 */
#ifndef XIR_UNIT_LOCALS_COMPILE_OWNER_H
#define XIR_UNIT_LOCALS_COMPILE_OWNER_H
#include "xir/xxir.h"
#include "base/xmalloc.h"
typedef struct UnitCompileBlock { void *pointer; size_t bytes; } UnitCompileBlock;
static UnitCompileBlock unit_compile_blocks[32768];
static size_t unit_compile_live, unit_compile_bytes, unit_compile_peak;
static size_t unit_compile_attempts, unit_compile_fail_at = SIZE_MAX;
static bool unit_compile_injected;
static uint64_t unit_compile_max_allocated, unit_compile_max_work;
static void *unit_compile_malloc(size_t bytes) {
    if (unit_compile_attempts++ == unit_compile_fail_at) {
        unit_compile_injected = true; return NULL;
    }
    void *pointer = xr_malloc(bytes);
    if (pointer) {
        CHECK(unit_compile_live < 32768 && bytes <= SIZE_MAX - unit_compile_bytes);
        unit_compile_blocks[unit_compile_live++] = (UnitCompileBlock){pointer, bytes};
        unit_compile_bytes += bytes;
        if (unit_compile_bytes > unit_compile_peak) unit_compile_peak = unit_compile_bytes;
    }
    return pointer;
}
static void unit_compile_free(void *pointer) {
    if (pointer) {
        size_t index = 0;
        while (index < unit_compile_live && unit_compile_blocks[index].pointer != pointer) ++index;
        CHECK(index < unit_compile_live && unit_compile_blocks[index].bytes <= unit_compile_bytes);
        unit_compile_bytes -= unit_compile_blocks[index].bytes;
        unit_compile_blocks[index] = unit_compile_blocks[--unit_compile_live];
    }
    xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) unit_compile_malloc(bytes)
#define xr_free(pointer) unit_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

typedef struct UnitCompileOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
} UnitCompileOwner;
static void unit_compile_owner_new(UnitCompileOwner *owner) {
    CHECK(!owner->context.resources);
    const XrCompileResourceLimits limits = {
        UINT64_C(33554432), UINT64_C(8388608), UINT64_C(64000000)};
    CHECK(xr_compile_resources_new(&limits, &owner->context.resources) == XR_COMPILE_RESOURCE_OK);
    owner->context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(owner->context.resources, &owner->baseline) == XR_COMPILE_RESOURCE_OK);
}
static void unit_compile_owner_free(UnitCompileOwner *owner) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(owner->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == owner->baseline.live_bytes);
    CHECK(stats.allocated_bytes <= UINT64_C(33554432) && stats.peak_bytes <= UINT64_C(8388608));
    CHECK(stats.work <= UINT64_C(64000000));
    if (stats.allocated_bytes > unit_compile_max_allocated) unit_compile_max_allocated = stats.allocated_bytes;
    if (stats.work > unit_compile_max_work) unit_compile_max_work = stats.work;
    xr_compile_resources_release(owner->context.resources);
    *owner = (UnitCompileOwner){0};
}
static void unit_compile_report(void) {
    CHECK(!unit_compile_live && !unit_compile_bytes);
    printf("Unit compiler physical blocks/bytes=0/0 max allocated/work=%llu/%llu peak=%zu limits=33554432/8388608/64000000\n",
        (unsigned long long)unit_compile_max_allocated, (unsigned long long)unit_compile_max_work, unit_compile_peak);
}
#endif // XIR_UNIT_LOCALS_COMPILE_OWNER_H
