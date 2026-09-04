/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_node.c - Cluster node connection management
 *
 * KEY CONCEPT:
 *   Manages TCP connections between cluster nodes.
 *   Implements challenge-response handshake with SHA-256.
 *   Provides frame-level send/recv over the connection.
 */

#include "cluster_internal.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/coro/xsocket.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/module/xstdlib_runtime_cache.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/vm/xvm_closure.h"
#include "../../src/vm/xvm_coro_api.h"
#include "../../src/base/xchecks.h"
#include "../../src/os/os_random.h"
#include "../../src/os/os_time.h"

#include "../../src/os/os_net.h"
#include <stdlib.h>
#include <string.h>

/* ========== Peer Resource Provider ========== */

void cluster_node_retain(XrClusterNode *node) {
    XR_DCHECK(node != NULL, "cluster node retain requires a node");
    if (node)
        atomic_fetch_add(&node->ref_count, 1);
}

void cluster_node_shutdown(XrClusterNode *node) {
    if (!node)
        return;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&node->shutdown_started, &expected, true))
        return;

    /*
     * Teardown sequence — order matters because the writer coroutine
     * may be yielded inside xr_socket_read(notify_pipe[0]):
     *
     *   1. Clear writer_running so any writer iteration after the
     *      next wake observes the stop signal.
     *   2. Stop the output queue. Its provider closes the write end of the
     *      wakeup pair, so xr_socket_read on the read end returns 0 (EOF),
     *      wakes the coroutine, and lets the writer exit.
     *   3. Shut down the peer socket. Any in-flight send fails cleanly;
     *      the writer loop's early checks on node->conn bail out.
     *   4. Each in-flight framed read or queued write owns a reference. The
     *      last operation release destroys the queue and node only after its
     *      netpoll continuation returns, so shutdown never waits on a worker
     *      that may be needed to resume the operation.
     */
    atomic_store(&node->writer_running, false);
    xr_cluster_output_queue_stop(node->outq);
    node->state = XR_NODE_CLOSING;
    /* Keep XrIOConn allocated until the last reader/writer reference exits.
     * shutdown(2) wakes concurrent socket I/O without letting the fd or the
     * connection object be reused underneath an in-flight coroutine. */
    if (node->conn && node->conn->fd >= 0)
        (void) shutdown(node->conn->fd, XR_SHUT_RDWR);
}

void cluster_node_release(XrClusterNode *node) {
    if (!node)
        return;
    uint32_t previous = atomic_fetch_sub(&node->ref_count, 1);
    XR_DCHECK(previous > 0, "cluster node reference underflow");
    if (previous == 1) {
        cluster_node_shutdown(node);
        XR_DCHECK(atomic_load(&node->shutdown_started), "cluster node destroyed before shutdown");
        if (node->conn) {
            xr_io_close(node->conn);
            node->conn = NULL;
        }
        xr_cluster_output_queue_destroy(node->outq);
        xr_free(node);
    }
}

/* ========== Opaque Peer Transport Provider ========== */

typedef enum XrPeerIoKind {
    XR_PEER_IO_READ,
    XR_PEER_IO_WRITE,
} XrPeerIoKind;

typedef struct XrPeerIoOperation {
    XrPeerIoKind kind;
    XrClusterNode *node;
    XrClusterOutputBatch *frames;
    size_t offset;
    uint8_t header[XR_FRAME_HEADER_SIZE + 1];
    size_t header_used;
    uint8_t *payload;
    uint32_t payload_len;
    uint32_t payload_used;
} XrPeerIoOperation;

/*
 * One provider continuation serves both transport directions. Each source
 * call owns exactly one framed read or one queued write batch. Resubmission,
 * fairness, slow-consumer handling and terminal disconnect remain decisions
 * of the Xray loops that call these leaves.
 */
