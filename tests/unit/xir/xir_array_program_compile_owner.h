/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_program_compile_owner.h - Whole-graph finite ledgers and compiler physical owners
 *
 * KEY CONCEPT:
 *   Parser, artifacts and Programs share a finite ledger and counted physical allocations.
 */
#ifndef XIR_ARRAY_PROGRAM_COMPILE_OWNER_H
#define XIR_ARRAY_PROGRAM_COMPILE_OWNER_H
#include "base/xmalloc.h"
#include "xir/xxir_compile_context.h"
#include "xir/xxir_internal.h"
#include <string.h>
typedef struct ArrayProgramAllocation { void *pointer; size_t bytes; } ArrayProgramAllocation;
enum { ARRAY_PROGRAM_OWNER_LIMIT = 32 };
static ArrayProgramAllocation *array_program_compile_allocations;
static size_t array_program_compile_capacity, array_program_compile_peak_capacity;
static size_t array_program_compile_peak_live, array_program_compile_peak_bytes;
static size_t array_program_compile_max_capacity(void);
static size_t array_program_compile_live, array_program_compile_bytes, array_program_compile_attempts;
static size_t array_program_compile_fail_at=SIZE_MAX;
static bool array_program_compile_injected;
static size_t array_program_compile_slot(const void *pointer) {
    uint64_t key = (uint64_t)(uintptr_t)pointer;
    key ^= key >> 33; key *= UINT64_C(0xff51afd7ed558ccd); key ^= key >> 33;
    return (size_t)key & (array_program_compile_capacity - 1);
}
/* Observer storage is outside the compiler fault and resource boundary. Each
 * actual compiler allocation still has its original index and physical record. */
static void array_program_compile_grow(void) {
    if (array_program_compile_live + 1 <= array_program_compile_capacity / 2) return;
    size_t capacity = array_program_compile_capacity ? array_program_compile_capacity * 2 : 512;
    CHECK(capacity <= array_program_compile_max_capacity());
    CHECK(capacity <= SIZE_MAX / sizeof(*array_program_compile_allocations));
    ArrayProgramAllocation *replacement = xr_calloc(capacity, sizeof(*replacement));
    CHECK(replacement);
    ArrayProgramAllocation *previous = array_program_compile_allocations;
    size_t previous_capacity = array_program_compile_capacity;
    array_program_compile_allocations = replacement; array_program_compile_capacity = capacity;
    for (size_t i = 0; i < previous_capacity; ++i) if (previous[i].pointer) {
        size_t slot = array_program_compile_slot(previous[i].pointer);
        while (replacement[slot].pointer) slot = (slot + 1) & (capacity - 1);
        replacement[slot] = previous[i];
    }
    xr_free(previous);
    if (capacity > array_program_compile_peak_capacity) array_program_compile_peak_capacity = capacity;
}
static void *array_program_compile_malloc(size_t bytes) {
    if (array_program_compile_attempts++==array_program_compile_fail_at) {
        array_program_compile_injected=true; return NULL;
    }
    array_program_compile_grow();
    void *pointer = xr_malloc(bytes);
    if (!pointer) return NULL;
    CHECK(bytes <= SIZE_MAX - array_program_compile_bytes);
    size_t slot = array_program_compile_slot(pointer);
    while (array_program_compile_allocations[slot].pointer) slot = (slot + 1) & (array_program_compile_capacity - 1);
    array_program_compile_allocations[slot] = (ArrayProgramAllocation){pointer, bytes};
    ++array_program_compile_live; array_program_compile_bytes += bytes;
    if (array_program_compile_live > array_program_compile_peak_live) array_program_compile_peak_live = array_program_compile_live;
    if (array_program_compile_bytes > array_program_compile_peak_bytes) array_program_compile_peak_bytes = array_program_compile_bytes;
    return pointer;
}
static void array_program_compile_free(void *pointer) {
    if (!pointer) return;
    size_t hole = array_program_compile_slot(pointer);
    while (array_program_compile_allocations[hole].pointer != pointer) {
        CHECK(array_program_compile_allocations[hole].pointer); hole = (hole + 1) & (array_program_compile_capacity - 1);
    }
    CHECK(array_program_compile_live && array_program_compile_bytes >= array_program_compile_allocations[hole].bytes);
    --array_program_compile_live; array_program_compile_bytes -= array_program_compile_allocations[hole].bytes;
    for (size_t next = (hole + 1) & (array_program_compile_capacity - 1); array_program_compile_allocations[next].pointer; next = (next + 1) & (array_program_compile_capacity - 1)) {
        size_t home = array_program_compile_slot(array_program_compile_allocations[next].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { array_program_compile_allocations[hole] = array_program_compile_allocations[next]; hole = next; }
    }
    array_program_compile_allocations[hole] = (ArrayProgramAllocation){0}; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) array_program_compile_malloc(bytes)
#define xr_free(pointer) array_program_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

/* Every tracked ledger has an 8 MiB live cap. A payload is nonzero and its
 * physical header is charged before malloc. Add one record per ledger itself. */
static size_t array_program_compile_max_capacity(void) {
    uint64_t records = ARRAY_PROGRAM_OWNER_LIMIT +
        ((uint64_t)ARRAY_PROGRAM_OWNER_LIMIT * 8 * 1024 * 1024) / (sizeof(CompileAllocation) + 1);
    size_t capacity = 512;
    while ((uint64_t)(capacity / 2) < records) {
        CHECK(capacity <= SIZE_MAX / 2); capacity *= 2;
    }
    return capacity;
}

typedef struct ArrayCompileOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
} ArrayCompileOwner;
static XrCompileResourceLimits array_compile_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
}
static XrCompileResourceStatus array_compile_owner_new(XrCompileResourceLimits caps,ArrayCompileOwner *owner) {
    CHECK(!array_program_compile_live && !array_program_compile_bytes);
    *owner=(ArrayCompileOwner){0};
    XrCompileResourceStatus status=xr_compile_resources_new(&caps,&owner->context.resources);
    if(status==XR_COMPILE_RESOURCE_OK) {
        owner->context.limits=xr_xir_compile_default_limits();
        CHECK(xr_compile_resources_stats(owner->context.resources,&owner->baseline)==XR_COMPILE_RESOURCE_OK);
    }
    return status;
}
static XrCompileResourceStats array_compile_stats(ArrayCompileOwner *owner) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(owner->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void array_compile_owner_free(ArrayCompileOwner *owner) {
    if(owner->context.resources) {
        CHECK(array_compile_stats(owner).live_bytes==owner->baseline.live_bytes);
        xr_compile_resources_release(owner->context.resources);
    }
    *owner=(ArrayCompileOwner){0};CHECK(!array_program_compile_live && !array_program_compile_bytes);
    xr_free(array_program_compile_allocations);array_program_compile_allocations=NULL;
    array_program_compile_capacity=0;
}
#endif // XIR_ARRAY_PROGRAM_COMPILE_OWNER_H
