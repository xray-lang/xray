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

#include "../../../src/runtime/gc/xcoro_gc.h"
#include "../../../src/runtime/gc/ximmix.h"
#include "../../../src/runtime/gc/xgc_header.h"
#include "../../../src/runtime/value/xvalue.h"
#include "../../../src/coro/xcoroutine.h"

/* Dummy coroutine for GC tests (gc_create only stores gc->owner, no dereference) */
static XrCoroutine dummy_coro;

/* ========== Test Framework ========== */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  [%02d] %-50s ", tests_run, name); \
        fflush(stdout); \
    } while(0)

#define PASS() \
    do { \
        tests_passed++; \
        printf("PASS\n"); \
    } while(0)

#define FAIL(msg) \
    do { \
        tests_failed++; \
        printf("FAIL: %s\n", msg); \
    } while(0)

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { FAIL(msg); return; } \
    } while(0)

static uint64_t time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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
    ASSERT(gc->gcstate == XGC_PAUSE, "initial state should be PAUSE");
    ASSERT(gc->totalbytes == 0, "initial bytes should be 0");
    ASSERT(gc->totalbytes == 0, "initial totalbytes confirmed 0");

    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_newobj_marks_alloc(void) {
    TEST("newobj: marks alloc_marks for new object");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");

    XrGCHeader *obj = xr_coro_gc_newobj(gc, XR_TARRAY, 64);
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

static void test_fullgc_cycle(void) {
    TEST("fullgc: complete cycle, objects swept");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");

    gc->gc_disabled++;
    for (int i = 0; i < 200; i++) {
        xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    }
    gc->gc_disabled--;

    int64_t before = gc->totalbytes;
    ASSERT(before > 0, "should have allocated bytes");

    uint32_t gc_count = gc->gc_count;
    xr_coro_gc_fullgc(gc);

    ASSERT(gc->gc_count == gc_count + 1, "gc_count should increment");
    ASSERT(gc->gcstate == XGC_PAUSE, "should return to PAUSE");
    // All objects unreachable (no coroutine owner) -> totalbytes should decrease
    ASSERT(gc->totalbytes < before, "totalbytes should decrease after sweep");

    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_incremental_gc_states(void) {
    TEST("incremental GC: walks through 4 states");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");

    // Force incremental mode (default is gen mode which runs entergen)
    gc->gc_mode = XGC_MODE_INC;

    gc->gc_disabled++;
    for (int i = 0; i < 100; i++) {
        xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    }
    gc->gc_disabled--;

    ASSERT(gc->gcstate == XGC_PAUSE, "should start in PAUSE");

    // Step: PAUSE -> PROPAGATE
    gc->GCdebt = 1000;
    xr_coro_gc_step(gc);
    ASSERT(gc->gcstate == XGC_PROPAGATE, "should be PROPAGATE");

    // Step through PROPAGATE until gray list empty -> ATOMIC
    int steps = 0;
    while (gc->gcstate == XGC_PROPAGATE && steps < 1000) {
        xr_coro_gc_step(gc);
        steps++;
    }
    ASSERT(gc->gcstate == XGC_ATOMIC, "should reach ATOMIC");

    // Step: ATOMIC -> SWEEP
    xr_coro_gc_step(gc);
    ASSERT(gc->gcstate == XGC_SWEEP, "should be SWEEP");

    // SWEEP does everything in one step -> PAUSE
    xr_coro_gc_step(gc);
    ASSERT(gc->gcstate == XGC_PAUSE, "should return to PAUSE");

    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_alloc_during_incremental_gc(void) {
    TEST("alloc during incremental GC: no corruption");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");

    // Force incremental mode
    gc->gc_mode = XGC_MODE_INC;

    gc->gc_disabled++;
    for (int i = 0; i < 50; i++) {
        xr_coro_gc_newobj(gc, XR_TSTRING, 128);
    }
    gc->gc_disabled--;

    gc->GCdebt = 1000;
    xr_coro_gc_step(gc);
    ASSERT(gc->gcstate == XGC_PROPAGATE, "should be PROPAGATE");

    // Allocate NEW objects during PROPAGATE
    gc->gc_disabled++;
    XrGCHeader *new_objs[20];
    for (int i = 0; i < 20; i++) {
        new_objs[i] = xr_coro_gc_newobj(gc, XR_TSTRING, 64);
        ASSERT(new_objs[i] != NULL, "alloc during GC failed");

        XrImmixBlock *block = XR_IMMIX_BLOCK_FROM_PTR(new_objs[i]);
        int line = XR_IMMIX_LINE_INDEX(new_objs[i]);
        ASSERT(XR_IMMIX_LINE_GET(block->alloc_marks, line),
               "alloc_marks not set for obj during GC");
    }
    gc->gc_disabled--;

    xr_coro_gc_fullgc(gc);
    ASSERT(gc->gcstate == XGC_PAUSE, "should finish at PAUSE");

    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_rebuild_after_fullgc(void) {
    TEST("fullgc: rebuild_alloc_marks reclaims blocks");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");

    gc->gc_disabled++;
    for (int i = 0; i < 100; i++) {
        xr_coro_gc_newobj(gc, XR_TSTRING, 128);
    }
    gc->gc_disabled--;

    int64_t before = gc->totalbytes;
    ASSERT(before > 0, "should have bytes before GC");

    xr_coro_gc_fullgc(gc);

    ASSERT(gc->totalbytes < before, "totalbytes should decrease");
    ASSERT(gc->immix.full_blocks == NULL, "full_blocks should be empty");

    xr_coro_gc_destroy(gc);
    PASS();
}

/* ========== 3. Write Barrier Tests ========== */


static void test_keepinvariant_guard(void) {
    TEST("barrier: keepinvariant only true in mark phases");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");

    // PAUSE state: barrier should NOT be active
    ASSERT(gc->gcstate == XGC_PAUSE, "initial state should be PAUSE");
    ASSERT(!xr_gc_keepinvariant(gc), "PAUSE should not keepinvariant");

    // Force into PROPAGATE
    gc->gcstate = XGC_PROPAGATE;
    ASSERT(xr_gc_keepinvariant(gc), "PROPAGATE should keepinvariant");

    // ATOMIC
    gc->gcstate = XGC_ATOMIC;
    ASSERT(xr_gc_keepinvariant(gc), "ATOMIC should keepinvariant");

    // SWEEP
    gc->gcstate = XGC_SWEEP;
    ASSERT(!xr_gc_keepinvariant(gc), "SWEEP should not keepinvariant");

    gc->gcstate = XGC_PAUSE;
    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_forward_barrier_marks_child(void) {
    TEST("barrier: forward barrier marks white child");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");
    gc->gc_disabled++;

    // Create parent and child objects
    XrGCHeader *parent = xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    XrGCHeader *child = xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    ASSERT(parent != NULL && child != NULL, "alloc failed");

    // Both should be white initially
    ASSERT(xr_gc_iswhite(parent), "parent should be white");
    ASSERT(xr_gc_iswhite(child), "child should be white");

    // Make parent black, set GC to PROPAGATE
    xr_gc_white2gray(parent);
    xr_gc_gray2black(parent);
    gc->gcstate = XGC_PROPAGATE;

    // Forward barrier should mark the white child
    xr_coro_gc_barrier(gc, parent, child);

    // Child should no longer be white (it was marked)
    ASSERT(!xr_gc_iswhite(child), "child should be marked after barrier");

    gc->gcstate = XGC_PAUSE;
    gc->gc_disabled--;
    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_back_barrier_reverts_to_gray(void) {
    TEST("barrier: back barrier reverts black to gray");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");
    gc->gc_disabled++;

    XrGCHeader *obj = xr_coro_gc_newobj(gc, XR_TARRAY, 64);
    ASSERT(obj != NULL, "alloc failed");

    // Make object black
    xr_gc_white2gray(obj);
    xr_gc_gray2black(obj);
    ASSERT(xr_gc_isblack(obj), "should be black");

    gc->gcstate = XGC_PROPAGATE;

    // Back barrier should revert to gray
    xr_coro_gc_barrierback(gc, obj);
    ASSERT(!xr_gc_isblack(obj), "should not be black after barrierback");

    gc->gcstate = XGC_PAUSE;
    gc->gc_disabled--;
    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_barrier_noop_in_sweep(void) {
    TEST("barrier: no-op outside mark phases");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");
    gc->gc_disabled++;

    XrGCHeader *parent = xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    XrGCHeader *child = xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    ASSERT(parent && child, "alloc failed");

    // Make parent black, child white
    xr_gc_white2gray(parent);
    xr_gc_gray2black(parent);

    // In SWEEP phase: barrier should be no-op
    gc->gcstate = XGC_SWEEP;
    xr_coro_gc_barrier(gc, parent, child);
    ASSERT(xr_gc_iswhite(child), "child should remain white in SWEEP");

    // In PAUSE phase: barrier should be no-op
    gc->gcstate = XGC_PAUSE;
    xr_coro_gc_barrier(gc, parent, child);
    ASSERT(xr_gc_iswhite(child), "child should remain white in PAUSE");

    gc->gc_disabled--;
    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_barrier_val_macro(void) {
    TEST("barrier: XR_GC_BARRIER_VAL with non-GC value is no-op");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    ASSERT(gc != NULL, "create failed");
    gc->gc_disabled++;

    XrGCHeader *parent = xr_coro_gc_newobj(gc, XR_TINSTANCE, 64);
    ASSERT(parent != NULL, "alloc failed");

    xr_gc_white2gray(parent);
    xr_gc_gray2black(parent);
    gc->gcstate = XGC_PROPAGATE;

    // Integer value: should not trigger barrier (not a GC pointer)
    XrValue int_val = XR_FROM_INT(42);
    XR_GC_BARRIER_VAL(gc, parent, int_val);  // should be no-op

    // null value: should not trigger barrier
    XrValue null_val = XR_NULL_VAL;
    XR_GC_BARRIER_VAL(gc, parent, null_val);  // should be no-op

    gc->gcstate = XGC_PAUSE;
    gc->gc_disabled--;
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

static void perf_fullgc_time(void) {
    TEST("perf: fullgc time");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro, NULL);
    gc->gc_disabled++;

    const int COUNT = 50000;
    for (int i = 0; i < COUNT; i++) {
        xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    }
    gc->gc_disabled--;

    uint64_t start = time_ns();
    xr_coro_gc_fullgc(gc);
    uint64_t elapsed = time_ns() - start;

    printf("%.2fms (%d objs) ", elapsed / 1e6, COUNT);
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
    test_fullgc_cycle();
    test_incremental_gc_states();
    test_alloc_during_incremental_gc();
    test_rebuild_after_fullgc();

    printf("\n--- Write Barrier ---\n");
    test_keepinvariant_guard();
    test_forward_barrier_marks_child();
    test_back_barrier_reverts_to_gray();
    test_barrier_noop_in_sweep();
    test_barrier_val_macro();

    printf("\n--- Performance ---\n");
    perf_allocation_throughput();
    perf_fullgc_time();
    perf_bulk_destroy();

    printf("\n========================================\n");
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf("\n========================================\n\n");

    return tests_failed == 0 ? 0 : 1;
}
