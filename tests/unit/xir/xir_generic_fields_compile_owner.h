/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generic_fields_compile_owner.h - Independent compiler physical ownership
 *
 * KEY CONCEPT:
 *   Compiler ledger blocks and runtime allocation blocks have separate observers.
 */
#ifndef XIR_GENERIC_FIELDS_COMPILE_OWNER_H
#define XIR_GENERIC_FIELDS_COMPILE_OWNER_H
#include "base/xmalloc.h"
#include "xir_source_fixture_owner.h"
typedef struct GenericFieldsCompileAllocation { void *pointer; size_t bytes; } GenericFieldsCompileAllocation;
static GenericFieldsCompileAllocation generic_fields_compile_allocations[32768];
static size_t generic_fields_compile_live, generic_fields_compile_bytes, generic_fields_compile_attempts;
static size_t generic_fields_compile_slot(const void *pointer) {
    uint64_t key = (uint64_t)(uintptr_t)pointer;
    key ^= key >> 33; key *= UINT64_C(0xff51afd7ed558ccd); key ^= key >> 33;
    return (size_t)key & 32767;
}
static void *generic_fields_compile_malloc(size_t bytes) {
    ++generic_fields_compile_attempts;
    void *pointer = xr_malloc(bytes);
    if (!pointer) return NULL;
    CHECK(generic_fields_compile_live < 16384 && bytes <= SIZE_MAX - generic_fields_compile_bytes);
    size_t slot = generic_fields_compile_slot(pointer);
    while (generic_fields_compile_allocations[slot].pointer) slot = (slot + 1) & 32767;
    generic_fields_compile_allocations[slot] = (GenericFieldsCompileAllocation){pointer, bytes};
    ++generic_fields_compile_live; generic_fields_compile_bytes += bytes;
    return pointer;
}
static void generic_fields_compile_free(void *pointer) {
    if (!pointer) return;
    size_t hole = generic_fields_compile_slot(pointer);
    while (generic_fields_compile_allocations[hole].pointer != pointer) {
        CHECK(generic_fields_compile_allocations[hole].pointer); hole = (hole + 1) & 32767;
    }
    CHECK(generic_fields_compile_live && generic_fields_compile_bytes >= generic_fields_compile_allocations[hole].bytes);
    --generic_fields_compile_live; generic_fields_compile_bytes -= generic_fields_compile_allocations[hole].bytes;
    for (size_t next = (hole + 1) & 32767; generic_fields_compile_allocations[next].pointer; next = (next + 1) & 32767) {
        size_t home = generic_fields_compile_slot(generic_fields_compile_allocations[next].pointer);
        bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
        if (!stays) { generic_fields_compile_allocations[hole] = generic_fields_compile_allocations[next]; hole = next; }
    }
    generic_fields_compile_allocations[hole] = (GenericFieldsCompileAllocation){0}; xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) generic_fields_compile_malloc(bytes)
#define xr_free(pointer) generic_fields_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
static void generic_fields_compile_owner_free(SourceFixtureOwner *owner) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(owner->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.allocated_bytes <= UINT64_C(64) * 1024 * 1024 &&
        stats.peak_bytes <= UINT64_C(8) * 1024 * 1024 && stats.work <= UINT64_C(128000000));
    source_fixture_owner_free(owner);
    CHECK(!generic_fields_compile_live && !generic_fields_compile_bytes);
    printf("generic fields compiler %zu actual allocation attempts; physical blocks/bytes=0/0\n",
        generic_fields_compile_attempts);
}
#endif // XIR_GENERIC_FIELDS_COMPILE_OWNER_H
