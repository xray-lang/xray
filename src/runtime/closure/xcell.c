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
#include "../gc/xcoro_gc.h"
#include "../gc/xgc.h"
#include "../xisolate_internal.h"

/*
 * Allocate a lightweight Cell (32 bytes) for single mutable capture.
 * Value initialized to null.
 */
XrCell *xr_cell_new(XrayIsolate *isolate, struct XrCoroutine *coro) {
    XR_DCHECK(isolate != NULL, "cell_new: NULL isolate");

    XrCell *cell;
    if (coro && coro->coro_gc) {
        cell = (XrCell *)xr_coro_gc_newobj(coro->coro_gc, XR_TCELL, XR_CELL_SIZE);
    } else {
        cell = (XrCell *)xr_gc_alloc(&isolate->gc, XR_CELL_SIZE, XR_TCELL);
    }
    if (cell == NULL) {
        return NULL;
    }

    xr_gc_header_init_type(&cell->gc, XR_TCELL);
    cell->value = xr_null();
    return cell;
}
