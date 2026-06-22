/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcell.c - XrCell allocation (closure capture slot)
 */

#include "xcell.h"
#include "../../base/xchecks.h"
#include "../../coro/xcoroutine.h"
#include "../gc/xcoro_heap.h"
#include "../gc/xgc.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"

/*
 * Allocate a lightweight Cell (32 bytes) for single mutable capture.
 * Value initialized to null.
 */
XrCell *xr_cell_new(XrayIsolate *isolate, struct XrCoroutine *coro) {
    XR_DCHECK(isolate != NULL, "cell_new: NULL isolate");

    XrCell *cell;
    if (coro && coro->heap) {
        cell = (XrCell *) xr_coro_heap_new_obj(coro->heap, XR_TCELL, XR_CELL_SIZE);
    } else {
        cell = (XrCell *) xr_fixed_heap_alloc(xr_isolate_get_fixed_heap(isolate), XR_CELL_SIZE,
                                              XR_TCELL);
    }
    if (cell == NULL) {
        return NULL;
    }

    xr_obj_header_init_type(&cell->hdr, XR_TCELL);
    if (coro && coro->heap) {
        XR_OBJ_SET_FLAG(&cell->hdr, XR_OBJ_CYCLE_CANDIDATE);
    }
    cell->value = xr_null();
    return cell;
}

XR_FUNC void xr_gc_destroy_cell(XrObjHeader *obj, XrCoroHeap *owner_heap) {
    if (!obj)
        return;
    XrCell *cell = (XrCell *) obj;
    xr_rc_release_value(owner_heap, cell->value);
    cell->value = xr_null();
}
