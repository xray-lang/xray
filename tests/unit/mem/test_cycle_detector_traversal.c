/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cycle_detector_traversal.c - Whole-heap traversal self-check.
 *
 * This self-check runs before any functional case because
 * that the traversal is new code with no precedent in the tree (the existing
 * `for (b = h->full_blocks; ...)` loops walk the block CHAIN, not the objects
 * inside a block) and its failure mode is silent: a desync does not crash, it
 * starts reading neighbouring bytes as if they were object headers, and every
 * answer after that is fiction.
 *
 * So the walk is checked against heap->object_count — a counter maintained
 * independently, incremented at allocation and decremented in rc_destroy_one.
 * Only when those two agree is the cycle logic worth running.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/runtime/mem/xcoro_heap.h"
#include "../../../src/runtime/mem/xcycle_detector.h"
#include "../../../src/runtime/mem/xobj_header.h"
#include "../../../src/runtime/value/xvalue.h"
#include "../../../src/runtime/xisolate_api.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../src/coro/xcoroutine.h"
#include "../test_win_compat.h"

/* Same shape as test_region.c: xr_coro_heap_create only stores heap->owner. */
static XrCoroutine dummy_coro;
static XrVMRuntime g_test_iso;
static XrRuntimeCore g_test_core;

static void setup_dummy_coro(void) {
    memset(&g_test_iso, 0, sizeof(g_test_iso));
    memset(&g_test_core, 0, sizeof(g_test_core));
    memset(&dummy_coro, 0, sizeof(dummy_coro));
    g_test_iso.core_rt = &g_test_core;
    g_test_core.vm_owner = &g_test_iso;
    dummy_coro.core = &g_test_core;
}

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));                                 \
            g_failed++;                                                                            \
            return;                                                                                \
        }                                                                                          \
        g_passed++;                                                                                \
    } while (0)

#ifdef XR_ENABLE_CYCLE_DETECTOR

/* Allocate `n` objects of `size` bytes and confirm the walk still finds
 * exactly what the heap says is live. */
static void check_after_allocating(XrCoroHeap *heap, int n, size_t size, const char *what) {
    for (int i = 0; i < n; i++) {
        XrObjHeader *o = xr_coro_heap_new_obj(heap, XR_TBLOB, size);
        if (!o) {
            printf("FAIL: allocation failed for %s\n", what);
            g_failed++;
            return;
        }
    }
    uint32_t counted = 0;
    bool ok = xr_cycle_detector_count_live(heap, &counted);
    if (!ok) {
        printf("FAIL: traversal self-check tripped for %s\n", what);
        g_failed++;
        return;
    }
    if (counted != heap->object_count) {
        printf("FAIL: %s — walked %u live objects, heap says %u\n", what, counted,
               heap->object_count);
        g_failed++;
        return;
    }
    g_passed++;
}

static void test_traversal_matches_object_count(void) {
    XrCoroHeap *heap = xr_coro_heap_create(dummy_coro.core);
    CHECK(heap != NULL, "heap create");

    uint32_t counted = 0;
    CHECK(xr_cycle_detector_count_live(heap, &counted), "empty heap walk");
    CHECK(counted == heap->object_count, "empty heap count");

    /* Small objects: one block, then enough to retire several. A 16 KB block
     * holds roughly 250 objects of 64 bytes, so 900 spans the
     * current_block / full_blocks boundary that the walk treats differently. */
    check_after_allocating(heap, 10, 64, "10 small objects");
    check_after_allocating(heap, 900, 64, "900 small objects (multiple blocks)");

    /* Mixed sizes exercise the objsize step rather than a fixed stride. */
    check_after_allocating(heap, 50, 24, "50 objects of 24 bytes");
    check_after_allocating(heap, 50, 200, "50 objects of 200 bytes");
    check_after_allocating(heap, 50, 1000, "50 objects of 1000 bytes");

    /* Large objects live in heap->large_set, entirely outside the blocks.
     * Walking blocks alone silently misses every one of them — which is the
     * defect this half of the check exists to catch. */
    check_after_allocating(heap, 5, 8 * 1024, "5 large objects (>4KB)");
    check_after_allocating(heap, 2, 300 * 1024, "2 very large objects (>=256KB)");

    xr_coro_heap_destroy(heap);
}

/* Dead objects stay walkable: the freelist link is written into the payload's
 * first word, past the 16-byte header, so objsize survives the object. If it
 * did not, the walk would desync at the first dead object — the exact reason
 * the whole-heap traversal contract lists that as its load-bearing seventh premise. */
static void test_traversal_steps_over_dead_objects(void) {
    XrCoroHeap *heap = xr_coro_heap_create(dummy_coro.core);
    CHECK(heap != NULL, "heap create");

    XrObjHeader *objs[200];
    for (int i = 0; i < 200; i++) {
        objs[i] = xr_coro_heap_new_obj(heap, XR_TBLOB, 48);
        CHECK(objs[i] != NULL, "allocate");
    }
    /* Kill every other one, leaving live and dead interleaved. */
    for (int i = 0; i < 200; i += 2)
        xr_coro_heap_destroy_obj(heap, objs[i]);

    uint32_t counted = 0;
    CHECK(xr_cycle_detector_count_live(heap, &counted), "walk over interleaved dead objects");
    CHECK(counted == heap->object_count, "live count with dead objects interleaved");

    xr_coro_heap_destroy(heap);
}

#endif /* XR_ENABLE_CYCLE_DETECTOR */

int main(void) {
    xr_test_suppress_dialogs();
    printf("test_cycle_detector_traversal:\n");
#ifdef XR_ENABLE_CYCLE_DETECTOR
    setup_dummy_coro();
    test_traversal_matches_object_count();
    test_traversal_steps_over_dead_objects();
#else
    printf("  (detector disabled at compile time; nothing to check)\n");
#endif
    printf("  %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
