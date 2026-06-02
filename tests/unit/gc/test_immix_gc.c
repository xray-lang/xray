/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_immix_gc.c - Unit tests for Immix single-bitmap GC
 *
 * KEY CONCEPT:
 *   Tests the single bitmap (alloc_marks only) design with
 *   rebuild_alloc_marks after each GC cycle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../../../src/runtime/gc/xcoro_gc.h"
#include "../../../src/runtime/gc/ximmix.h"
#include "../../../src/runtime/gc/xgc_header.h"
#include "../../../src/runtime/value/xvalue.h"
#include "../../../src/coro/xcoroutine.h"
#include "../test_win_compat.h"

/* Dummy coroutine for GC tests (gc_create only stores gc->owner, no dereference) */
static XrCoroutine dummy_coro;

/* ========== Test Framework ========== */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                                                                 \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  [%02d] %-50s ", tests_run, name);                                                \
        fflush(stdout);                                                                            \
    } while (0)

#define PASS()                                                                                     \
    do {                                                                                           \
        tests_passed++;                                                                            \
        printf("PASS\n");                                                                          \
    } while (0)

#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_failed++;                                                                            \
        printf("FAIL: %s\n", msg);                                                                 \
    } while (0)

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            FAIL(msg);                                                                             \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static uint64_t time_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t) (counter.QuadPart * 1000000000ULL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
#endif
}

/* ========== 1. Immix Block & Bitmap Tests ========== */

static void test_block_init_marks(void) {
    TEST("block init: alloc_marks has line 0 reserved");

    XrImmixHeap heap;
    xr_immix_init(&heap);

    void *p = xr_immix_alloc(&heap, 64);
    ASSERT(p != NULL, "alloc failed");

    XrImmixBlock *block = heap.current_block;
    ASSERT(block != NULL, "no current block");

    ASSERT(block->alloc_marks[0] & 1ULL, "alloc_marks line 0 not set");

    xr_immix_destroy(&heap);
    PASS();
}

static void test_mark_alloc_lines(void) {
    TEST("mark_alloc_lines: sets alloc_marks correctly");

    XrImmixHeap heap;
    xr_immix_init(&heap);

    void *p = xr_immix_alloc(&heap, 64);
    ASSERT(p != NULL, "alloc failed");

    XrImmixBlock *block = heap.current_block;

    // Clear alloc_marks (except line 0)
    block->alloc_marks[0] = 1ULL;
    block->alloc_marks[1] = 0;

    xr_immix_mark_alloc_lines(p, 64);

    int line = XR_IMMIX_LINE_INDEX(p);
    ASSERT(XR_IMMIX_LINE_GET(block->alloc_marks, line), "alloc_marks not set");

    xr_immix_destroy(&heap);
    PASS();
}

static void test_mark_alloc_lines_fast(void) {
    TEST("mark_alloc_lines_fast: inline fast path works");

    XrImmixHeap heap;
    xr_immix_init(&heap);

    void *p = xr_immix_alloc(&heap, 64);
    ASSERT(p != NULL, "alloc failed");

    XrImmixBlock *block = heap.current_block;

    block->alloc_marks[0] = 1ULL;
    block->alloc_marks[1] = 0;

    xr_immix_mark_alloc_lines_fast(p, 64);

    int line = XR_IMMIX_LINE_INDEX(p);
    ASSERT(XR_IMMIX_LINE_GET(block->alloc_marks, line), "alloc_marks not set by fast path");

    xr_immix_destroy(&heap);
    PASS();
}

static void test_hole_scanning_uses_alloc_marks(void) {
    TEST("hole scanning: uses alloc_marks for free line detection");

    XrImmixHeap heap;
    xr_immix_init(&heap);

    // Allocate and mark alloc_marks
    for (int i = 0; i < 80; i++) {
        void *p = xr_immix_alloc(&heap, 128);
        xr_immix_mark_alloc_lines(p, 128);
    }

    XrImmixBlock *block = heap.current_block;
    ASSERT(block != NULL, "no current block");

    uint64_t alloc_bits = (block->alloc_marks[0] & ~1ULL) | block->alloc_marks[1];
    ASSERT(alloc_bits != 0, "alloc_marks should have live lines");

    xr_immix_destroy(&heap);
    PASS();
}

