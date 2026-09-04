/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_peer_transport.c - Opaque cluster peer I/O provider
 *
 * KEY CONCEPT:
 *   The provider owns only socket, queue, framing-buffer and netpoll state for
 *   one operation. It returns bytes or a raw queue/socket event and never
 *   chooses protocol dispatch, retry, overload or peer lifecycle policy.
 */

#include "xcluster_peer_transport.h"

#include "../base/xmalloc.h"
#include "../coro/xcoroutine.h"
#include "../coro/xsocket.h"
#include "../runtime/object/xarray.h"
#include "xcluster_wire.h"

#include <string.h>

typedef enum XrClusterPeerIoKind {
    XR_CLUSTER_PEER_IO_READ,
    XR_CLUSTER_PEER_IO_WRITE,
} XrClusterPeerIoKind;

typedef struct XrClusterPeerIoOperation {
    XrClusterPeerIoKind kind;
    XrClusterPeerIoLease lease;
    XrClusterOutputBatch *frames;
    size_t offset;
    uint8_t header[XR_FRAME_HEADER_SIZE + 1];
    size_t header_used;
    uint8_t *payload;
    uint32_t payload_len;
    uint32_t payload_used;
    uint32_t max_frame_payload;
} XrClusterPeerIoOperation;

static void peer_counter_add(_Atomic(uint64_t) *counter, uint64_t amount) {
    if (counter)
        atomic_fetch_add_explicit(counter, amount, memory_order_relaxed);
}

static void peer_lease_release(const XrClusterPeerIoLease *lease) {
    if (!lease)
        return;
    if (lease->release_owner && lease->owner)
        lease->release_owner(lease->owner);
    if (lease->release_runtime_owner && lease->runtime_owner)
        lease->release_runtime_owner(lease->runtime_owner);
}

static void peer_io_operation_destroy(XrClusterPeerIoOperation *operation) {
    if (!operation)
        return;
    if (operation->frames)
        xr_cluster_output_batch_drop(operation->lease.queue, operation->frames);
    xr_free(operation->payload);
    peer_lease_release(&operation->lease);
    xr_free(operation);
}

static XrClusterPeerIoOperation *peer_io_operation_new(XrClusterPeerIoKind kind,
                                                       const XrClusterPeerIoLease *lease,
                                                       uint32_t max_frame_payload) {
    if (!lease || !lease->conn || !lease->queue || !lease->owner || !lease->release_owner ||
        !lease->runtime_owner || !lease->release_runtime_owner) {
        peer_lease_release(lease);
        return NULL;
    }
    XrClusterPeerIoOperation *operation =
        (XrClusterPeerIoOperation *) xr_calloc(1, sizeof(*operation));
    if (!operation) {
        peer_lease_release(lease);
        return NULL;
    }
    operation->kind = kind;
    operation->lease = *lease;
    operation->max_frame_payload = max_frame_payload;
    return operation;
}

