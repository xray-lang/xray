/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfixed_heap.h - Fixed-lifetime heap interfaces.
 */

#ifndef XFIXED_HEAP_H
#define XFIXED_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "xobj_ops.h"

typedef enum XrFixedHeapState {
    XFIXED_HEAP_LIVE = 0,
    XFIXED_HEAP_FINALIZING,
    XFIXED_HEAP_FINALIZED,
    XFIXED_HEAP_RECLAIMED,
} XrFixedHeapState;

/* The fixed heap manages objects that live until runtime teardown and never
 * participate in mark-sweep. It is a small bootstrap/fallback allocator, not a
 * tracing collector.
 */

struct XrVMRuntime;
typedef struct XrFixedHeap XrFixedHeap;

typedef struct XrFixedHeapObjectNode {
    XrObjHeader *obj;
    struct XrFixedHeapObjectNode *next;
} XrFixedHeapObjectNode;

typedef struct XrFixedHeap {
    uint8_t state;
    uint8_t _pad[7];
    struct XrVMRuntime *isolate;
    int64_t totalbytes;
    XrFixedHeapObjectNode *objects;  // Fixed-lifetime objects
    size_t object_count;
} XrFixedHeap;

XR_FUNC void xr_fixed_heap_init(XrFixedHeap *heap, struct XrVMRuntime *isolate);
XR_FUNC void xr_fixed_heap_finalize(XrFixedHeap *heap);
XR_FUNC void xr_fixed_heap_reclaim(XrFixedHeap *heap);
XR_FUNC void *xr_fixed_heap_alloc(XrFixedHeap *heap, size_t size, uint8_t type);
XR_FUNC XrObjHeader *xr_fixed_heap_new_obj(XrFixedHeap *heap, uint8_t type, size_t size);
XR_FUNC void xr_fixed_heap_print_stats(XrFixedHeap *heap);

#endif  // XFIXED_HEAP_H
