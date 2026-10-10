/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * parser_resources_observer.h - Actual compiler allocator fault observation
 *
 * The including test defines CHECK. Include once per test translation unit;
 * the production ledger implementation observes actual allocation/free calls.
 */
#ifndef XR_TEST_PARSER_RESOURCES_OBSERVER_H
#define XR_TEST_PARSER_RESOURCES_OBSERVER_H
#include "base/xmalloc.h"
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[8192];
static size_t physical_live, physical_peak, physical_total, allocation_count, attempt_count;
static size_t fail_at = SIZE_MAX;

static void *observed_malloc(size_t bytes) {
    if (attempt_count++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t slot = 0;
    while (slot < 8192 && allocations[slot].pointer) ++slot;
    CHECK(slot < 8192);
    allocations[slot] = (Allocation) {memory, bytes};
    physical_live += bytes;
    physical_total += bytes;
    if (physical_live > physical_peak) physical_peak = physical_live;
    ++allocation_count;
    return memory;
}

static void observed_free(void *memory) {
    if (!memory) return;
    size_t slot = 0;
    while (slot < 8192 && allocations[slot].pointer != memory) ++slot;
    CHECK(slot < 8192);
    physical_live -= allocations[slot].bytes;
    --allocation_count;
    allocations[slot] = (Allocation) {0};
    xr_free(memory);
}

#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_malloc(bytes)
#define xr_free(memory) observed_free(memory)
#include "base/xcompile_resources.c"

static void reset(void) {
    CHECK(!physical_live && !allocation_count);
    physical_peak = physical_total = attempt_count = 0;
    fail_at = SIZE_MAX;
}

#endif
