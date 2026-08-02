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

void xr_obj_destroy_event_count(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    /* Destroy ops release internals only; the shared-destroy caller retires
     * and frees the object itself afterwards. Freeing it here too made every
     * EventCount die by double free (sibling latch/semaphore/queue/group
     * destroy ops all follow the internals-only contract). */
    xr_event_count_close((XrEventCount *) obj);
}
