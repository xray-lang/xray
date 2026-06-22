/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xheap.c - Unified heap allocation entrypoints.
 */

#include "xheap.h"
#include "xalloc_unified.h"
#include "xcoro_heap.h"
#include "../core/xr_runtime_core.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../../coro/xcoroutine.h"
#include <stdio.h>

/* ========== Debug ========== */

void xr_obj_header_print(XrObjHeader *obj) {
    if (!obj) {
        printf("Object Header: NULL\n");
        return;
    }
    printf("Object Header:\n");
    printf("  type: %d\n", obj->type);
    printf("  refcount: %d\n", obj->refcount);
    printf("  objsize: %u\n", obj->objsize);
}

/* ========== Unified Allocation Interface ========== */

void *xr_alloc(struct XrCoroutine *coro, size_t size, uint8_t type) {
    XR_DCHECK(coro != NULL, "xr_alloc: coro must not be NULL");
    XR_DCHECK(((XrObjHeader *) coro)->type == XR_TCOROUTINE,
              "xr_alloc: coro is not XrCoroutine (caller passed wrong type)");
    if (!coro)
        return NULL;

    XrCoroHeap *heap = xr_coro_ensure_heap(coro);
    if (heap) {
        XrObjHeader *obj = xr_coro_heap_new_obj(heap, type, size);
        if (obj)
            return obj;
        xr_log_warning("heap", "xr_alloc: coroutine heap allocation failed for type=%d size=%zu",
                       type, size);
        return NULL;
    }

    if (coro->core) {
        return xr_fixed_heap_alloc(&coro->core->fixed_heap, size, type);
    }
    return NULL;
}
