/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_peer_transport.h - Opaque cluster peer I/O provider
 *
 * KEY CONCEPT:
 *   A lease keeps one native peer resource alive while a framed read or
 *   queued write is parked on netpoll. The provider reports raw transport
 *   outcomes; cluster.xr owns retry, overload and disconnect policy.
 */

#ifndef XR_IO_CLUSTER_PEER_TRANSPORT_H
#define XR_IO_CLUSTER_PEER_TRANSPORT_H

#include "../base/xdefs.h"
#include "../coro/xcluster_output_queue.h"
#include "../coro/xyieldable.h"
#include "xnet_transport.h"

#include <stdatomic.h>
#include <stdint.h>

typedef void (*XrClusterPeerLeaseRelease)(void *owner);

typedef struct XrClusterPeerIoLease {
    XrIOConn *conn;
    XrClusterOutputQueue *queue;
    void *owner;
    XrClusterPeerLeaseRelease release_owner;
    void *runtime_owner;
    XrClusterPeerLeaseRelease release_runtime_owner;
    _Atomic(uint64_t) *frames_sent;
    _Atomic(uint64_t) *frames_recv;
    _Atomic(uint64_t) *bytes_sent;
    _Atomic(uint64_t) *bytes_recv;
    _Atomic(uint64_t) *send_errors;
    _Atomic(uint64_t) *queue_full_events;
} XrClusterPeerIoLease;

typedef enum XrClusterPeerReadEvent {
    XR_CLUSTER_PEER_READ_STALE = 1,
    XR_CLUSTER_PEER_READ_RUNTIME_STOPPED = 2,
    XR_CLUSTER_PEER_READ_EOF = 3,
    XR_CLUSTER_PEER_READ_IO_ERROR = 4,
    XR_CLUSTER_PEER_READ_INVALID_HEADER = 5,
    XR_CLUSTER_PEER_READ_PAYLOAD_TOO_LARGE = 6,
    XR_CLUSTER_PEER_READ_RESOURCE_UNAVAILABLE = 7,
    XR_CLUSTER_PEER_READ_CANCELLED = 8,
} XrClusterPeerReadEvent;

typedef enum XrClusterPeerWriteEvent {
    XR_CLUSTER_PEER_WRITE_DRAINED = 0,
    XR_CLUSTER_PEER_WRITE_STALE = 1,
    XR_CLUSTER_PEER_WRITE_RUNTIME_STOPPED = 2,
    XR_CLUSTER_PEER_WRITE_QUEUE_FULL = 3,
    XR_CLUSTER_PEER_WRITE_QUEUE_STOPPED = 4,
    XR_CLUSTER_PEER_WRITE_RESOURCE_UNAVAILABLE = 5,
    XR_CLUSTER_PEER_WRITE_SOCKET_CLOSED = 6,
    XR_CLUSTER_PEER_WRITE_IO_ERROR = 7,
    XR_CLUSTER_PEER_WRITE_CANCELLED = 8,
} XrClusterPeerWriteEvent;

/* Both entry points consume lease->owner on every return path. */
XR_FUNC XrCFuncResult xr_cluster_peer_read_frame(struct XrVMRuntime *X,
                                                 const XrClusterPeerIoLease *lease,
                                                 uint32_t max_frame_payload, XrValue *result);
XR_FUNC XrCFuncResult xr_cluster_peer_write_batch(struct XrVMRuntime *X,
                                                  const XrClusterPeerIoLease *lease,
                                                  XrValue *result);

#endif  // XR_IO_CLUSTER_PEER_TRANSPORT_H
