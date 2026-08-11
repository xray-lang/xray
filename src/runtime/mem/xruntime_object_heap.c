/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xruntime_object_heap.c - Physical ownership for canonical runtime objects
 */

#include "xruntime_object_heap.h"
#include "xcoro_heap.h"
#include "xsystem_heap.h"
#include "xweak_handle.h"
#include "../../base/xmalloc.h"
#include <limits.h>
#include <string.h>

#define XR_RUNTIME_OBJECT_ALLOCATION_MAGIC UINT64_C(0x58524f424a414249)

typedef struct XrRuntimeObjectAllocation {
    struct XrRuntimeObjectAllocation *previous;
    struct XrRuntimeObjectAllocation *next;
    void *owner;
    XrRuntimeObjectFinalizer finalizer;
    void *finalizer_context;
    size_t object_size;
    uint64_t magic;
    uint8_t owner_kind;
    uint8_t finalizing;
    uint8_t has_weak;
    uint8_t reserved[5];
} XrRuntimeObjectAllocation;

_Static_assert(sizeof(XrRuntimeObjectAllocation) %
                       XR_RUNTIME_OBJECT_HEADER_ALIGNMENT ==
                   0,
               "allocation prefix must preserve canonical header alignment");

static XrRuntimeObjectAllocation *allocation_from_header(
    const XrRuntimeObjectHeader *header) {
    if (!header)
        return NULL;
    XrRuntimeObjectAllocation *allocation =
        ((XrRuntimeObjectAllocation *) header) - 1;
    return allocation->magic == XR_RUNTIME_OBJECT_ALLOCATION_MAGIC
               ? allocation
               : NULL;
}

static bool owner_valid(XrRuntimeObjectAllocationOwner owner_kind, void *owner) {
    switch (owner_kind) {
        case XR_RUNTIME_OBJECT_OWNER_EXECUTION:
        case XR_RUNTIME_OBJECT_OWNER_TRANSFERABLE:
        case XR_RUNTIME_OBJECT_OWNER_SHARED:
            return owner != NULL;
        case XR_RUNTIME_OBJECT_OWNER_STATIC:
            return owner == NULL;
        default:
            return false;
    }
}

static void link_execution(XrRuntimeObjectAllocation *allocation,
                           XrCoroHeap *heap) {
    allocation->next = heap->runtime_object_allocations;
    if (allocation->next)
        allocation->next->previous = allocation;
    heap->runtime_object_allocations = allocation;
    heap->totalbytes += (int64_t) allocation->object_size;
    heap->object_count++;
}

static void unlink_execution(XrRuntimeObjectAllocation *allocation,
                             XrCoroHeap *heap) {
    if (allocation->previous)
        allocation->previous->next = allocation->next;
    else if (heap && heap->runtime_object_allocations == allocation)
        heap->runtime_object_allocations = allocation->next;
    if (allocation->next)
        allocation->next->previous = allocation->previous;
    if (heap && !heap->is_tearing_down) {
        heap->totalbytes -= (int64_t) allocation->object_size;
        if (heap->object_count > 0)
            heap->object_count--;
    }
    allocation->previous = NULL;
    allocation->next = NULL;
}

XrRuntimeObjectHeader *xr_runtime_object_allocate(
    size_t object_size, uint16_t object_kind, uint32_t layout_id,
    uint32_t domain_id, XrRuntimeObjectAllocationOwner owner_kind, void *owner,
    XrRuntimeObjectFinalizer finalizer, void *finalizer_context) {
    if (object_size < sizeof(XrRuntimeObjectHeader) ||
        object_size > SIZE_MAX - sizeof(XrRuntimeObjectAllocation) ||
        object_size > INT64_MAX || !owner_valid(owner_kind, owner))
        return NULL;
    XrRuntimeObjectAllocation *allocation =
        (XrRuntimeObjectAllocation *) xr_calloc(
            1, sizeof(XrRuntimeObjectAllocation) + object_size);
    if (!allocation)
        return NULL;
    allocation->owner = owner;
    allocation->finalizer = finalizer;
    allocation->finalizer_context = finalizer_context;
    allocation->object_size = object_size;
    allocation->magic = XR_RUNTIME_OBJECT_ALLOCATION_MAGIC;
    allocation->owner_kind = (uint8_t) owner_kind;
    XrRuntimeObjectHeader *header =
        (XrRuntimeObjectHeader *) (allocation + 1);
    XrRuntimeAbiStatus status = xr_runtime_object_header_init(
        header, object_kind, XR_RUNTIME_OBJECT_FLAG_NONE, layout_id, domain_id);
    if (status != XR_RUNTIME_ABI_OK) {
        allocation->magic = 0;
        xr_free(allocation);
        return NULL;
    }
    if (owner_kind == XR_RUNTIME_OBJECT_OWNER_EXECUTION) {
        link_execution(allocation, (XrCoroHeap *) owner);
    } else if (owner_kind == XR_RUNTIME_OBJECT_OWNER_SHARED ||
               owner_kind == XR_RUNTIME_OBJECT_OWNER_TRANSFERABLE) {
        xr_sysheap_note_runtime_object_alloc(
            (XrSystemHeap *) owner, object_size,
            owner_kind == XR_RUNTIME_OBJECT_OWNER_SHARED);
    }
    return header;
}

