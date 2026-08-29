/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_deep_copy_array_owner_heap.c - A deep-copied array must carry an
 * initialised accounting owner.
 *
 * xr_deep_copy_array_with_ctx once set every field of the new array except
 * owner_heap.  With no destination coroutine — a root-level copy, which is what
 * a top-level script does — copy_ctx_alloc falls back to xr_fixed_heap_alloc,
 * and that allocator initialises only the object header.  The field therefore
 * kept whatever the malloc'd block happened to hold, and VM teardown crashed:
 * xr_fixed_heap_finalize -> xr_obj_destroy_array -> xr_coro_heap_sub_external
 * dereferenced the garbage pointer, sailing past that function's NULL guard.
 *
 * Both sides of the byte accounting must name the same heap, so the copy fixes
 * the owner to ctx->dst_heap exactly as the map and set copies do.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/coro/xdeep_copy.h"
#include "../../../src/runtime/core/xr_runtime_core.h"
#include "../../../src/runtime/mem/xcoro_heap.h"
#include "../../../src/runtime/mem/xfixed_heap.h"
#include "../../../src/runtime/object/xarray.h"
#include "../../../src/runtime/xisolate_internal.h"

#define ARRAY_ELEMENTS 4

static int check(bool condition, const char *message) {
    if (condition)
        return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

/* Try to leave a dirty same-sized block at the head of the allocator's free
 * list, so an uninitialised owner_heap reads back as a wrong pointer rather
 * than an accidental NULL.  This works where freed blocks keep their contents
 * (glibc's tcache), and does nothing on macOS, whose allocator scrubs small
 * blocks on free and hands out fresh zero pages.  The reliable detector is the
 * sanitizer lane: under ASan's malloc fill this field reads 0xbebebebebebebebe
 * without the fix, and scripts/run_asan_focused.py runs every ^test_ target.
 * In a plain build the assertion still pins the contract against a later
 * change that sets the field to the wrong heap. */
static void dirty_the_free_list(void) {
    void *blocks[16];
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        blocks[i] = malloc(sizeof(XrArray));
        if (blocks[i])
            memset(blocks[i], 0xAB, sizeof(XrArray));
    }
    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++)
        free(blocks[i]);
}

int main(void) {
    XrVMRuntime isolate;
    memset(&isolate, 0, sizeof(isolate));

    XrRuntimeCoreConfig config;
    memset(&config, 0, sizeof(config));
    config.owner_isolate = &isolate;

    XrRuntimeCore *core = xr_runtime_core_new(&config);
    if (check(core != NULL, "runtime core allocation failed"))
        return 1;
    isolate.core_rt = core;

    /* The source array lives on the root execution heap, exactly as a top-level
     * array literal does. */
    XrArray *source = xr_array_with_capacity_in(&core->root_alloc, ARRAY_ELEMENTS, XR_ELEM_I64);
    if (check(source != NULL, "source array allocation failed"))
        return 1;
    for (int i = 0; i < ARRAY_ELEMENTS; i++)
        xr_array_push(source, XR_FROM_INT(i + 1));
    if (check(source->owner_heap == &core->root_heap,
              "a root-heap array must be charged to the root heap"))
        return 1;

    size_t fixed_objects_before = core->fixed_heap.object_count;
    dirty_the_free_list();

    /* No destination coroutine: this is the root-level copy that reaches the
     * fixed-heap fallback in copy_ctx_alloc. */
    XrValue copied = xr_deep_copy_explicit_to_coro_core(core, XR_FROM_PTR(source), NULL);
    if (check(XR_IS_PTR(copied), "deep copy did not return an object"))
        return 1;

    XrArray *copy = (XrArray *) XR_VALUE_GCPTR(copied);
    if (check(copy != source, "deep copy must produce a distinct array") ||
        check(copy->length == ARRAY_ELEMENTS, "deep copy must preserve the element count") ||
        check(core->fixed_heap.object_count == fixed_objects_before + 1,
              "the root-level copy must land on the fixed heap"))
        return 1;

    /* The contract under test: the copy names the context's destination heap
     * (NULL here), never uninitialised memory. */
    if (check(copy->owner_heap == NULL,
              "a root-level deep copy must fix owner_heap to the destination heap"))
        return 1;

    /* And teardown, which is where the uninitialised pointer used to be
     * dereferenced, must run through cleanly. */
    xr_runtime_core_finalize_fixed_heap(core);
    xr_runtime_core_teardown_root_heap(core);
    xr_runtime_core_reclaim_fixed_heap(core);
    if (check(core->fixed_heap.state == XFIXED_HEAP_RECLAIMED,
              "fixed heap must reach the reclaimed state"))
        return 1;

    xr_runtime_core_delete(core);
    isolate.core_rt = NULL;

    printf("deep-copied array owner heap: PASS\n");
    return 0;
}