static XrCFuncResult cluster_peer_io_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                              void *context, XrValue *result) {
    (void) resume_value;
    XrPeerIoOperation *operation = (XrPeerIoOperation *) context;
    if (!operation) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    if (status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR) {
        if (operation->frames)
            xr_cluster_output_batch_drop(operation->node->outq, operation->frames);
        xr_free(operation->payload);
        cluster_node_release(operation->node);
        XrPeerIoKind kind = operation->kind;
        xr_free(operation);
        *result = kind == XR_PEER_IO_WRITE ? xr_int(-1) : xr_null();
        return XR_CFUNC_DONE;
    }

    if (operation->kind == XR_PEER_IO_WRITE) {
        XrClusterNode *node = operation->node;
        int64_t event_status = 0;
        if (!atomic_load(&node->writer_running) || node->state != XR_NODE_CONNECTED ||
            !node->conn) {
            event_status = 2;
            goto writer_done;
        }

        for (;;) {
            if (!operation->frames) {
                bool slow = xr_cluster_output_queue_is_full(node->outq);
                operation->frames = xr_cluster_output_queue_take_all(node->outq);
                if (slow) {
                    atomic_fetch_add(&node->metrics.slow_consumer_events, 1);
                    event_status = 1;
                    goto writer_done;
                }
            }

            if (!operation->frames) {
                uint8_t drain[64];
                int notify_fd = xr_cluster_output_queue_notify_fd(node->outq);
                XrIOTryResult read_result =
                    xr_socket_read_try(X, notify_fd, (char *) drain, sizeof(drain));
                if (!read_result.ready)
                    return xr_yield_for_io(X, notify_fd, XR_WAIT_READ, -1,
                                           cluster_peer_io_continue, operation, result);
                if (read_result.error != 0 || read_result.value == 0) {
                    event_status = 2;
                    goto writer_done;
                }
                continue;
            }

            uint32_t frame_length = xr_cluster_output_batch_length(operation->frames);
            const uint8_t *frame_data = xr_cluster_output_batch_data(operation->frames);
            int wait_events = XR_WAIT_WRITE;
            int n = xr_io_conn_write_try(node->conn, frame_data + operation->offset,
                                         (size_t) frame_length - operation->offset, &wait_events);
            if (n == -1)
                return xr_yield_for_io(X, node->conn->fd, wait_events, -1,
                                       cluster_peer_io_continue, operation, result);
            if (n <= 0) {
                atomic_fetch_add(&node->metrics.send_errors, 1);
                event_status = 2;
                goto writer_done;
            }

            operation->offset += (size_t) n;
            atomic_fetch_add(&node->metrics.bytes_sent, (uint64_t) n);
            if (operation->offset != frame_length)
                continue;
            operation->offset = 0;
            xr_cluster_output_batch_consume(node->outq, &operation->frames);
            atomic_fetch_add(&node->metrics.frames_sent, 1);
            if (!operation->frames)
                goto writer_done;
        }

    writer_done:
        if (operation->frames)
            xr_cluster_output_batch_drop(node->outq, operation->frames);
        cluster_node_release(node);
        xr_free(operation);
        *result = xr_int(event_status);
        return XR_CFUNC_DONE;
    }

    XrClusterNode *node = operation->node;
    int64_t event_status = 1;
    XrValue wire_value = xr_null();
    if (node->state != XR_NODE_CONNECTED || !node->conn)
        goto reader_done;

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
        int n = xr_io_conn_read_try(node->conn, target, remaining, &wait_events);
        if (n == -1)
            return xr_yield_for_io(X, node->conn->fd, wait_events, -1,
                                   cluster_peer_io_continue, operation, result);
        if (n <= 0)
            goto reader_done;

        atomic_fetch_add(&node->metrics.bytes_recv, (uint64_t) n);
        if (operation->header_used < sizeof(operation->header)) {
            operation->header_used += (size_t) n;
            if (operation->header_used < sizeof(operation->header))
                continue;
            uint8_t frame_type = 0;
            if (cluster_frame_read_header(operation->header, sizeof(operation->header), &frame_type,
                                          &operation->payload_len) != 0 ||
                operation->payload_len > XR_FRAME_MAX_PAYLOAD)
                goto reader_done;
            if (operation->payload_len > 0) {
                operation->payload = (uint8_t *) xr_malloc(operation->payload_len);
                if (!operation->payload)
                    goto reader_done;
                continue;
            }
        } else {
            operation->payload_used += (uint32_t) n;
            if (operation->payload_used < operation->payload_len)
                continue;
        }

        size_t wire_length = sizeof(operation->header) + (size_t) operation->payload_len;
        XrArray *wire = xr_byte_array_new(xr_current_coro(X), (int32_t) wire_length);
        if (!wire)
            goto reader_done;
        memcpy(wire->data, operation->header, sizeof(operation->header));
        if (operation->payload_len > 0)
            memcpy((uint8_t *) wire->data + sizeof(operation->header), operation->payload,
                   operation->payload_len);
        wire->length = (int32_t) wire_length;
        wire_value = xr_value_from_array(wire);
        event_status = 0;
        atomic_fetch_add(&node->metrics.frames_recv, 1);
        break;
    }