size_t xr_runtime_object_allocation_size(
    const XrRuntimeObjectHeader *header) {
    XrRuntimeObjectAllocation *allocation = allocation_from_header(header);
    return allocation ? allocation->object_size : 0;
}

XrRuntimeAbiStatus xr_runtime_object_reclaim(XrRuntimeObjectHeader *header) {
    XrRuntimeObjectAllocation *allocation = allocation_from_header(header);
    if (!allocation)
        return XR_RUNTIME_ABI_INVALID_ARGUMENT;
    if (allocation->finalizing)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    allocation->finalizing = 1;
    XrRuntimeObjectAllocationOwner owner_kind =
        (XrRuntimeObjectAllocationOwner) allocation->owner_kind;
    if (allocation->has_weak &&
        owner_kind == XR_RUNTIME_OBJECT_OWNER_EXECUTION) {
        xr_weak_table_runtime_target_dying(
            (XrCoroHeap *) allocation->owner, header);
        allocation->has_weak = 0;
    }
    if (owner_kind == XR_RUNTIME_OBJECT_OWNER_EXECUTION) {
        unlink_execution(allocation, (XrCoroHeap *) allocation->owner);
    } else if (owner_kind == XR_RUNTIME_OBJECT_OWNER_SHARED ||
               owner_kind == XR_RUNTIME_OBJECT_OWNER_TRANSFERABLE) {
        xr_sysheap_note_runtime_object_free(
            (XrSystemHeap *) allocation->owner, allocation->object_size,
            owner_kind == XR_RUNTIME_OBJECT_OWNER_SHARED);
    }
    allocation->owner = NULL;
    allocation->owner_kind = XR_RUNTIME_OBJECT_OWNER_INVALID;
    if (allocation->finalizer)
        allocation->finalizer(header, allocation->finalizer_context);
    allocation->magic = 0;
    xr_free(allocation);
    return XR_RUNTIME_ABI_OK;
}

XrRuntimeAbiStatus xr_runtime_object_register_weak(
    XrRuntimeObjectHeader *header, XrCoroHeap *heap) {
    XrRuntimeObjectAllocation *allocation = allocation_from_header(header);
    if (!allocation || !heap || allocation->finalizing ||
        allocation->owner_kind != XR_RUNTIME_OBJECT_OWNER_EXECUTION ||
        allocation->owner != heap)
        return XR_RUNTIME_ABI_INVALID_POLICY;
    allocation->has_weak = 1;
    return XR_RUNTIME_ABI_OK;
}

void xr_runtime_object_heap_teardown(XrCoroHeap *heap) {
    if (!heap)
        return;
    XrRuntimeObjectAllocation *allocation = heap->runtime_object_allocations;
    heap->runtime_object_allocations = NULL;
    while (allocation) {
        XrRuntimeObjectAllocation *next = allocation->next;
        if (allocation->has_weak) {
            xr_weak_table_runtime_target_dying(
                heap, (XrRuntimeObjectHeader *) (allocation + 1));
            allocation->has_weak = 0;
        }
        allocation->previous = NULL;
        allocation->next = NULL;
        allocation->owner = NULL;
        allocation->owner_kind = XR_RUNTIME_OBJECT_OWNER_INVALID;
        allocation->finalizing = 1;
        XrRuntimeObjectHeader *header =
            (XrRuntimeObjectHeader *) (allocation + 1);
        atomic_store_explicit(&header->rc, XR_RUNTIME_OBJECT_RC_STICKY,
                              memory_order_release);
        if (allocation->finalizer)
            allocation->finalizer(header, allocation->finalizer_context);
        allocation->magic = 0;
        xr_free(allocation);
        allocation = next;
    }
}