/* ========== 2. CoroGC Integration Tests ========== */

static void test_gc_create_destroy(void) {
    TEST("CoroGC create/destroy");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");
    ASSERT(gc->totalbytes == 0, "initial bytes should be 0");

    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_newobj_marks_alloc(void) {
    TEST("newobj: marks alloc_marks for new object");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");

    XrGCHeader *obj = xr_coro_gc_newobj(gc, XR_TBLOB, 64);
    ASSERT(obj != NULL, "alloc failed");

    XrImmixBlock *block = XR_IMMIX_BLOCK_FROM_PTR(obj);
    int line = XR_IMMIX_LINE_INDEX(obj);

    ASSERT(XR_IMMIX_LINE_GET(block->alloc_marks, line), "alloc_marks not set for new obj");

    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_large_object(void) {
    TEST("large object: goes to malloc, not Immix");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");

    XrGCHeader *large = xr_coro_gc_newobj(gc, XR_TSTRING, 8 * 1024);
    ASSERT(large != NULL, "large alloc failed");
    ASSERT(gc->large_objects == large, "should be in large_objects list");

    xr_coro_gc_destroy(gc);
    PASS();
}

/* ========== 4. Performance Tests ========== */

static void perf_allocation_throughput(void) {
    TEST("perf: allocation throughput");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    gc->gc_disabled++;

    const int COUNT = 100000;
    uint64_t start = time_ns();

    for (int i = 0; i < COUNT; i++) {
        xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    }

    uint64_t elapsed = time_ns() - start;
    double ms = elapsed / 1e6;
    double mps = COUNT / (ms / 1000.0) / 1e6;

    gc->gc_disabled--;
    printf("%.1fM/s (%.1fms) ", mps, ms);
    xr_coro_gc_destroy(gc);
    PASS();
}

static void perf_bulk_destroy(void) {
    TEST("perf: bulk destroy (coroutine exit)");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    gc->gc_disabled++;

    const int COUNT = 100000;
    for (int i = 0; i < COUNT; i++) {
        xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    }
    gc->gc_disabled--;

    XrImmixStats stats;
    xr_immix_get_stats(&gc->immix, &stats);

    uint64_t start = time_ns();
    xr_coro_gc_destroy(gc);
    uint64_t elapsed = time_ns() - start;

    printf("%.0fus (%zu blocks) ", elapsed / 1e3, stats.total_blocks);
    PASS();
}

/* ========== Main ========== */

int main(void) {
    xr_test_suppress_dialogs();
    printf("\n========================================\n");
    printf("  Immix Single-Bitmap GC Unit Tests\n");
    printf("========================================\n\n");

    printf("--- Immix Bitmap ---\n");
    test_block_init_marks();
    test_mark_alloc_lines();
    test_mark_alloc_lines_fast();
    test_hole_scanning_uses_alloc_marks();

    printf("\n--- CoroGC Integration ---\n");
    test_gc_create_destroy();
    test_newobj_marks_alloc();
    test_large_object();
    /* Tracing collector (fullgc / incremental step / sweep / write barriers)
     * has been removed: reference counting owns reclamation. The tests that
     * asserted tracing-cycle behavior (gc_count increment, PROPAGATE state
     * walk, sweep-driven totalbytes decrease, tri-color barriers) are gone
     * with it. What remains here covers the Immix bump allocator + alloc-mark
     * bookkeeping, which RC still relies on for region allocation. */

    printf("\n--- Performance ---\n");
    perf_allocation_throughput();
    perf_bulk_destroy();

    printf("\n========================================\n");
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf(", %d FAILED", tests_failed);
    printf("\n========================================\n\n");

    return tests_failed == 0 ? 0 : 1;
}
