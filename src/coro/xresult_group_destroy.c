/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresult_group_destroy.c - ResultGroup object destroy entrypoint
 */

#include "xresult_group.h"

#include "../base/xmalloc.h"

static void result_group_batch_free_all(XrResultGroupBatch *batch) {
    while (batch) {
        XrResultGroupBatch *next = batch->next;
        xr_free(batch);
        batch = next;
    }
}

void xr_obj_destroy_result_group(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    (void) owner_heap;
    if (!obj)
        return;
    XrResultGroup *g = (XrResultGroup *) obj;
    result_group_batch_free_all(g->batch_first);
    g->batch_first = NULL;
    g->batch_last = NULL;
}
