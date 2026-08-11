/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xruntime_object_heap.h - Physical ownership for canonical runtime objects
 */

#ifndef XR_RUNTIME_OBJECT_HEAP_H
#define XR_RUNTIME_OBJECT_HEAP_H

#include "../abi/xr_runtime_object_header.h"
#include <stddef.h>
#include <stdint.h>

struct XrCoroHeap;
struct XrSystemHeap;

typedef enum XrRuntimeObjectAllocationOwner {
    XR_RUNTIME_OBJECT_OWNER_INVALID = 0,
    XR_RUNTIME_OBJECT_OWNER_EXECUTION = 1,
    XR_RUNTIME_OBJECT_OWNER_TRANSFERABLE = 2,
    XR_RUNTIME_OBJECT_OWNER_SHARED = 3,
    XR_RUNTIME_OBJECT_OWNER_STATIC = 4,
} XrRuntimeObjectAllocationOwner;

typedef void (*XrRuntimeObjectFinalizer)(XrRuntimeObjectHeader *header,
                                         void *context);

XR_FUNC XrRuntimeObjectHeader *xr_runtime_object_allocate(
    size_t object_size, uint16_t object_kind, uint32_t layout_id,
    uint32_t domain_id, XrRuntimeObjectAllocationOwner owner_kind,
    void *owner, XrRuntimeObjectFinalizer finalizer, void *finalizer_context);

XR_FUNC size_t xr_runtime_object_allocation_size(
    const XrRuntimeObjectHeader *header);
XR_FUNC XrRuntimeAbiStatus xr_runtime_object_reclaim(
    XrRuntimeObjectHeader *header);
XR_FUNC XrRuntimeAbiStatus xr_runtime_object_register_weak(
    XrRuntimeObjectHeader *header, struct XrCoroHeap *heap);
XR_FUNC void xr_runtime_object_heap_teardown(struct XrCoroHeap *heap);

#endif /* XR_RUNTIME_OBJECT_HEAP_H */
