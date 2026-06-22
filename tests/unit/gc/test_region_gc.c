/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_region_gc.c - Unit tests for the Region bump allocator (pure RC)
 *
 * KEY CONCEPT:
 *   Region is a bump allocator (no per-line bitmap under RC): objects start
 *   at line 1, per-block alloc_count/alloc_bytes drive whole-block reclaim,
 *   and reclamation of individual objects is the per-coroutine RC freelist.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../../../src/runtime/gc/xcoro_gc.h"
#include "../../../src/runtime/gc/xregion.h"
#include "../../../src/runtime/gc/xgc_header.h"
#include "../../../src/runtime/value/xvalue.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../src/coro/xcoroutine.h"
#include "../test_win_compat.h"

/* Dummy coroutine for GC tests (gc_create only stores gc->owner, no dereference) */
static XrCoroutine dummy_coro;
static XrayIsolate g_test_iso;
static XrRuntimeCore g_test_core;

#define XR_TEST_EXT_TYPE 63

static int g_destroy_count = 0;

static void test_ext_destroy(XrObjHeader *obj, XrCoroGC *owning_gc) {
    (void) obj;
    (void) owning_gc;
    g_destroy_count++;
}

static void test_ext_destroy_api(XrObjHeader *obj, void *owning_gc) {
    test_ext_destroy(obj, (XrCoroGC *) owning_gc);
}

static void setup_test_ext_finalizer(void) {
    memset(&g_test_iso, 0, sizeof(g_test_iso));
    memset(&g_test_core, 0, sizeof(g_test_core));
    g_test_iso.core_rt = &g_test_core;
    xr_register_extension_destroy(&g_test_iso, XR_TEST_EXT_TYPE, test_ext_destroy_api);
    dummy_coro.isolate = &g_test_iso;
}

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

/* ========== 1. Region Bump Allocator Tests ==========
 *
 * Pure RC: there is no per-line occupancy bitmap. The region is a plain
 * bump allocator (objects start at line 1; line 0 holds block metadata),
 * with per-block alloc_count/alloc_bytes driving whole-block reclaim. */

static void test_region_bump_alloc(void) {
    TEST("region: bump allocation advances within one block");

    XrRegionHeap heap;
    xr_region_init(&heap);

    void *p1 = xr_region_alloc(&heap, 64);
    void *p2 = xr_region_alloc(&heap, 64);
    ASSERT(p1 != NULL && p2 != NULL, "alloc failed");
    ASSERT((char *) p2 > (char *) p1, "bump pointer should advance");
    ASSERT(((uintptr_t) p1 & 7) == 0, "pointer not 8-byte aligned");

    XrRegionBlock *b1 = XR_REGION_BLOCK_FROM_PTR(p1);
    XrRegionBlock *b2 = XR_REGION_BLOCK_FROM_PTR(p2);
    ASSERT(b1 == b2 && b1 == heap.current_block, "both allocs in current block");

    xr_region_destroy(&heap);
    PASS();
}

static void test_region_objects_skip_metadata_line(void) {
    TEST("region: objects never alias the line-0 metadata");

    XrRegionHeap heap;
    xr_region_init(&heap);

    void *p = xr_region_alloc(&heap, 64);
    ASSERT(p != NULL, "alloc failed");

    XrRegionBlock *block = XR_REGION_BLOCK_FROM_PTR(p);
    uintptr_t off = (uintptr_t) p - (uintptr_t) block;
    ASSERT(off >= (uintptr_t) (XR_REGION_FIRST_LINE * XR_REGION_LINE_SIZE),
           "object overlaps the reserved metadata line 0");

    xr_region_destroy(&heap);
    PASS();
}

static void test_region_block_accounting(void) {
    TEST("region: per-block alloc_count tracks bumped slots");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro);
    ASSERT(gc != NULL, "create failed");

    XrObjHeader *a = xr_coro_gc_newobj(gc, XR_TBLOB, 64);
    XrObjHeader *b = xr_coro_gc_newobj(gc, XR_TBLOB, 64);
    ASSERT(a != NULL && b != NULL, "alloc failed");

    XrRegionBlock *block = XR_REGION_BLOCK_FROM_PTR(a);
    ASSERT(block->alloc_count >= 2, "alloc_count should count bumped slots");
    ASSERT(block->alloc_bytes >= 128, "alloc_bytes should track allocated size");

    xr_coro_gc_destroy(gc);
    PASS();
}