static XrCFuncResult peer_read_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                        void *context, XrValue *result) {
    (void) resume_value;
    XrClusterPeerIoOperation *operation = (XrClusterPeerIoOperation *) context;
    if (!operation) {
        *result = xr_int(XR_CLUSTER_PEER_READ_PROVIDER_ERROR);
        return XR_CFUNC_DONE;
    }
    if (status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR) {
        peer_io_operation_destroy(operation);
        *result = xr_int(status == XR_RESUME_CANCELLED ? XR_CLUSTER_PEER_READ_CANCELLED
                                                      : XR_CLUSTER_PEER_READ_PROVIDER_ERROR);
        return XR_CFUNC_DONE;
    }

    XrClusterPeerReadEvent event = XR_CLUSTER_PEER_READ_IO_ERROR;
    for (;;) {
        uint8_t *target;
        size_t remaining;
        if (operation->header_used < sizeof(operation->header)) {
            target = operation->header + operation->header_used;
            remaining = sizeof(operation->header) - operation->header_used;
        } else {
            target = operation->payload + operation->payload_used;
            remaining = (size_t) operation->payload_len - operation->payload_used;
        }

        int wait_events = XR_WAIT_READ;
        int n = xr_io_conn_read_try(operation->lease.conn, target, remaining, &wait_events);
        if (n == -1)
            return xr_yield_for_io(X, operation->lease.conn->fd, wait_events, -1,
                                   peer_read_continue, operation, result);
        if (n == 0) {
            event = XR_CLUSTER_PEER_READ_EOF;
            break;
        }
        if (n < 0) {
            event = XR_CLUSTER_PEER_READ_IO_ERROR;
            break;
        }

        peer_counter_add(operation->lease.bytes_recv, (uint64_t) n);
        if (operation->header_used < sizeof(operation->header)) {
            operation->header_used += (size_t) n;
            if (operation->header_used < sizeof(operation->header))
                continue;
            uint32_t total =
                ((uint32_t) operation->header[0] << 24) | ((uint32_t) operation->header[1] << 16) |
                ((uint32_t) operation->header[2] << 8) | (uint32_t) operation->header[3];
            if (total < 1) {
                event = XR_CLUSTER_PEER_READ_INVALID_LENGTH;
                break;
            }
            if (total > operation->max_frame_payload) {
                event = XR_CLUSTER_PEER_READ_PAYLOAD_TOO_LARGE;
                break;
            }
            operation->payload_len = total - 1;
            if (operation->payload_len > 0) {
                operation->payload = (uint8_t *) xr_malloc(operation->payload_len);
                if (!operation->payload) {
                    event = XR_CLUSTER_PEER_READ_RESOURCE_UNAVAILABLE;
                    break;
                }
                continue;
            }
        } else {
            operation->payload_used += (uint32_t) n;
            if (operation->payload_used < operation->payload_len)
                continue;
        }

        size_t wire_length = sizeof(operation->header) + (size_t) operation->payload_len;
        XrArray *wire = xr_byte_array_new(xr_current_coro(X), (int32_t) wire_length);
        if (!wire) {
            event = XR_CLUSTER_PEER_READ_RESOURCE_UNAVAILABLE;
            break;
        }
        memcpy(wire->data, operation->header, sizeof(operation->header));
        if (operation->payload_len > 0)
            memcpy((uint8_t *) wire->data + sizeof(operation->header), operation->payload,
                   operation->payload_len);
        wire->length = (int32_t) wire_length;
        XrValue wire_value = xr_value_from_array(wire);
        peer_counter_add(operation->lease.frames_recv, 1);
        peer_io_operation_destroy(operation);
        *result = wire_value;
        return XR_CFUNC_DONE;
    }

    peer_io_operation_destroy(operation);
    *result = xr_int(event);
    return XR_CFUNC_DONE;
}

