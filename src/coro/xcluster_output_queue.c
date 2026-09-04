/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_output_queue.c - Opaque cluster frame queue provider
 *
 * KEY CONCEPT:
 *   Native transport loops exchange owned frame batches through this queue.
 *   Callers choose the byte limit; the provider implements only allocation,
 *   synchronization and wakeup projection.
 */

#include "xcluster_output_queue.h"

#include "../base/xmalloc.h"
#include "../base/xmutex.h"
#include "../os/os_net.h"
#include "../os/os_poll.h"

#include <stdatomic.h>
#include <string.h>

struct XrClusterOutputBatch {
    uint8_t *data;
    uint32_t length;
    struct XrClusterOutputBatch *next;
};

struct XrClusterOutputQueue {
    XrClusterOutputBatch *head;
    XrClusterOutputBatch *tail;
    size_t bytes;
    int64_t frames;
    size_t high_watermark;
    _Atomic(bool) full;
    _Atomic(bool) stopped;
    _Atomic(uint64_t) pending;
    int notify_pipe[2];
    XrAdaptiveMutex lock;
};

static void output_batch_free(XrClusterOutputBatch *batch) {
    if (!batch)
        return;
    xr_free(batch->data);
    xr_free(batch);
}

static XrClusterOutputBatch *output_batch_new_owned(uint8_t *data, uint32_t length) {
    if (!data || length == 0)
        return NULL;
    XrClusterOutputBatch *batch = (XrClusterOutputBatch *) xr_calloc(1, sizeof(*batch));
    if (!batch)
        return NULL;
    batch->data = data;
    batch->length = length;
    return batch;
}

static int output_queue_push_batch(XrClusterOutputQueue *queue, XrClusterOutputBatch *batch) {
    if (!queue || !batch)
        return -1;
    xr_amutex_lock(&queue->lock);
    if (atomic_load_explicit(&queue->stopped, memory_order_relaxed) ||
        atomic_load_explicit(&queue->full, memory_order_relaxed) ||
        batch->length > SIZE_MAX - queue->bytes) {
        xr_amutex_unlock(&queue->lock);
        return -1;
    }
    if (queue->tail)
        queue->tail->next = batch;
    else
        queue->head = batch;
    queue->tail = batch;
    queue->bytes += batch->length;
    queue->frames++;
    atomic_fetch_add_explicit(&queue->pending, 1, memory_order_relaxed);
    if (queue->bytes >= queue->high_watermark)
        atomic_store_explicit(&queue->full, true, memory_order_release);
    if (queue->notify_pipe[1] >= 0)
        xr_poll_signal_wakeup(queue->notify_pipe[1]);
    xr_amutex_unlock(&queue->lock);
    return 0;
}

XrClusterOutputQueue *xr_cluster_output_queue_new(size_t high_watermark) {
    if (high_watermark == 0)
        return NULL;
    XrClusterOutputQueue *queue = (XrClusterOutputQueue *) xr_calloc(1, sizeof(*queue));
    if (!queue)
        return NULL;
    queue->notify_pipe[0] = queue->notify_pipe[1] = -1;
    queue->high_watermark = high_watermark;
    if (xr_poll_create_wakeup_pipe(queue->notify_pipe) != 0) {
        xr_free(queue);
        return NULL;
    }
    xr_amutex_init(&queue->lock);
    return queue;
}

void xr_cluster_output_queue_stop(XrClusterOutputQueue *queue) {
    if (!queue || atomic_exchange_explicit(&queue->stopped, true, memory_order_acq_rel))
        return;
    xr_amutex_lock(&queue->lock);
    xr_closesocket(queue->notify_pipe[1]);
    queue->notify_pipe[1] = -1;
    xr_amutex_unlock(&queue->lock);
}