/* ========== 2. CoroGC Integration Tests ========== */

static void test_gc_create_destroy(void) {
    TEST("CoroGC create/destroy");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro);
    ASSERT(gc != NULL, "create failed");
    ASSERT(gc->totalbytes == 0, "initial bytes should be 0");

    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_large_object(void) {
    TEST("large object: goes to malloc, not Region");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro);
    ASSERT(gc != NULL, "create failed");

    XrObjHeader *large = xr_coro_gc_newobj(gc, XR_TSTRING, 8 * 1024);
    ASSERT(large != NULL, "large alloc failed");
    ASSERT(gc->large_set.count == 1, "should have large object registered");

    xr_coro_gc_destroy(gc);
    PASS();
}

static void test_teardown_finalizer_registry(void) {
    TEST("finalizer registry: teardown finalizes live object");

    setup_test_ext_finalizer();
    g_destroy_count = 0;

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro);
    ASSERT(gc != NULL, "create failed");

    XrObjHeader *obj = xr_coro_gc_newobj(gc, XR_TEST_EXT_TYPE, 64);
    ASSERT(obj != NULL, "alloc failed");
    ASSERT(XR_OBJ_HAS_DESTRUCTOR(obj), "object should carry destructor flag");

    xr_coro_gc_destroy(gc);
    ASSERT(g_destroy_count == 1, "teardown should run finalizer once");
    PASS();
}

static void test_rc_destroy_unregisters_finalizer(void) {
    TEST("finalizer registry: RC destroy prevents teardown duplicate");

    setup_test_ext_finalizer();
    g_destroy_count = 0;

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro);
    ASSERT(gc != NULL, "create failed");

    XrObjHeader *obj = xr_coro_gc_newobj(gc, XR_TEST_EXT_TYPE, 64);
    ASSERT(obj != NULL, "alloc failed");

    xr_coro_gc_rc_destroy(gc, obj);
    ASSERT(g_destroy_count == 1, "RC destroy should run finalizer once");
    ASSERT(gc->finalize_set.count == 0, "RC destroy should unregister finalizer");

    xr_coro_gc_destroy(gc);
    ASSERT(g_destroy_count == 1, "teardown should not rerun finalizer");
    PASS();
}

static void test_large_object_rc_free_unregisters_node(void) {
    TEST("large object: RC free unregisters large object node");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro);
    ASSERT(gc != NULL, "create failed");

    XrObjHeader *large = xr_coro_gc_newobj(gc, XR_TSTRING, 8 * 1024);
    ASSERT(large != NULL, "large alloc failed");
    ASSERT(gc->large_set.count == 1, "large registration missing");

    xr_coro_gc_rc_destroy(gc, large);
    ASSERT(gc->large_set.count == 0, "large registration should be removed");
    ASSERT(gc->large_bytes == 0, "large bytes should be decremented");

    xr_coro_gc_destroy(gc);
    PASS();
}

/* ========== 4. Performance Tests ========== */

static void perf_allocation_throughput(void) {
    TEST("perf: allocation throughput");

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro);
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

    XrCoroGC *gc = xr_coro_gc_create(&dummy_coro);
    gc->gc_disabled++;

    const int COUNT = 100000;
    for (int i = 0; i < COUNT; i++) {
        xr_coro_gc_newobj(gc, XR_TSTRING, 64);
    }
    gc->gc_disabled--;

    XrRegionStats stats;
    xr_region_get_stats(&gc->region, &stats);

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
    printf("  Region Single-Bitmap GC Unit Tests\n");
    printf("========================================\n\n");

    printf("--- Region Bump Allocator ---\n");
    test_region_bump_alloc();
    test_region_objects_skip_metadata_line();
    test_region_block_accounting();

    printf("\n--- CoroGC Integration ---\n");
    test_gc_create_destroy();
    test_large_object();
    test_teardown_finalizer_registry();
    test_rc_destroy_unregisters_finalizer();
    test_large_object_rc_free_unregisters_node();
    /* Tracing collector (fullgc / incremental step / sweep / write barriers)
     * has been removed: reference counting owns reclamation. The tests that
     * asserted tracing-cycle behavior (gc_count increment, PROPAGATE state
     * walk, sweep-driven totalbytes decrease, tri-color barriers) are gone
     * with it. What remains here covers the Region bump allocator + alloc-mark
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
