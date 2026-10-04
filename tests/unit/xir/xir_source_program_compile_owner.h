/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_program_compile_owner.h - Whole-graph finite ledgers and compiler physical owners
 *
 * KEY CONCEPT:
 *   Parser, artifacts and Programs share a finite ledger and counted physical allocations.
 */
#ifndef XIR_SOURCE_PROGRAM_COMPILE_OWNER_H
#define XIR_SOURCE_PROGRAM_COMPILE_OWNER_H
#include "base/xmalloc.h"
#include "xir/xxir_compile_context.h"
#include "xir/xxir_internal.h"
#include <string.h>
typedef struct SourceProgramAllocation { void *pointer; size_t bytes; } SourceProgramAllocation;
enum { SOURCE_PROGRAM_OWNER_LIMIT = 32 };
static SourceProgramAllocation *source_program_compile_allocations;
static size_t source_program_compile_capacity, source_program_compile_peak_capacity;
static size_t source_program_compile_peak_live, source_program_compile_peak_bytes;
static size_t source_program_compile_max_capacity(void);
static size_t source_program_compile_live, source_program_compile_bytes, source_program_compile_attempts;
static size_t source_program_compile_fail_at=SIZE_MAX;
static bool source_program_compile_injected;
static size_t source_program_compile_slot(const void *pointer) {
    uint64_t key = (uint64_t)(uintptr_t)pointer;
    key ^= key >> 33; key *= UINT64_C(0xff51afd7ed558ccd); key ^= key >> 33;
    return (size_t)key & (source_program_compile_capacity - 1);
}
/* Observer storage is outside the compiler fault and resource boundary. Each
 * actual compiler allocation still has its original index and physical record. */
