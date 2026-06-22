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
#include <stdio.h>
#include <string.h>

void xr_fixed_heap_init(XrFixedHeap *heap, struct XrayIsolate *isolate) {
    XR_DCHECK(heap != NULL, "fixed_heap_init: NULL heap");
    memset(heap, 0, sizeof(XrFixedHeap));
    heap->isolate = isolate;
    heap->state = XFIXED_HEAP_IDLE;
}

void xr_fixed_heap_cleanup(XrFixedHeap *heap) {
    XR_DCHECK(heap != NULL, "fixed_heap_cleanup: NULL heap");
    XrFixedHeapObjectNode *node = heap->objects;
    XrRuntimeCore *core = heap->isolate ? xr_isolate_get_runtime_core(heap->isolate) : NULL;
    while (node != NULL) {
        XrFixedHeapObjectNode *next = node->next;
        XrObjHeader *obj = node->obj;
        uint8_t type = XR_OBJ_GET_TYPE(obj);
        XrObjDestroyFn destroy = xr_runtime_core_destroy_op(core, type);
        if (destroy != NULL) {
            destroy(obj, NULL);
        }
        xr_free(obj);
        xr_free(node);
        node = next;
    }
    heap->objects = NULL;

    heap->object_count = 0;
    heap->totalbytes = 0;
}

void *xr_fixed_heap_alloc(XrFixedHeap *heap, size_t size, uint8_t type) {
    XR_DCHECK(heap != NULL, "fixed_heap_alloc: NULL heap");
    XR_DCHECK(size >= sizeof(XrObjHeader), "fixed_heap_alloc: size too small");
    XR_DCHECK(type < XR_OBJ_TYPE_MAX, "fixed_heap_alloc: invalid object type");
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
        obj->_rsv = XR_CYCLE_NOT_IN_ROOTS;
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