reader_done: {
        int64_t received_at_ms = (int64_t) xr_time_monotonic_ms();
        XrObjectInstance *frame = xr_stdlib_record_new(X, "cluster", "__ClusterInboundFrame");
        if (frame) {
            xr_object_instance_set_by_key(X, frame, "receivedAtMs", xr_int(received_at_ms));
            xr_object_instance_set_by_key(X, frame, "status", xr_int(event_status));
            if (!XR_IS_NULL(wire_value))
                xr_object_instance_set_by_key(X, frame, "wire", wire_value);
        }
        xr_free(operation->payload);
        cluster_node_release(node);
        xr_free(operation);
        *result = frame ? xr_object_instance_value(frame) : xr_null();
        return XR_CFUNC_DONE;
    }
}

XrCFuncResult cluster_peer_read_fn(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 1 || !XR_IS_INT(args[0])) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }
    XrCluster *cluster = (XrCluster *) X->cluster;
    if (!cluster || !atomic_load(&cluster->running)) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    XrClusterNode *node = NULL;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *candidate = cluster->nodes; candidate; candidate = candidate->next) {
        if (candidate->generation_token == generation && candidate->state == XR_NODE_CONNECTED &&
            candidate->conn) {
            node = candidate;
            cluster_node_retain(node);
            break;
        }
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    if (!node) {
        *result = xr_null();
        return XR_CFUNC_DONE;
    }

    XrPeerIoOperation *operation = (XrPeerIoOperation *) xr_calloc(1, sizeof(*operation));
    if (!operation) {
        cluster_node_release(node);
        *result = xr_null();
        return XR_CFUNC_DONE;
    }
    operation->kind = XR_PEER_IO_READ;
    operation->node = node;
    return cluster_peer_io_continue(X, XR_RESUME_OK, xr_null(), operation, result);
}

XrCFuncResult cluster_peer_write_fn(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 1 || !XR_IS_INT(args[0])) {
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }
    XrCluster *cluster = (XrCluster *) X->cluster;
    if (!cluster || !atomic_load(&cluster->running)) {
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    XrClusterNode *node = NULL;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *candidate = cluster->nodes; candidate; candidate = candidate->next) {
        if (candidate->generation_token == generation && candidate->state == XR_NODE_CONNECTED &&
            candidate->conn) {
            node = candidate;
            cluster_node_retain(node);
            break;
        }
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    if (!node) {
        *result = xr_int(-1);
        return XR_CFUNC_DONE;
    }

    XrPeerIoOperation *operation = (XrPeerIoOperation *) xr_calloc(1, sizeof(*operation));
    if (!operation) {
        cluster_node_release(node);
        *result = xr_int(2);
        return XR_CFUNC_DONE;
    }
    operation->kind = XR_PEER_IO_WRITE;
    operation->node = node;
    return cluster_peer_io_continue(X, XR_RESUME_OK, xr_null(), operation, result);
}

bool cluster_node_start_io(struct XrCluster *cluster, XrClusterNode *node, XrValue inbound_handler,
                           XrValue outbound_handler) {
    if (!cluster || !node || !cluster->isolate)
        return false;
    XrClosure *reader_handler =
        xr_closure_from_callback_arg(cluster->isolate, inbound_handler, "cluster inbound");
    XrClosure *writer_handler =
        xr_closure_from_callback_arg(cluster->isolate, outbound_handler, "cluster outbound");
    XrRuntime *runtime = (XrRuntime *) cluster->isolate->vm.scheduler;
    if (!reader_handler || !writer_handler || !runtime)
        return false;
    if (atomic_exchange(&node->writer_running, true))
        return false;
    if (atomic_exchange(&node->reader_running, true)) {
        atomic_store(&node->writer_running, false);
        return false;
    }

    node->isolate = cluster->isolate;
    atomic_store(&node->writer_exited, false);
    XrValue generation = xr_int((int64_t) node->generation_token);
    const uint8_t arg_mode = XR_TRANSFER_SHARE;
    XrCoroutine *writer = xr_coro_create_vm_closure_owned(
        cluster->isolate, writer_handler, &generation, &arg_mode, 1, "cluster_writer", NULL, 0);
    if (!writer) {
        atomic_store(&node->writer_running, false);
        atomic_store(&node->reader_running, false);
        return false;
    }

    XrCoroutine *reader = xr_coro_create_vm_closure_owned(
        cluster->isolate, reader_handler, &generation, &arg_mode, 1, "cluster_reader", NULL, 0);
    if (!reader) {
        xr_coro_destroy(writer);
        atomic_store(&node->writer_running, false);
        atomic_store(&node->reader_running, false);
        return false;
    }

    /* The native side only starts the two source-owned loops as one scheduler
     * batch. Each loop chooses whether and when to request another I/O step. */
    XrCoroutine *io_coros[2] = {writer, reader};
    xr_runtime_spawn_batch(runtime, io_coros, 2);
    return true;
}
