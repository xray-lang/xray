/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_output_queue.h - Opaque cluster frame queue provider
 *
 * KEY CONCEPT:
 *   The queue owns frame bytes, synchronization and the writer wakeup handle.
 *   Its byte limit is supplied by cluster.xr through the runtime boundary.
 */

#ifndef XR_CORO_CLUSTER_OUTPUT_QUEUE_H
#define XR_CORO_CLUSTER_OUTPUT_QUEUE_H

#include "../base/xdefs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct XrClusterOutputBatch XrClusterOutputBatch;
typedef struct XrClusterOutputQueue XrClusterOutputQueue;

typedef enum XrClusterOutputPushResult {
    XR_CLUSTER_OUTPUT_ACCEPTED = 0,
    XR_CLUSTER_OUTPUT_FULL = 1,
    XR_CLUSTER_OUTPUT_STOPPED = 2,
    XR_CLUSTER_OUTPUT_RESOURCE_UNAVAILABLE = 3,
    XR_CLUSTER_OUTPUT_INVALID = 4,
} XrClusterOutputPushResult;

XR_FUNC XrClusterOutputQueue *xr_cluster_output_queue_new(size_t high_watermark);
XR_FUNC void xr_cluster_output_queue_stop(XrClusterOutputQueue *queue);
XR_FUNC void xr_cluster_output_queue_destroy(XrClusterOutputQueue *queue);
XR_FUNC XrClusterOutputPushResult xr_cluster_output_queue_push_copy(XrClusterOutputQueue *queue,
                                                                    const uint8_t *data,
                                                                    uint32_t length);
/* The queue consumes data only when this operation succeeds. */
XR_FUNC XrClusterOutputPushResult xr_cluster_output_queue_push_owned(XrClusterOutputQueue *queue,
                                                                     uint8_t *data,
                                                                     uint32_t length);
XR_FUNC XrClusterOutputBatch *xr_cluster_output_queue_take_all(XrClusterOutputQueue *queue);
XR_FUNC const uint8_t *xr_cluster_output_batch_data(const XrClusterOutputBatch *batch);
XR_FUNC uint32_t xr_cluster_output_batch_length(const XrClusterOutputBatch *batch);
XR_FUNC void xr_cluster_output_batch_consume(XrClusterOutputQueue *queue,
                                             XrClusterOutputBatch **batch);
XR_FUNC void xr_cluster_output_batch_drop(XrClusterOutputQueue *queue, XrClusterOutputBatch *batch);
XR_FUNC int xr_cluster_output_queue_notify_fd(const XrClusterOutputQueue *queue);
XR_FUNC bool xr_cluster_output_queue_is_full(const XrClusterOutputQueue *queue);
XR_FUNC int64_t xr_cluster_output_queue_bytes(XrClusterOutputQueue *queue);
XR_FUNC int64_t xr_cluster_output_queue_frames(XrClusterOutputQueue *queue);
XR_FUNC uint64_t xr_cluster_output_queue_pending(const XrClusterOutputQueue *queue);

#endif  // XR_CORO_CLUSTER_OUTPUT_QUEUE_H
