/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xevent_count_destroy.c - EventCount object destroy entrypoint
 */

#include "xevent_count.h"

#include "../runtime/mem/xsystem_heap.h"

void xr_obj_destroy_event_count(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    XrEventCount *event = (XrEventCount *) obj;
    xr_event_count_close(event);
    xr_sysheap_free_shared(event, sizeof(XrEventCount));
}
