/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xwork_queue_gc.c - WorkQueue GC cleanup entrypoint
 */

#include "xwork_queue.h"

#include "../base/xmalloc.h"

static void work_queue_shard_destroy(XrWorkQueueShard *shard) {
    if (!shard)
        return;
    xr_free(shard->items);
    shard->items = NULL;
    shard->capacity = 0;
    shard->head = 0;
    shard->count = 0;
}

void xr_gc_destroy_work_queue(XrObjHeader *obj, struct XrCoroGC *owning_gc) {
    (void) owning_gc;
    if (!obj)
        return;
    XrWorkQueue *q = (XrWorkQueue *) obj;
    for (uint32_t i = 0; i < q->shard_count; i++)
        work_queue_shard_destroy(&q->shards[i]);
}
