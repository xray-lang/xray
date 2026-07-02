/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcountdown_latch_destroy.c - CountdownLatch object destroy entrypoint
 */

#include "xcountdown_latch.h"

void xr_obj_destroy_countdown_latch(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    xr_countdown_latch_close((XrCountdownLatch *) obj);
}
