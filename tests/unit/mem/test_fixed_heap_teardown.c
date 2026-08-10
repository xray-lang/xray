/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_fixed_heap_teardown.c - Cross-domain teardown ordering contract.
 */

#include <stdio.h>
#include <string.h>

#include "../../../src/runtime/core/xr_runtime_core.h"
#include "../../../src/runtime/xisolate_internal.h"
#include "../../../src/runtime/mem/xcoro_heap.h"
#include "../../../src/runtime/mem/xfixed_heap.h"

#define TEST_FIXED_TYPE 62
#define TEST_ROOT_TYPE 63

typedef struct TestCrossObject {
    XrObjHeader hdr;
    XrValue peer;
} TestCrossObject;

static XrRuntimeCore *g_core;
static TestCrossObject *g_fixed;
static TestCrossObject *g_root;
static int g_fixed_destroy_count;
static int g_root_destroy_count;
static int g_order_errors;

static void destroy_cross_object(XrObjHeader *obj, XrCoroHeap *owner_heap) {
    TestCrossObject *cross = (TestCrossObject *) obj;
    if (cross == g_fixed) {
        g_fixed_destroy_count++;
        if (!g_core || g_core->fixed_heap.state != XFIXED_HEAP_FINALIZING ||
            g_core->root_heap_torn_down)
            g_order_errors++;
        xr_rc_release_value(&g_core->root_heap, cross->peer);
        return;
    }
    if (cross == g_root) {
        g_root_destroy_count++;
        if (!g_core || g_core->fixed_heap.state != XFIXED_HEAP_FINALIZED)
            g_order_errors++;
        /* The fixed peer must still have an addressable sticky header. */
        xr_rc_release_value(owner_heap, cross->peer);
        return;
    }
    g_order_errors++;
}

static int check(bool condition, const char *message) {
    if (condition)
        return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void) {
    XrVMRuntime isolate;
    memset(&isolate, 0, sizeof(isolate));

    XrRuntimeCoreConfig config;
    memset(&config, 0, sizeof(config));
    config.owner_isolate = &isolate;

    g_core = xr_runtime_core_new(&config);
    if (check(g_core != NULL, "runtime core allocation failed"))
        return 1;
    isolate.core_rt = g_core;

    xr_runtime_core_set_destroy_op(g_core, TEST_FIXED_TYPE, destroy_cross_object);
    xr_runtime_core_set_destroy_op(g_core, TEST_ROOT_TYPE, destroy_cross_object);

    g_fixed = (TestCrossObject *) xr_fixed_heap_new_obj(&g_core->fixed_heap, TEST_FIXED_TYPE,
                                                        sizeof(TestCrossObject));
    g_root = (TestCrossObject *) xr_coro_heap_new_obj(&g_core->root_heap, TEST_ROOT_TYPE,
                                                      sizeof(TestCrossObject));
    if (check(g_fixed != NULL && g_root != NULL, "cross-domain allocation failed"))
        return 1;

    g_fixed->peer = xr_make_ptr_val(&g_root->hdr);
    g_root->peer = xr_make_ptr_val(&g_fixed->hdr);
    xr_rc_retain(&g_root->hdr);

    xr_runtime_core_finalize_fixed_heap(g_core);
    xr_runtime_core_finalize_fixed_heap(g_core);
    if (check(g_fixed_destroy_count == 1, "fixed object must be finalized exactly once") ||
        check(g_core->fixed_heap.state == XFIXED_HEAP_FINALIZED,
              "fixed heap must retain finalized bodies"))
        return 1;

    xr_runtime_core_teardown_root_heap(g_core);
    xr_runtime_core_teardown_root_heap(g_core);
    if (check(g_root_destroy_count == 1, "root object must be finalized exactly once") ||
        check(g_order_errors == 0, "cross-domain finalizers ran in an unsafe order"))
        return 1;

    xr_runtime_core_reclaim_fixed_heap(g_core);
    xr_runtime_core_reclaim_fixed_heap(g_core);
    if (check(g_core->fixed_heap.state == XFIXED_HEAP_RECLAIMED,
              "fixed heap must enter the reclaimed state") ||
        check(g_core->fixed_heap.objects == NULL && g_core->fixed_heap.object_count == 0,
              "fixed heap reclaim must release every body"))
        return 1;

    xr_runtime_core_delete(g_core);
    isolate.core_rt = NULL;
    g_core = NULL;
    g_fixed = NULL;
    g_root = NULL;

    printf("fixed/root phased teardown: PASS\n");
    return 0;
}
