/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_source_nested_compile_owner.h - Independent compiler physical ownership
 *
 * KEY CONCEPT:
 *   Compiler ledger blocks and runtime allocation blocks have separate observers.
 */
#ifndef XIR_SOURCE_NESTED_NULLABLE_COMPILE_OWNER_H
#define XIR_SOURCE_NESTED_NULLABLE_COMPILE_OWNER_H
#include "base/xmalloc.h"
#include "xir_source_fixture_owner.h"
typedef struct SourceNestedCompileAllocation { void *pointer; size_t bytes; } SourceNestedCompileAllocation;
static SourceNestedCompileAllocation source_nested_compile_allocations[32768];
static size_t source_nested_compile_live, source_nested_compile_bytes, source_nested_compile_attempts;
static size_t source_nested_compile_fail_at=SIZE_MAX;
static bool source_nested_compile_injected;
/* Shards report every completed physical-zero ordinal in their own JSONL. */
static bool source_nested_compile_report=true;
static size_t source_nested_compile_slot(const void *pointer) {
    uint64_t key = (uint64_t)(uintptr_t)pointer;
    key ^= key >> 33; key *= UINT64_C(0xff51afd7ed558ccd); key ^= key >> 33;
    return (size_t)key & 32767;
}
static void *source_nested_compile_malloc(size_t bytes) {
    if (source_nested_compile_attempts++==source_nested_compile_fail_at) {
        source_nested_compile_injected=true; return NULL;
    }
    void *pointer = xr_malloc(bytes);
    if (!pointer) return NULL;
    CHECK(source_nested_compile_live < 16384 && bytes <= SIZE_MAX - source_nested_compile_bytes);
    size_t slot = source_nested_compile_slot(pointer);
    while (source_nested_compile_allocations[slot].pointer) slot = (slot + 1) & 32767;
    source_nested_compile_allocations[slot] = (SourceNestedCompileAllocation){pointer, bytes};
    ++source_nested_compile_live; source_nested_compile_bytes += bytes;
    return pointer;
}
static void source_nested_compile_free(void *pointer) {
    if (!pointer) return;
    size_t hole = source_nested_compile_slot(pointer);
    while (source_nested_compile_allocations[hole].pointer != pointer) {
        CHECK(source_nested_compile_allocations[hole].pointer); hole = (hole + 1) & 32767;
    }
    CHECK(source_nested_compile_live && source_nested_compile_bytes >= source_nested_compile_allocations[hole].bytes);
    --source_nested_compile_live; source_nested_compile_bytes -= source_nested_compile_allocations[hole].bytes;
    for (size_t next = (hole + 1) & 32767; source_nested_compile_allocations[next].pointer; next = (next + 1) & 32767) {
        size_t home = source_nested_compile_slot(source_nested_compile_allocations[next].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { source_nested_compile_allocations[hole] = source_nested_compile_allocations[next]; hole = next; }
    }
    source_nested_compile_allocations[hole] = (SourceNestedCompileAllocation){0}; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) source_nested_compile_malloc(bytes)
#define xr_free(pointer) source_nested_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
static void source_nested_compile_owner_free(SourceFixtureOwner *owner) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(owner->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.allocated_bytes <= UINT64_C(64) * 1024 * 1024 &&
        stats.peak_bytes <= UINT64_C(8) * 1024 * 1024 && stats.work <= UINT64_C(128000000));
    CHECK(stats.live_bytes==owner->baseline.live_bytes);
    CHECK(stats.allocated_bytes>=owner->baseline.allocated_bytes && stats.work>=owner->baseline.work);
    if (source_nested_compile_report) fprintf(stderr,"nested compiler ledger: work=%llu allocated=%llu peak=%llu live=%llu\n",
        (unsigned long long)stats.work,(unsigned long long)stats.allocated_bytes,
        (unsigned long long)stats.peak_bytes,(unsigned long long)stats.live_bytes);
    xr_compile_resources_release(owner->context.resources); *owner=(SourceFixtureOwner){0};
    CHECK(!source_nested_compile_live && !source_nested_compile_bytes);
    if (source_nested_compile_report) printf("nested nullable compiler %zu actual allocation attempts; physical blocks/bytes=0/0\n",
        source_nested_compile_attempts);
}
#endif // XIR_SOURCE_NESTED_NULLABLE_COMPILE_OWNER_H
