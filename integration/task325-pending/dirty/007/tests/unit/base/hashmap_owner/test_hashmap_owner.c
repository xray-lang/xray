/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#include "base/xhashmap.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while(0)
typedef struct Physical { void *pointer; size_t bytes; } Physical;
static Physical blocks[64];
static size_t attempts, fail_at = SIZE_MAX, physical_live, physical_total, physical_peak, block_count;
static void *observed_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *p = xr_malloc(bytes); CHECK(p);
    size_t i = 0; while (i < 64 && blocks[i].pointer) ++i; CHECK(i < 64);
    blocks[i] = (Physical){p,bytes}; ++block_count; physical_live += bytes; physical_total += bytes;
    if (physical_live > physical_peak) physical_peak = physical_live;
    return p;
}
static void observed_free(void *p) {
    if (!p) return;
    size_t i = 0; while (i < 64 && blocks[i].pointer != p) ++i; CHECK(i < 64);
    physical_live -= blocks[i].bytes; --block_count; blocks[i] = (Physical){0}; xr_free(p);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_alloc(bytes)
#define xr_free(memory) observed_free(memory)
#include "base/xcompile_resources.c"
static const XrCompileResourceLimits unlimited = {UINT64_MAX,UINT64_MAX,UINT64_MAX};
static XrCompileResourceStats measured;
static char keys[80][24];
static unsigned char canary;
static void reset(size_t failure) {
    CHECK(!physical_live && !block_count); attempts = physical_total = physical_peak = 0; fail_at = failure;
}
static XrCompileResourceStats stats(XrCompileResources *owner) {
    XrCompileResourceStats result; CHECK(xr_compile_resources_stats(owner,&result) == XR_COMPILE_RESOURCE_OK);
    CHECK(result.live_bytes == physical_live && result.allocated_bytes == physical_total && result.peak_bytes == physical_peak);
    return result;
}
static void cleanup(void) { CHECK(!physical_live && !block_count); }
static void check_entries(XrHashMap *map, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        void *value = NULL; CHECK(xr_hashmap_owned_get(map,keys[i],&value) == XR_OS_IO_OK);
        CHECK(value == (void *)(uintptr_t)(i+2));
    }
}
static size_t sequence(const XrCompileResourceLimits *limits, size_t failure, bool success) {
    reset(failure); XrCompileResources *owner = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(limits,&owner);
    if (created != XR_COMPILE_RESOURCE_OK) { CHECK(!success && !owner); cleanup(); return attempts; }
    XrOsIoPolicy policy = xr_compile_io_policy(owner); XrHashMap *map = (void *)&canary;
    XrOsIoStatus status = xr_hashmap_owned_new(&policy,&map);
    if (status != XR_OS_IO_OK) CHECK(map == (void *)&canary);
    else {
        for (unsigned i = 0; i < 80 && status == XR_OS_IO_OK; ++i)
            status = xr_hashmap_owned_set(map,keys[i],(void *)(uintptr_t)(i+2));
        if (status == XR_OS_IO_OK) {
            bool removed = false; status = xr_hashmap_owned_delete(map,keys[8],&removed);
            if (status == XR_OS_IO_OK) CHECK(removed && map->count == 79);
        }
        xr_hashmap_owned_free(map);
    }
    CHECK(status == (success ? XR_OS_IO_OK : failure != SIZE_MAX ? XR_OS_IO_OUT_OF_MEMORY : XR_OS_IO_BUDGET));
    measured = stats(owner); CHECK(block_count == 1); xr_compile_resources_release(owner); cleanup(); return attempts;
}
typedef struct FaultPolicy { XrCompileResources *owner; size_t calls, failure; XrOsIoStatus error; } FaultPolicy;
static XrOsIoStatus fault_alloc(void *context, size_t size, void **out) {
    FaultPolicy *fault = context; XrOsIoPolicy p = xr_compile_io_policy(fault->owner); return p.alloc(p.context,size,out);
}
static void fault_free(void *context, void *p) { (void)context; xr_compile_resources_free(p); }
static XrOsIoStatus fault_work(void *context, uint64_t units) {
    FaultPolicy *fault = context;
    if (++fault->calls == fault->failure) return fault->error;
    XrOsIoPolicy p = xr_compile_io_policy(fault->owner); return p.work(p.context,units);
}
static void counter(const char *key, void *value, void *context) { (void)key; (void)value; ++*(size_t *)context; }
static XrOsIoStatus operation(XrHashMap *map, unsigned op) {
    bool b = true; void *v = &canary; size_t count = 0; XrOsIoStatus s;
    switch (op) {
    case 0: return xr_hashmap_owned_set(map,keys[12],(void *)(uintptr_t)14);
    case 1: return xr_hashmap_owned_set(map,keys[0],NULL);
    case 2: s = xr_hashmap_owned_delete(map,keys[1],&b); if(s) CHECK(b); return s;
    case 3: return xr_hashmap_owned_clear(map);
    case 4: s = xr_hashmap_owned_get(map,keys[0],&v); if(s) CHECK(v == &canary); return s;
    case 5: s = xr_hashmap_owned_has(map,keys[1],&b); if(s) CHECK(b); return s;
    default: return xr_hashmap_owned_foreach(map,counter,&count);
    }
}
static size_t one_operation(unsigned op, size_t failure, XrOsIoStatus error) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    FaultPolicy fault = {owner,0,SIZE_MAX,error}; XrOsIoPolicy p = {&fault,fault_alloc,fault_free,fault_work};
    XrHashMap *map = NULL; CHECK(xr_hashmap_owned_new(&p,&map) == XR_OS_IO_OK);
    for (unsigned i = 0; i < 12; ++i) CHECK(xr_hashmap_owned_set(map,keys[i],(void *)(uintptr_t)(i+2)) == XR_OS_IO_OK);
    CHECK(map->capacity == 16); XrHashMapEntry snapshot[16]; memcpy(snapshot,map->entries,sizeof(snapshot));
    XrHashMapEntry *original = map->entries; size_t before = physical_live;
    fault.calls = 0; fault.failure = failure;
    XrOsIoStatus s = operation(map,op); size_t calls = fault.calls;
    if (failure != SIZE_MAX) {
        CHECK(s == error && calls == failure && map->entries == original && map->capacity == 16 && map->count == 12);
        CHECK(memcmp(snapshot,map->entries,sizeof(snapshot)) == 0 && physical_live == before);
        fault.failure = SIZE_MAX; check_entries(map,12);
    } else CHECK(s == XR_OS_IO_OK);
    fault.failure = fault.calls+1; size_t disposed = 0, active = map->count;
    xr_compile_resources_release(owner); xr_hashmap_owned_dispose(map,counter,&disposed);
    CHECK(disposed == active); cleanup(); return calls;
}
static void growth_oom(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL;
    CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy policy = xr_compile_io_policy(owner); XrHashMap *map = NULL;
    CHECK(xr_hashmap_owned_new(&policy,&map) == XR_OS_IO_OK);
    for (unsigned i = 0; i < 12; ++i) CHECK(xr_hashmap_owned_set(map,keys[i],(void *)(uintptr_t)(i+2)) == XR_OS_IO_OK);
    XrHashMapEntry snapshot[16]; memcpy(snapshot,map->entries,sizeof(snapshot));
    XrHashMapEntry *entries = map->entries; XrCompileResourceStats before = stats(owner);
    fail_at = attempts;
    CHECK(xr_hashmap_owned_set(map,keys[12],(void *)(uintptr_t)14) == XR_OS_IO_OUT_OF_MEMORY);
    XrCompileResourceStats after = stats(owner);
    CHECK(map->entries == entries && map->capacity == 16 && map->count == 12 && !memcmp(snapshot,entries,sizeof(snapshot)));
    CHECK(after.live_bytes == before.live_bytes && after.allocated_bytes == before.allocated_bytes && after.work > before.work);
    fail_at = SIZE_MAX; check_entries(map,12);
    CHECK(xr_hashmap_owned_set(map,keys[12],(void *)(uintptr_t)14) == XR_OS_IO_OK && map->capacity == 32);
    check_entries(map,13); xr_compile_resources_release(owner); xr_hashmap_owned_free(map); cleanup();
}
static void collisions(void) {
    reset(SIZE_MAX); XrCompileResources *owner = NULL; CHECK(xr_compile_resources_new(&unlimited,&owner) == XR_COMPILE_RESOURCE_OK);
    XrOsIoPolicy p = xr_compile_io_policy(owner); XrHashMap *map = NULL;
    CHECK(xr_hashmap_owned_new(&p,&map) == XR_OS_IO_OK);
    /* Published FNV-1a collision: both byte strings hash to 0x5e4daa9d. */
    const char *a = "costarring", *b = "liquid";
    CHECK(xr_hashmap_owned_set(map,a,(void *)(uintptr_t)7) == XR_OS_IO_OK);
    CHECK(xr_hashmap_owned_set(map,b,NULL) == XR_OS_IO_OK);
    bool present = false; CHECK(xr_hashmap_owned_has(map,b,&present) == XR_OS_IO_OK && present);
    bool removed = false; CHECK(xr_hashmap_owned_delete(map,a,&removed) == XR_OS_IO_OK && removed);
    CHECK(xr_hashmap_owned_has(map,b,&present) == XR_OS_IO_OK && present);
    void *value = &canary; CHECK(xr_hashmap_owned_get(map,b,&value) == XR_OS_IO_OK && !value);
    CHECK(xr_hashmap_owned_set(map,a,(void *)(uintptr_t)8) == XR_OS_IO_OK && map->count == 2);
    unsigned hashes = 0; for (unsigned i = 0; i < map->capacity; ++i) if (map->entries[i].key) {
        CHECK(map->entries[i].hash == UINT32_C(0x5e4daa9d)); ++hashes;
    } CHECK(hashes == 2);
    CHECK(xr_hashmap_owned_delete(map,"missing",&removed) == XR_OS_IO_OK && !removed);
    xr_compile_resources_release(owner); CHECK(xr_hashmap_owned_get(map,a,&value) == XR_OS_IO_OK && value == (void *)(uintptr_t)8);
    xr_hashmap_owned_free(map); cleanup();
}
int main(void) {
    for (unsigned i = 0; i < 80; ++i) snprintf(keys[i],sizeof(keys[i]),"collision_candidate_%u",i);
    size_t sites = sequence(&unlimited,SIZE_MAX,true); XrCompileResourceStats baseline = measured;
    for (size_t i = 0; i < sites; ++i) sequence(&unlimited,i,false);
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned below = 0; below < 2; ++below) {
        XrCompileResourceLimits limits = unlimited;
        if (!axis) limits.allocated_bytes = baseline.allocated_bytes-below;
        else if (axis == 1) limits.live_bytes = baseline.peak_bytes-below;
        else limits.work = baseline.work-below;
        sequence(&limits,SIZE_MAX,!below);
    }
    size_t boundaries = 0;
    for (unsigned op = 0; op < 7; ++op) {
        size_t calls = one_operation(op,SIZE_MAX,XR_OS_IO_BUDGET);
        const XrOsIoStatus errors[] = {XR_OS_IO_BUDGET,XR_OS_IO_IO,XR_OS_IO_OUT_OF_MEMORY};
        for (size_t e = 0; e < 3; ++e) for (size_t i = 1; i <= calls; ++i) {
            one_operation(op,i,errors[e]); ++boundaries;
        }
    }
    collisions(); growth_oom();
    printf("hashmap: %zu real OOM sites, %zu transactional work failures, 3-axis exact/minus1, FNV collision/tombstone, lifetime and physical zero PASS\n",sites,boundaries);
    return 0;
}
