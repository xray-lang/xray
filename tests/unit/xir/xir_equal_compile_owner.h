/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_equal_compile_owner.h - Separate physical compiler ownership for equality
 *
 * KEY CONCEPT:
 *   Immutable type metadata uses a finite compiler ledger independent of
 *   runtime value scratch, and its real allocator is observed separately.
 */
#ifndef XIR_EQUAL_COMPILE_OWNER_H
#define XIR_EQUAL_COMPILE_OWNER_H
#include "base/xmalloc.h"
#include "xir_source_fixture_owner.h"
typedef struct EqualCompileRecord { void *pointer; size_t bytes; } EqualCompileRecord;
static EqualCompileRecord *equal_compile_records;
static size_t equal_compile_live, equal_compile_bytes, equal_compile_capacity;
static SourceFixtureOwner equal_owner;
/* The bookkeeping buffer is outside the implementation allocation boundary. */
static void *equal_compile_malloc(size_t bytes) {
    void *pointer = xr_malloc(bytes);
    if (!pointer) return NULL;
    if (equal_compile_live == equal_compile_capacity) {
        CHECK(equal_compile_capacity <= SIZE_MAX / 2 / sizeof(*equal_compile_records));
        size_t capacity = equal_compile_capacity ? equal_compile_capacity * 2 : 64;
        EqualCompileRecord *grown = xr_realloc(equal_compile_records, capacity * sizeof(*grown));
        CHECK(grown); equal_compile_records = grown; equal_compile_capacity = capacity;
    }
    CHECK(bytes <= SIZE_MAX - equal_compile_bytes);
    equal_compile_records[equal_compile_live++] = (EqualCompileRecord) {pointer, bytes};
    equal_compile_bytes += bytes; return pointer;
}
static void equal_compile_free(void *pointer) {
    if (!pointer) return;
    size_t index = 0;
    while (index < equal_compile_live && equal_compile_records[index].pointer != pointer) ++index;
    CHECK(index < equal_compile_live && equal_compile_records[index].bytes <= equal_compile_bytes);
    equal_compile_bytes -= equal_compile_records[index].bytes;
    equal_compile_records[index] = equal_compile_records[--equal_compile_live]; xr_free(pointer);
    if (!equal_compile_live) {
        CHECK(!equal_compile_bytes);
        xr_free(equal_compile_records); equal_compile_records = NULL; equal_compile_capacity = 0;
    }
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) equal_compile_malloc(bytes)
#define xr_free(pointer) equal_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
#endif // XIR_EQUAL_COMPILE_OWNER_H
