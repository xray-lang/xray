/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsemaphore_destroy.c - Semaphore object destroy entrypoint
 */

#include "xsemaphore.h"

void xr_obj_destroy_semaphore(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    xr_semaphore_close((XrSemaphore *) obj);
}
