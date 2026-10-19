/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_effect_execution_owner.h - Whole-graph finite ledgers and compiler physical owners
 */
#ifndef XIR_EFFECT_EXECUTION_OWNER_H
#define XIR_EFFECT_EXECUTION_OWNER_H
#include "base/xmalloc.h"
#include "xir/xxir_compile_context.h"
#include <string.h>
typedef struct EffectsCompileAllocation { void *pointer; size_t bytes; } EffectsCompileAllocation;
static EffectsCompileAllocation effects_compile_allocations[32768];
static size_t effects_compile_live, effects_compile_bytes, effects_compile_attempts;
static size_t effects_compile_fail_at=SIZE_MAX;
static bool effects_compile_injected;
static size_t effects_compile_slot(const void *pointer) {
    uint64_t key = (uint64_t)(uintptr_t)pointer;
    key ^= key >> 33; key *= UINT64_C(0xff51afd7ed558ccd); key ^= key >> 33;
    return (size_t)key & 32767;
}
static void *effects_compile_malloc(size_t bytes) {
    if (effects_compile_attempts++==effects_compile_fail_at) {
        effects_compile_injected=true; return NULL;
    }
    void *pointer = xr_malloc(bytes);
    if (!pointer) return NULL;
    CHECK(effects_compile_live < 16384 && bytes <= SIZE_MAX - effects_compile_bytes);
    size_t slot = effects_compile_slot(pointer);
    while (effects_compile_allocations[slot].pointer) slot = (slot + 1) & 32767;
    effects_compile_allocations[slot] = (EffectsCompileAllocation){pointer, bytes};
    ++effects_compile_live; effects_compile_bytes += bytes;
    return pointer;
}
static void effects_compile_free(void *pointer) {
    if (!pointer) return;
    size_t hole = effects_compile_slot(pointer);
    while (effects_compile_allocations[hole].pointer != pointer) {
        CHECK(effects_compile_allocations[hole].pointer); hole = (hole + 1) & 32767;
    }
    CHECK(effects_compile_live && effects_compile_bytes >= effects_compile_allocations[hole].bytes);
    --effects_compile_live; effects_compile_bytes -= effects_compile_allocations[hole].bytes;
    for (size_t next = (hole + 1) & 32767; effects_compile_allocations[next].pointer; next = (next + 1) & 32767) {
        size_t home = effects_compile_slot(effects_compile_allocations[next].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { effects_compile_allocations[hole] = effects_compile_allocations[next]; hole = next; }
    }
    effects_compile_allocations[hole] = (EffectsCompileAllocation){0}; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) effects_compile_malloc(bytes)
#define xr_free(pointer) effects_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")

typedef struct EffectsSourceOwner { XrXirCompileContext context; XrCompileResourceStats baseline; } EffectsSourceOwner;
static EffectsSourceOwner effects_source_owners[512];
static size_t effects_source_owner_count;
/* Separate semantic probes own separate finite graphs; all transitions of a
 * given graph derive the original artifact context and retain its ledger. */
static inline const XrXirCompileContext *effects_source_owner(uint64_t allocated, uint64_t work) {
    CHECK(effects_source_owner_count < 512);
    CHECK(allocated <= UINT64_C(64)*1024*1024 && work <= UINT64_C(128000000));
    EffectsSourceOwner *o=&effects_source_owners[effects_source_owner_count++];
    XrCompileResourceLimits caps={allocated,UINT64_C(8)*1024*1024,work};
    CHECK(xr_compile_resources_new(&caps,&o->context.resources)==XR_COMPILE_RESOURCE_OK);
    o->context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(o->context.resources,&o->baseline)==XR_COMPILE_RESOURCE_OK);
    return &o->context;
}
static inline void effects_source_owners_free(void) {
    uint64_t max_allocated=0,max_peak=0,max_work=0;
    for(size_t i=0;i<effects_source_owner_count;++i) {
        EffectsSourceOwner *o=&effects_source_owners[i];XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(o->context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        CHECK(stats.live_bytes==o->baseline.live_bytes);
        if(stats.allocated_bytes>max_allocated)max_allocated=stats.allocated_bytes;
        if(stats.peak_bytes>max_peak)max_peak=stats.peak_bytes;
        if(stats.work>max_work)max_work=stats.work;
        xr_compile_resources_release(o->context.resources);*o=(EffectsSourceOwner){0};
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    fprintf(stderr,"effects Source compiler: %zu finite owners, max allocated=%llu peak=%llu work=%llu; physical=0/0\n",
        effects_source_owner_count,(unsigned long long)max_allocated,(unsigned long long)max_peak,(unsigned long long)max_work);
    effects_source_owner_count=0;
}
#endif // XIR_EFFECT_EXECUTION_OWNER_H
