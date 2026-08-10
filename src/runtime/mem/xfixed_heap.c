/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfixed_heap.c - Fixed-lifetime heap implementation.
 */

#include "xfixed_heap.h"
#include "../core/xr_runtime_core.h"
#include "../xisolate_api.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include <string.h>

void xr_fixed_heap_init(XrFixedHeap *heap, struct XrVMRuntime *isolate) {
    XR_DCHECK(heap != NULL, "fixed_heap_init: NULL heap");
    memset(heap, 0, sizeof(XrFixedHeap));
    heap->isolate = isolate;
    heap->state = XFIXED_HEAP_LIVE;
}

void xr_fixed_heap_finalize(XrFixedHeap *heap) {
    XR_DCHECK(heap != NULL, "fixed_heap_finalize: NULL heap");
    if (!heap || heap->state == XFIXED_HEAP_FINALIZED || heap->state == XFIXED_HEAP_RECLAIMED)
        return;
    XR_DCHECK(heap->state == XFIXED_HEAP_LIVE, "fixed_heap_finalize: invalid or reentrant state");
    if (heap->state != XFIXED_HEAP_LIVE)
        return;

    heap->state = XFIXED_HEAP_FINALIZING;
    XrRuntimeCore *core = heap->isolate ? xr_isolate_get_runtime_core(heap->isolate) : NULL;

    /* Fixed-lifetime objects can reference one another.  Run every destructor
     * while the complete object graph is still addressable; their sticky RC
     * makes nested drops no-ops, while side buffers and native resources are
     * released exactly once.  Freeing each object immediately after its own
     * destructor made later container destructors dereference already-freed
     * children and rendered teardown order observable. */
    for (XrFixedHeapObjectNode *node = heap->objects; node != NULL; node = node->next) {
        XrObjHeader *obj = node->obj;
        uint8_t type = XR_OBJ_GET_TYPE(obj);
        XrObjDestroyFn destroy = xr_runtime_core_destroy_op(core, type);
        if (destroy != NULL)
            destroy(obj, NULL);
    }

    heap->state = XFIXED_HEAP_FINALIZED;
}

void xr_fixed_heap_reclaim(XrFixedHeap *heap) {
    XR_DCHECK(heap != NULL, "fixed_heap_reclaim: NULL heap");
    if (!heap || heap->state == XFIXED_HEAP_RECLAIMED)
        return;
    XR_DCHECK(heap->state == XFIXED_HEAP_FINALIZED,
              "fixed_heap_reclaim: objects must be finalized first");
    if (heap->state != XFIXED_HEAP_FINALIZED)
        return;

    XrFixedHeapObjectNode *node = heap->objects;
    while (node != NULL) {
        XrFixedHeapObjectNode *next = node->next;
        xr_free(node->obj);
        xr_free(node);
        node = next;
    }
    heap->objects = NULL;

    heap->object_count = 0;
    heap->totalbytes = 0;
    heap->state = XFIXED_HEAP_RECLAIMED;
}

void *xr_fixed_heap_alloc(XrFixedHeap *heap, size_t size, uint8_t type) {
    XR_DCHECK(heap != NULL, "fixed_heap_alloc: NULL heap");
    XR_DCHECK(size >= sizeof(XrObjHeader), "fixed_heap_alloc: size too small");
    XR_DCHECK(type < XR_OBJ_TYPE_MAX, "fixed_heap_alloc: invalid object type");
    XR_DCHECK(heap->state == XFIXED_HEAP_LIVE,
              "fixed_heap_alloc: allocation after finalization began");
    if (heap->state != XFIXED_HEAP_LIVE)
        return NULL;
    XrFixedHeapObjectNode *node =
        (XrFixedHeapObjectNode *) xr_malloc(sizeof(XrFixedHeapObjectNode));
    if (!node)
        return NULL;
    XrObjHeader *obj = (XrObjHeader *) xr_malloc(size);
    if (obj) {
        obj->type = type;
        obj->extra = XR_OBJ_MANAGED;
        /* Fixed objects are immortal: the sign-tagged RC must be sticky so
         * hot-path drop routes to the immortal no-op cold path. */
        obj->refcount = XR_RC_STICKY;
        obj->objsize = (uint32_t) size;
        node->obj = obj;
        node->next = heap->objects;
        heap->objects = node;
        heap->totalbytes += (int64_t) size;
        heap->object_count++;
    } else {
        xr_free(node);
    }
    return obj;
}

XrObjHeader *xr_fixed_heap_new_obj(XrFixedHeap *heap, uint8_t type, size_t size) {
    XR_DCHECK(heap != NULL, "fixed_heap_new_obj: NULL heap");
    return (XrObjHeader *) xr_fixed_heap_alloc(heap, size, type);
}

void xr_fixed_heap_print_stats(XrFixedHeap *heap) {
    XR_DCHECK(heap != NULL, "fixed_heap_print_stats: NULL heap");
    printf("=== XrFixedHeap Stats ===\n");
    printf("Objects: %zu\n", heap->object_count);
    printf("Total bytes: %lld\n", (long long) heap->totalbytes);
    printf("=================================\n");
}