static void source_program_compile_grow(void) {
    if (source_program_compile_live + 1 <= source_program_compile_capacity / 2) return;
    size_t capacity = source_program_compile_capacity ? source_program_compile_capacity * 2 : 512;
    CHECK(capacity <= source_program_compile_max_capacity());
    CHECK(capacity <= SIZE_MAX / sizeof(*source_program_compile_allocations));
    SourceProgramAllocation *replacement = xr_calloc(capacity, sizeof(*replacement));
    CHECK(replacement);
    SourceProgramAllocation *previous = source_program_compile_allocations;
    size_t previous_capacity = source_program_compile_capacity;
    source_program_compile_allocations = replacement; source_program_compile_capacity = capacity;
    for (size_t i = 0; i < previous_capacity; ++i) if (previous[i].pointer) {
        size_t slot = source_program_compile_slot(previous[i].pointer);
        while (replacement[slot].pointer) slot = (slot + 1) & (capacity - 1);
        replacement[slot] = previous[i];
    }
    xr_free(previous);
    if (capacity > source_program_compile_peak_capacity) source_program_compile_peak_capacity = capacity;
}
static void *source_program_compile_malloc(size_t bytes) {
    if (source_program_compile_attempts++==source_program_compile_fail_at) {
        source_program_compile_injected=true; return NULL;
    }
    source_program_compile_grow();
    void *pointer = xr_malloc(bytes);
    if (!pointer) return NULL;
    CHECK(bytes <= SIZE_MAX - source_program_compile_bytes);
    size_t slot = source_program_compile_slot(pointer);
    while (source_program_compile_allocations[slot].pointer) slot = (slot + 1) & (source_program_compile_capacity - 1);
    source_program_compile_allocations[slot] = (SourceProgramAllocation){pointer, bytes};
    ++source_program_compile_live; source_program_compile_bytes += bytes;
    if (source_program_compile_live > source_program_compile_peak_live) source_program_compile_peak_live = source_program_compile_live;
    if (source_program_compile_bytes > source_program_compile_peak_bytes) source_program_compile_peak_bytes = source_program_compile_bytes;
    return pointer;
}
static void source_program_compile_free(void *pointer) {
    if (!pointer) return;
    size_t hole = source_program_compile_slot(pointer);
    while (source_program_compile_allocations[hole].pointer != pointer) {
        CHECK(source_program_compile_allocations[hole].pointer); hole = (hole + 1) & (source_program_compile_capacity - 1);
    }
    CHECK(source_program_compile_live && source_program_compile_bytes >= source_program_compile_allocations[hole].bytes);
    --source_program_compile_live; source_program_compile_bytes -= source_program_compile_allocations[hole].bytes;
    for (size_t next = (hole + 1) & (source_program_compile_capacity - 1); source_program_compile_allocations[next].pointer; next = (next + 1) & (source_program_compile_capacity - 1)) {
        size_t home = source_program_compile_slot(source_program_compile_allocations[next].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { source_program_compile_allocations[hole] = source_program_compile_allocations[next]; hole = next; }
    }
    source_program_compile_allocations[hole] = (SourceProgramAllocation){0}; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) source_program_compile_malloc(bytes)
#define xr_free(pointer) source_program_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

/* Every tracked ledger has an 8 MiB live cap. A payload is nonzero and its
 * physical header is charged before malloc. Add one record per ledger itself. */
static size_t source_program_compile_max_capacity(void) {
    uint64_t records = SOURCE_PROGRAM_OWNER_LIMIT +
        ((uint64_t)SOURCE_PROGRAM_OWNER_LIMIT * 8 * 1024 * 1024) / (sizeof(CompileAllocation) + 1);
    size_t capacity = 512;
    while ((uint64_t)(capacity / 2) < records) {
        CHECK(capacity <= SIZE_MAX / 2); capacity *= 2;
    }
    return capacity;
}

typedef struct SourceProgramOwner { XrXirCompileContext context; XrCompileResourceStats baseline; } SourceProgramOwner;
static SourceProgramOwner source_program_owners[SOURCE_PROGRAM_OWNER_LIMIT];
static size_t source_program_owner_count;
/* Separate semantic probes own separate finite graphs; all transitions of a
 * given graph derive the original artifact context and retain its ledger. */
static inline const XrXirCompileContext *source_program_owner(uint64_t allocated, uint64_t work) {
    CHECK(source_program_owner_count < SOURCE_PROGRAM_OWNER_LIMIT);
    CHECK(allocated <= UINT64_C(64)*1024*1024 && work <= UINT64_C(128000000));
    SourceProgramOwner *o=&source_program_owners[source_program_owner_count++];
    XrCompileResourceLimits caps={allocated,UINT64_C(8)*1024*1024,work};
    CHECK(xr_compile_resources_new(&caps,&o->context.resources)==XR_COMPILE_RESOURCE_OK);
    o->context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(o->context.resources,&o->baseline)==XR_COMPILE_RESOURCE_OK);
    return &o->context;
}
static inline void source_program_owners_free(void) {
    uint64_t max_allocated=0,max_peak=0,max_work=0;
    for(size_t i=0;i<source_program_owner_count;++i) {
        SourceProgramOwner *o=&source_program_owners[i];XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(o->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        CHECK(stats.live_bytes==o->baseline.live_bytes);
        fprintf(stderr,"Source compiler owner %zu: allocated=%llu peak=%llu work=%llu; final live=%llu\n",
            i,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,
            (unsigned long long)stats.work,(unsigned long long)stats.live_bytes);
        if(stats.allocated_bytes>max_allocated)max_allocated=stats.allocated_bytes;
        if(stats.peak_bytes>max_peak)max_peak=stats.peak_bytes;
        if(stats.work>max_work)max_work=stats.work;
        xr_compile_resources_release(o->context.resources);*o=(SourceProgramOwner){0};
    }
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    fprintf(stderr,"Source program compiler: %zu finite owners, max allocated=%llu peak=%llu work=%llu; physical=0/0\n",
        source_program_owner_count,(unsigned long long)max_allocated,(unsigned long long)max_peak,(unsigned long long)max_work);
    fprintf(stderr,"Source compiler observer: actual peak blocks=%zu bytes=%zu table=%zu; final physical=0/0\n",
        source_program_compile_peak_live,source_program_compile_peak_bytes,source_program_compile_peak_capacity);
    xr_free(source_program_compile_allocations); source_program_compile_allocations=NULL;
    source_program_compile_capacity=0; source_program_compile_peak_capacity=0;
    source_program_compile_peak_live=0; source_program_compile_peak_bytes=0;
    source_program_owner_count=0;
}
#endif // XIR_SOURCE_PROGRAM_COMPILE_OWNER_H
