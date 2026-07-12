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
#include "../mem/xcoro_heap.h"
#include "../mem/xheap.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"

/*
 * Allocate a lightweight Cell (32 bytes) for single mutable capture.
 * Value initialized to null.
 */
XrCell *xr_cell_new(XrVMRuntime *isolate, struct XrCoroutine *coro) {
    XR_DCHECK(isolate != NULL, "cell_new: NULL isolate");

    XrCell *cell = (XrCell *) xr_alloc(coro, XR_CELL_SIZE, XR_TCELL);
    if (cell == NULL) {
        return NULL;
    }

    xr_obj_header_init_type(&cell->hdr, XR_TCELL);
    if (coro) {
        XR_OBJ_SET_FLAG(&cell->hdr, XR_OBJ_CYCLE_CANDIDATE);
    }
    cell->value = xr_null();
    return cell;
}

XR_FUNC void xr_obj_destroy_cell(XrObjHeader *obj, XrCoroHeap *owner_heap) {
    if (!obj)
        return;
    XrCell *cell = (XrCell *) obj;
    xr_rc_release_value(owner_heap, cell->value);
    cell->value = xr_null();
}
