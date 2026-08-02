/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cycle_detector_shared.c - Shared-domain cycle scan (task 259 §3).
 *
 * The scan is exercised at the C level because current banding discipline
 * blocks every source-level construction we tried: sending a closure that
 * captures its own channel and sending a class instance that holds a Channel
 * field both fail closed (xchannel_ops.h "implicit copy removed" checks)
 * before a cycle can form. The detector is the net for when those fences move
 * — so the test builds the cycle the allocator-level way: two shared cells
 * pointing at each other, registered by xr_sysheap_alloc_shared itself.
 *
 * Also covers, in the same breath:
 *   - the false-positive guard: an externally referenced pair must NOT be
 *     reported (scan-black must clear it);
 *   - per-domain metering: xr_sysheap_shared_live_bytes_total rises on
 *     alloc_shared and falls on free_shared.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/runtime/mem/xcycle_detector.h"
#include "../../../src/runtime/mem/xobj_header.h"
#include "../../../src/runtime/mem/xsystem_heap.h"
#include "../../../src/runtime/xshared.h"
#include "../../../src/runtime/closure/xcell.h"
#include "../../../src/runtime/value/xvalue.h"
#include "../test_win_compat.h"

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(desc, cond)                                                                          \
    do {                                                                                           \
        if (cond) {                                                                                \
            g_passed++;                                                                            \
        } else {                                                                                   \
            g_failed++;                                                                            \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", desc, __FILE__, __LINE__);                       \
        }                                                                                          \
    } while (0)

#ifdef XR_ENABLE_CYCLE_DETECTOR

static XrCell *alloc_shared_cell(XrSystemHeap *heap) {
    XrCell *cell = (XrCell *) xr_sysheap_alloc_shared(heap, sizeof(XrCell), XR_TCELL);
    if (cell)
        cell->value = XR_NULL_VAL;
    return cell;
}

static XrValue cell_ref(XrCell *cell) {
    return XR_FROM_PTR(cell);
}

static void run_tests(void) {
    XrSystemHeap heap;
    memset(&heap, 0, sizeof(heap));
    CHECK("sysheap init", xr_sysheap_init(&heap, NULL));

    uint64_t bytes0 = xr_sysheap_shared_live_bytes_total();

    /* ---- metering: alloc raises, free lowers ---- */
    XrCell *lone = alloc_shared_cell(&heap);
    CHECK("lone cell allocated", lone != NULL);
    CHECK("sharedBytes rose after alloc",
          xr_sysheap_shared_live_bytes_total() == bytes0 + sizeof(XrCell));
    xr_sysheap_free_shared(lone, sizeof(XrCell));
    CHECK("sharedBytes fell after free", xr_sysheap_shared_live_bytes_total() == bytes0);

    /* ---- a dead two-cell cycle is reported ---- */
    XrCell *a = alloc_shared_cell(&heap);
    XrCell *b = alloc_shared_cell(&heap);
    CHECK("cycle cells allocated", a && b);
    a->value = cell_ref(b);
    b->value = cell_ref(a);
    /* Each cell's only reference is the other's slot: logical RC 1 apiece.
     * alloc_shared already set exactly that. */

    XrCycleReport report;
    bool found = xr_cycle_detector_scan_shared(&report);
    CHECK("dead shared cycle found", found);
    CHECK("one cycle", report.cycle_count == 1);
    CHECK("two objects on it", report.object_count == 2);
    CHECK("bytes attributed", report.byte_count == 2 * sizeof(XrCell));

    /* ---- the same shape with an external reference is NOT a leak ---- */
    xr_shared_set_refc((XrObjHeader *) a, 2); /* simulate a live outside handle */
    found = xr_cycle_detector_scan_shared(&report);
    CHECK("externally referenced pair not reported", !found && report.cycle_count == 0);
    xr_shared_set_refc((XrObjHeader *) a, 1);

    /* ---- scan is an observation: counts must be untouched ---- */
    CHECK("scan restored a's count", xr_shared_get_refc((XrObjHeader *) a) == 1);
    CHECK("scan restored b's count", xr_shared_get_refc((XrObjHeader *) b) == 1);

    /* Break the cycle by hand and release, so teardown sees a clean registry. */
    a->value = XR_NULL_VAL;
    b->value = XR_NULL_VAL;
    xr_sysheap_free_shared(a, sizeof(XrCell));
    xr_sysheap_free_shared(b, sizeof(XrCell));
    CHECK("sharedBytes back to baseline", xr_sysheap_shared_live_bytes_total() == bytes0);

    /* ---- empty registry scans clean ---- */
    found = xr_cycle_detector_scan_shared(&report);
    CHECK("empty shared registry reports nothing", !found && report.cycle_count == 0);

    xr_cycle_detector_reset();
    xr_sysheap_destroy(&heap);
}

#else /* !XR_ENABLE_CYCLE_DETECTOR */

static void run_tests(void) {
    /* Compiled out with the detector; the target exists so the test matrix
     * does not change shape between configurations. */
    printf("cycle detector disabled; shared-domain scan test is a no-op\n");
}

#endif

int main(void) {
    run_tests();
    printf("test_cycle_detector_shared: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
