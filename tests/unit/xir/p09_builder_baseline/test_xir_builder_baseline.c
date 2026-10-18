/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_builder_baseline.c - Measure the original AOT builder and allocator
 *
 * KEY CONCEPT:
 *   Instrumentation delegates storage to xr_malloc and enters the real arena.
 *   It observes explicit copies; realloc copying remains provider opaque.
 */
#include "base/xmalloc.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Baseline assertion failed at %d: %s\n", __LINE__, #condition); exit(1); } } while (0)
typedef struct { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[64];
static size_t attempts, allocation_count, resize_count, requested, live, peak;
static size_t copy_bytes, resize_copy_bound, fail_at = SIZE_MAX;

static size_t allocation_slot(void *pointer) {
    for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i)
        if (allocations[i].pointer == pointer) return i;
    CHECK(false);
    return 0;
}
static void allocation_record(size_t slot, void *pointer, size_t bytes) {
    CHECK(pointer && bytes <= SIZE_MAX - live && bytes <= SIZE_MAX - requested);
    allocations[slot] = (Allocation){pointer, bytes};
    requested += bytes; live += bytes;
    if (live > peak) peak = live;
}
static void *baseline_malloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *pointer = xr_malloc(bytes);
    if (pointer) { allocation_record(allocation_slot(NULL), pointer, bytes); ++allocation_count; }
    return pointer;
}
static void *baseline_calloc(size_t count, size_t bytes) {
    CHECK(!count || bytes <= SIZE_MAX / count);
    if (attempts++ == fail_at) return NULL;
    void *pointer = xr_calloc(count, bytes);
    if (pointer) { allocation_record(allocation_slot(NULL), pointer, count * bytes); ++allocation_count; }
    return pointer;
}
static void *baseline_realloc(void *pointer, size_t bytes) {
    if (!pointer) return baseline_malloc(bytes);
    if (attempts++ == fail_at) return NULL;
    size_t slot = allocation_slot(pointer), old_bytes = allocations[slot].bytes;
    void *replacement = xr_realloc(pointer, bytes);
    if (replacement) {
        CHECK(live >= old_bytes && old_bytes <= SIZE_MAX - resize_copy_bound);
        live -= old_bytes; resize_copy_bound += old_bytes;
        allocation_record(slot, replacement, bytes); ++resize_count;
    }
    return replacement;
}
static void baseline_free(void *pointer) {
    if (!pointer) return;
    size_t slot = allocation_slot(pointer);
    CHECK(live >= allocations[slot].bytes);
    live -= allocations[slot].bytes; allocations[slot] = (Allocation){0};
    xr_free(pointer);
}
static void *baseline_memcpy(void *destination, const void *source, size_t bytes) {
    CHECK(bytes <= SIZE_MAX - copy_bytes);
    copy_bytes += bytes;
    return memcpy(destination, source, bytes);
}

#define XRT_IMPL
#define XRT_MALLOC baseline_malloc
#define XRT_CALLOC baseline_calloc
#define XRT_REALLOC baseline_realloc
#define XRT_FREE baseline_free
#define memcpy baseline_memcpy
#include "aot/xrt_coll.h"
#include "aot/xrt_arith.h"
#undef memcpy

static double wall_seconds(void) {
    struct timespec value;
    CHECK(timespec_get(&value, TIME_UTC) == TIME_UTC);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}
static void check_text(XrValue value, size_t length, bool digits) {
    CHECK(xr_str_len(value) >= 0 && (size_t)xr_str_len(value) == length);
    const char *text = xr_str_data(value);
    CHECK(text && text[length] == 0);
    for (size_t i = 0; i < length; ++i)
        CHECK(text[i] == (digits ? (char)('0' + i % 10) : 'x'));
}
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return 2;
    const char *scenario = argv[1];
    bool digits = !strcmp(scenario, "digits"), chunks = !strcmp(scenario, "chunks");
    bool reserve = !strcmp(scenario, "reserve"), join = !strcmp(scenario, "join");
    bool snapshot = !strcmp(scenario, "snapshot");
    if (!digits && !chunks && !reserve && !join && !snapshot && strcmp(scenario, "short")) return 2;
    if (argc == 3) {
        char *end = NULL; unsigned long long parsed = strtoull(argv[2], &end, 10);
        if (!end || *end || parsed > SIZE_MAX) return 2;
        fail_at = (size_t)parsed;
    }
    void *arena = xrt_execution_arena_new();
    if (!arena) { puts("baseline arena OOM"); return 3; }
    void *previous = xrt_execution_arena_enter(arena);
    size_t piece_length = chunks ? 16384 : join ? 1000 : 1;
    XrValue piece = xrt_str_alloc(piece_length);
    memset(xr_str_buf(piece), 'x', piece_length);
    XrValue builder = xrt_strbuf_new(), old = {0};
    size_t count = chunks ? 64 : join ? 1000 : snapshot ? 4096 : 1000000;
    clock_t cpu_start = clock(); double wall_start = wall_seconds();
    if (reserve || join) xrt_strbuf_reserve_extra_exact(builder, (int64_t)(count * piece_length));
    for (size_t i = 0; i < count; ++i)
        xrt_strbuf_append(builder, digits ? XR_FROM_INT(i % 10) : piece);
    if (snapshot) {
        old = xrt_strbuf_finish(builder);
        for (size_t i = 0; i < 64; ++i) xrt_strbuf_append(builder, piece);
    }
    XrValue result = xrt_strbuf_finish(builder);
    double wall = wall_seconds() - wall_start, cpu = (double)(clock() - cpu_start) / CLOCKS_PER_SEC;
    size_t length = count * piece_length + (snapshot ? 64 : 0);
    check_text(result, length, digits);
    if (snapshot) check_text(old, count, false);
    size_t final_live = live;
    xrt_release(result);
    if (snapshot) xrt_release(old);
    xrt_release(builder); xrt_release(piece);
    xrt_execution_arena_restore(previous); xrt_execution_arena_destroy(arena);
    CHECK(live == 0);
    printf("{\"scenario\":\"%s\",\"result\":%zu,\"allocation_attempts\":%zu,"
        "\"allocations\":%zu,\"reallocations\":%zu,\"requested\":%zu,\"peak\":%zu,"
        "\"live_before_drop\":%zu,\"live_after_drop\":%zu,\"explicit_copy_bytes\":%zu,"
        "\"realloc_copy_upper_bound\":%zu,\"wall_seconds\":%.9f,\"cpu_seconds\":%.9f,"
        "\"physical_allocator\":\"xr_malloc\",\"execution_arena\":\"actual\","
        "\"qualification\":\"BASELINE_MEASUREMENT_ONLY\"}\n",
        scenario, length, attempts, allocation_count, resize_count, requested, peak,
        final_live, live, copy_bytes, resize_copy_bound, wall, cpu);
    return 0;
}