void xr_cluster_output_queue_destroy(XrClusterOutputQueue *queue) {
    if (!queue)
        return;
    xr_cluster_output_queue_stop(queue);
    XrClusterOutputBatch *batch = queue->head;
    while (batch) {
        XrClusterOutputBatch *next = batch->next;
        output_batch_free(batch);
        batch = next;
    }
    xr_poll_close_wakeup_pipe(queue->notify_pipe);
    xr_free(queue);
}

int xr_cluster_output_queue_push_copy(XrClusterOutputQueue *queue, const uint8_t *data,
                                      uint32_t length) {
    if (!queue || !data || length == 0)
        return -1;
    uint8_t *owned = (uint8_t *) xr_malloc(length);
    if (!owned)
        return -1;
    memcpy(owned, data, length);
    XrClusterOutputBatch *batch = output_batch_new_owned(owned, length);
    if (!batch) {
        xr_free(owned);
        return -1;
    }
    if (output_queue_push_batch(queue, batch) != 0) {
        output_batch_free(batch);
        return -1;
    }
    return 0;
}

int xr_cluster_output_queue_push_owned(XrClusterOutputQueue *queue, uint8_t *data,
                                       uint32_t length) {
    XrClusterOutputBatch *batch = output_batch_new_owned(data, length);
    if (!batch)
        return -1;
    if (output_queue_push_batch(queue, batch) != 0) {
        batch->data = NULL;
        output_batch_free(batch);
        return -1;
    }
    return 0;
}

XrClusterOutputBatch *xr_cluster_output_queue_take_all(XrClusterOutputQueue *queue) {
    if (!queue)
        return NULL;
    xr_amutex_lock(&queue->lock);
    XrClusterOutputBatch *batch = queue->head;
    queue->head = NULL;
    queue->tail = NULL;
    queue->bytes = 0;
    queue->frames = 0;
    atomic_store_explicit(&queue->full, false, memory_order_release);
    xr_amutex_unlock(&queue->lock);
    return batch;
}

const uint8_t *xr_cluster_output_batch_data(const XrClusterOutputBatch *batch) {
    return batch ? batch->data : NULL;
}

uint32_t xr_cluster_output_batch_length(const XrClusterOutputBatch *batch) {
    return batch ? batch->length : 0;
}

void xr_cluster_output_batch_consume(XrClusterOutputQueue *queue, XrClusterOutputBatch **batch) {
    if (!queue || !batch || !*batch)
        return;
    XrClusterOutputBatch *current = *batch;
    *batch = current->next;
    output_batch_free(current);
    atomic_fetch_sub_explicit(&queue->pending, 1, memory_order_relaxed);
}

void xr_cluster_output_batch_drop(XrClusterOutputQueue *queue, XrClusterOutputBatch *batch) {
    while (batch) {
        XrClusterOutputBatch *next = batch->next;
        output_batch_free(batch);
        atomic_fetch_sub_explicit(&queue->pending, 1, memory_order_relaxed);
        batch = next;
    }
}

int xr_cluster_output_queue_notify_fd(const XrClusterOutputQueue *queue) {
    return queue ? queue->notify_pipe[0] : -1;
}

bool xr_cluster_output_queue_is_full(const XrClusterOutputQueue *queue) {
    return queue && atomic_load_explicit(&queue->full, memory_order_acquire);
}

int64_t xr_cluster_output_queue_bytes(XrClusterOutputQueue *queue) {
    if (!queue)
        return 0;
    xr_amutex_lock(&queue->lock);
    int64_t bytes = (int64_t) queue->bytes;
    xr_amutex_unlock(&queue->lock);
    return bytes;
}

int64_t xr_cluster_output_queue_frames(XrClusterOutputQueue *queue) {
    if (!queue)
        return 0;
    xr_amutex_lock(&queue->lock);
    int64_t frames = queue->frames;
    xr_amutex_unlock(&queue->lock);
    return frames;
}

uint64_t xr_cluster_output_queue_pending(const XrClusterOutputQueue *queue) {
    return queue ? atomic_load_explicit(&queue->pending, memory_order_relaxed) : 0;
}