static XrCFuncResult peer_write_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                         void *context, XrValue *result) {
    (void) resume_value;
    XrClusterPeerIoOperation *operation = (XrClusterPeerIoOperation *) context;
    if (!operation) {
        *result = xr_int(XR_CLUSTER_PEER_WRITE_PROVIDER_ERROR);
        return XR_CFUNC_DONE;
    }
    if (status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR) {
        peer_io_operation_destroy(operation);
        *result = xr_int(status == XR_RESUME_CANCELLED ? XR_CLUSTER_PEER_WRITE_CANCELLED
                                                      : XR_CLUSTER_PEER_WRITE_PROVIDER_ERROR);
        return XR_CFUNC_DONE;
    }

    XrClusterPeerWriteEvent event = XR_CLUSTER_PEER_WRITE_IO_ERROR;
    for (;;) {
        if (!operation->frames) {
            if (xr_cluster_output_queue_is_full(operation->lease.queue)) {
                peer_counter_add(operation->lease.queue_full_events, 1);
                peer_io_operation_destroy(operation);
                *result = xr_int(XR_CLUSTER_PEER_WRITE_QUEUE_FULL);
                return XR_CFUNC_DONE;
            }
            operation->frames = xr_cluster_output_queue_take_all(operation->lease.queue);
        }

        if (!operation->frames) {
            uint8_t drain[64];
            int notify_fd = xr_cluster_output_queue_notify_fd(operation->lease.queue);
            XrIOTryResult read_result =
                xr_socket_read_try(X, notify_fd, (char *) drain, sizeof(drain));
            if (!read_result.ready)
                return xr_yield_for_io(X, notify_fd, XR_WAIT_READ, -1, peer_write_continue,
                                       operation, result);
            if (read_result.error != 0) {
                event = XR_CLUSTER_PEER_WRITE_IO_ERROR;
                break;
            }
            if (read_result.value == 0) {
                event = XR_CLUSTER_PEER_WRITE_QUEUE_STOPPED;
                break;
            }
            continue;
        }

        uint32_t frame_length = xr_cluster_output_batch_length(operation->frames);
        const uint8_t *frame_data = xr_cluster_output_batch_data(operation->frames);
        int wait_events = XR_WAIT_WRITE;
        int n = xr_io_conn_write_try(operation->lease.conn, frame_data + operation->offset,
                                     (size_t) frame_length - operation->offset, &wait_events);
        if (n == -1)
            return xr_yield_for_io(X, operation->lease.conn->fd, wait_events, -1,
                                   peer_write_continue, operation, result);
        if (n == 0) {
            event = XR_CLUSTER_PEER_WRITE_SOCKET_CLOSED;
            break;
        }
        if (n < 0) {
            peer_counter_add(operation->lease.send_errors, 1);
            event = XR_CLUSTER_PEER_WRITE_IO_ERROR;
            break;
        }

        operation->offset += (size_t) n;
        peer_counter_add(operation->lease.bytes_sent, (uint64_t) n);
        if (operation->offset != frame_length)
            continue;
        operation->offset = 0;
        xr_cluster_output_batch_consume(operation->lease.queue, &operation->frames);
        peer_counter_add(operation->lease.frames_sent, 1);
        if (!operation->frames) {
            peer_io_operation_destroy(operation);
            *result = xr_int(XR_CLUSTER_PEER_WRITE_DRAINED);
            return XR_CFUNC_DONE;
        }
    }

    peer_io_operation_destroy(operation);
    *result = xr_int(event);
    return XR_CFUNC_DONE;
}

XrCFuncResult xr_cluster_peer_read_frame(XrVMRuntime *X, const XrClusterPeerIoLease *lease,
                                         uint32_t max_frame_payload, XrValue *result) {
    if (!X || !result) {
        peer_lease_release(lease);
        return XR_CFUNC_ERROR;
    }
    if (max_frame_payload == 0 ||
        max_frame_payload > (uint32_t) (INT32_MAX - XR_FRAME_HEADER_SIZE)) {
        peer_lease_release(lease);
        *result = xr_int(XR_CLUSTER_PEER_READ_INVALID_LIMIT);
        return XR_CFUNC_DONE;
    }
    XrClusterPeerIoOperation *operation =
        peer_io_operation_new(XR_CLUSTER_PEER_IO_READ, lease, max_frame_payload);
    if (!operation) {
        *result = xr_int(XR_CLUSTER_PEER_READ_RESOURCE_UNAVAILABLE);
        return XR_CFUNC_DONE;
    }
    return peer_read_continue(X, XR_RESUME_OK, xr_null(), operation, result);
}

XrCFuncResult xr_cluster_peer_write_batch(XrVMRuntime *X, const XrClusterPeerIoLease *lease,
                                          XrValue *result) {
    if (!X || !result) {
        peer_lease_release(lease);
        return XR_CFUNC_ERROR;
    }
    XrClusterPeerIoOperation *operation = peer_io_operation_new(XR_CLUSTER_PEER_IO_WRITE, lease, 0);
    if (!operation) {
        *result = xr_int(XR_CLUSTER_PEER_WRITE_RESOURCE_UNAVAILABLE);
        return XR_CFUNC_DONE;
    }
    return peer_write_continue(X, XR_RESUME_OK, xr_null(), operation, result);
}
