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
     *   4. The writer and reader each own a reference. The last release
     *      destroys the queue and node only after both coroutines have
     *      returned, so shutdown never waits on a worker that may be
     *      needed to run those coroutines.
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

/* ========== Writer Coroutine ========== */

typedef struct XrWriterContext {
    struct XrCluster *cluster;
    XrClusterNode *node;
    XrClusterOutputBatch *frames;
    size_t offset;
    XrValue outbound_handler;
    XrCoroHeap *handler_owner_heap;
    int64_t event_status;
    bool event_pending;
} XrWriterContext;

static void cluster_writer_context_destroy(void *context) {
    XrWriterContext *ctx = (XrWriterContext *) context;
    if (!ctx)
        return;
    xr_cluster_output_batch_drop(ctx->node->outq, ctx->frames);
    if (ctx->event_pending)
        cluster_node_shutdown(ctx->node);
    xr_rc_release_value(ctx->handler_owner_heap, ctx->outbound_handler);
    atomic_store(&ctx->node->writer_running, false);
    atomic_store(&ctx->node->writer_exited, true);
    cluster_node_release(ctx->node);
    cluster_runtime_release(ctx->cluster);
    xr_free(ctx);
}

static XrCFuncResult cluster_writer_drive(XrVMRuntime *X, void *context, XrValue *result);

static XrCFuncResult cluster_writer_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                             void *context, XrValue *result) {
    (void) resume_value;
    XrWriterContext *ctx = (XrWriterContext *) context;
    if (ctx->event_pending)
        return XR_CFUNC_DONE;
    if (status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR)
        return XR_CFUNC_DONE;
    return cluster_writer_drive(X, context, result);
}

static XrCFuncResult cluster_writer_drive(XrVMRuntime *X, void *context, XrValue *result) {
    XrWriterContext *ctx = (XrWriterContext *) context;
    XrClusterNode *node = ctx ? ctx->node : NULL;
    if (!node || !atomic_load(&node->writer_running) || node->state != XR_NODE_CONNECTED ||
        !node->conn)
        return XR_CFUNC_DONE;

    for (int operations = 0; operations < 64; operations++) {
        if (!ctx->frames) {
            bool slow = xr_cluster_output_queue_is_full(node->outq);
            ctx->frames = xr_cluster_output_queue_take_all(node->outq);
            if (slow) {
                atomic_fetch_add(&node->metrics.slow_consumer_events, 1);
                ctx->event_status = 1;
                ctx->event_pending = true;
                return xr_call_closure(X, xr_value_to_closure(ctx->outbound_handler), NULL, 0,
                                       cluster_writer_continue, ctx, result);
            }
        }

        if (!ctx->frames) {
            uint8_t drain[64];
            int notify_fd = xr_cluster_output_queue_notify_fd(node->outq);
            XrIOTryResult read_result =
                xr_socket_read_try(X, notify_fd, (char *) drain, sizeof(drain));
            if (!read_result.ready) {
                return xr_yield_for_io(X, notify_fd, XR_WAIT_READ, -1, cluster_writer_continue, ctx,
                                       result);
            }
            if (read_result.error != 0 || read_result.value == 0)
                goto dispatch_closed;
            continue;
        }

        uint32_t frame_length = xr_cluster_output_batch_length(ctx->frames);
        const uint8_t *frame_data = xr_cluster_output_batch_data(ctx->frames);
        int wait_events = XR_WAIT_WRITE;
        int n = xr_io_conn_write_try(node->conn, frame_data + ctx->offset,
                                     (size_t) frame_length - ctx->offset, &wait_events);
        if (n == -1) {
            return xr_yield_for_io(X, node->conn->fd, wait_events, -1, cluster_writer_continue, ctx,
                                   result);
        }
        if (n <= 0) {
            atomic_fetch_add(&node->metrics.send_errors, 1);
            goto dispatch_closed;
        }

        ctx->offset += (size_t) n;
        atomic_fetch_add(&node->metrics.bytes_sent, (uint64_t) n);
        if (ctx->offset == frame_length) {
            ctx->offset = 0;
            xr_cluster_output_batch_consume(node->outq, &ctx->frames);
            atomic_fetch_add(&node->metrics.frames_sent, 1);
        }
    }

    return xr_yield(X, cluster_writer_continue, ctx);

dispatch_closed:
    ctx->event_status = 2;
    ctx->event_pending = true;
    return xr_call_closure(X, xr_value_to_closure(ctx->outbound_handler), NULL, 0,
                           cluster_writer_continue, ctx, result);
}

/*
 * Frame-processing reader coroutine.
 *
 * Ownership contract:
 *   - cluster_process_node owns disconnect cleanup once it starts:
 *     it removes the node, fires monitors, stops the writer, and frees
 *     the XrClusterNode.
 *   - The reader loop must therefore not dereference node after
 *     cluster_process_node returns. That includes reader_running; the
 *     frame loop clears it immediately before teardown.
 */
typedef struct XrReaderContext {
    struct XrCluster *cluster;
    XrClusterNode *node;
    uint8_t header[XR_FRAME_HEADER_SIZE + 1];
    size_t header_used;
    uint8_t frame_type;
    uint8_t *payload;
    uint32_t payload_len;
    uint32_t payload_used;
    XrValue inbound_handler;
    XrCoroHeap *handler_owner_heap;
    XrValue pending_wire;
    XrCoroHeap *pending_wire_heap;
    int64_t received_at_ms;
    int64_t event_status;
    bool event_pending;
} XrReaderContext;

static void cluster_reader_context_destroy(void *context) {
    XrReaderContext *ctx = (XrReaderContext *) context;
    if (!ctx)
        return;
    if (cluster_node_remove(ctx->cluster, ctx->node)) {
        if (atomic_load(&ctx->cluster->running))
            xr_monitor_registry_notify_node(ctx->cluster->monitors, ctx->cluster->isolate,
                                            ctx->node->name);
        cluster_node_shutdown(ctx->node);
        cluster_node_release(ctx->node);
    } else {
        cluster_node_shutdown(ctx->node);
    }
    xr_free(ctx->payload);
    if (!XR_IS_NULL(ctx->pending_wire))
        xr_rc_release_value(ctx->pending_wire_heap, ctx->pending_wire);
    xr_rc_release_value(ctx->handler_owner_heap, ctx->inbound_handler);
    atomic_store(&ctx->node->reader_running, false);
    cluster_node_release(ctx->node);
    cluster_runtime_release(ctx->cluster);
    xr_free(ctx);
}

static XrCFuncResult cluster_reader_drive(XrVMRuntime *X, void *context, XrValue *result);

static XrCFuncResult cluster_reader_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                             void *context, XrValue *result) {
    (void) resume_value;
    XrReaderContext *ctx = (XrReaderContext *) context;
    if (ctx->event_pending) {
        int64_t event_status = ctx->event_status;
        ctx->event_pending = false;
        if (!XR_IS_NULL(ctx->pending_wire))
            xr_rc_release_value(ctx->pending_wire_heap, ctx->pending_wire);
        ctx->pending_wire = xr_null();
        ctx->pending_wire_heap = NULL;
        xr_free(ctx->payload);
        ctx->payload = NULL;
        ctx->payload_len = 0;
        ctx->payload_used = 0;
        ctx->header_used = 0;
        if (status != XR_RESUME_CLOSURE_DONE) {
            *result = xr_null();
            return XR_CFUNC_DONE;
        }
        if (event_status != 0)
            return XR_CFUNC_DONE;
        atomic_fetch_add(&ctx->node->metrics.frames_recv, 1);
    }
    if (status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR)
        return XR_CFUNC_DONE;
    return cluster_reader_drive(X, ctx, result);
}

static XrCFuncResult cluster_reader_drive(XrVMRuntime *X, void *context, XrValue *result) {
    XrReaderContext *ctx = (XrReaderContext *) context;
    if (!ctx || !atomic_load(&ctx->cluster->running) || ctx->node->state != XR_NODE_CONNECTED ||
        !ctx->node->conn)
        return XR_CFUNC_DONE;

    for (int operations = 0; operations < 64; operations++) {
        uint8_t *target;
        size_t remaining;
        if (ctx->header_used < sizeof(ctx->header)) {
            target = ctx->header + ctx->header_used;
            remaining = sizeof(ctx->header) - ctx->header_used;
        } else {
            target = ctx->payload + ctx->payload_used;
            remaining = (size_t) ctx->payload_len - ctx->payload_used;
        }

        int wait_events = XR_WAIT_READ;
        int n = xr_io_conn_read_try(ctx->node->conn, target, remaining, &wait_events);
        if (n == -1) {
            return xr_yield_for_io(X, ctx->node->conn->fd, wait_events, -1, cluster_reader_continue,
                                   ctx, result);
        }
        if (n <= 0) {
            goto dispatch_closed;
        }

        atomic_fetch_add(&ctx->node->metrics.bytes_recv, (uint64_t) n);
        if (ctx->header_used < sizeof(ctx->header)) {
            ctx->header_used += (size_t) n;
            if (ctx->header_used < sizeof(ctx->header))
                continue;
            if (cluster_frame_read_header(ctx->header, sizeof(ctx->header), &ctx->frame_type,
                                          &ctx->payload_len) != 0 ||
                ctx->payload_len > XR_FRAME_MAX_PAYLOAD) {
                goto dispatch_closed;
            }
            if (ctx->payload_len > 0) {
                ctx->payload = (uint8_t *) xr_malloc(ctx->payload_len);
                if (!ctx->payload) {
                    goto dispatch_closed;
                }
                continue;
            }
        } else {
            ctx->payload_used += (uint32_t) n;
            if (ctx->payload_used < ctx->payload_len)
                continue;
        }

        size_t wire_length = sizeof(ctx->header) + (size_t) ctx->payload_len;
        XrArray *wire = xr_byte_array_new(xr_current_coro(X), (int32_t) wire_length);
        if (!wire)
            goto dispatch_closed;
        memcpy(wire->data, ctx->header, sizeof(ctx->header));
        if (ctx->payload_len > 0)
            memcpy((uint8_t *) wire->data + sizeof(ctx->header), ctx->payload, ctx->payload_len);
        wire->length = (int32_t) wire_length;
        ctx->pending_wire = xr_value_from_array(wire);
        ctx->pending_wire_heap = xr_current_coro_heap();
        ctx->received_at_ms = (int64_t) xr_time_monotonic_ms();
        ctx->event_status = 0;
        ctx->event_pending = true;
        return xr_call_closure(X, xr_value_to_closure(ctx->inbound_handler), NULL, 0,
                               cluster_reader_continue, ctx, result);
    }

    return xr_yield(X, cluster_reader_continue, ctx);

dispatch_closed:
    ctx->received_at_ms = (int64_t) xr_time_monotonic_ms();
    ctx->event_status = 1;
    ctx->event_pending = true;
    return xr_call_closure(X, xr_value_to_closure(ctx->inbound_handler), NULL, 0,
                           cluster_reader_continue, ctx, result);
}

XrValue cluster_take_inbound_frame_fn(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrReaderContext *ctx = (XrReaderContext *) xr_coro_vm_cfunc_context(xr_current_coro(isolate));
    if (!ctx || !ctx->event_pending)
        return xr_null();
    XrObjectInstance *frame = xr_stdlib_record_new(isolate, "cluster", "__ClusterInboundFrame");
    if (!frame)
        return xr_null();
    xr_object_instance_set_by_key(isolate, frame, "peerGeneration",
                                  xr_int((int64_t) ctx->node->generation_token));
    xr_object_instance_set_by_key(isolate, frame, "receivedAtMs", xr_int(ctx->received_at_ms));
    xr_object_instance_set_by_key(isolate, frame, "status", xr_int(ctx->event_status));
    if (!XR_IS_NULL(ctx->pending_wire)) {
        xr_rc_retain_value(ctx->pending_wire);
        xr_object_instance_set_by_key(isolate, frame, "wire", ctx->pending_wire);
    }
    return xr_object_instance_value(frame);
}

XrValue cluster_take_outbound_event_fn(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrWriterContext *ctx = (XrWriterContext *) xr_coro_vm_cfunc_context(xr_current_coro(isolate));
    if (!ctx || !ctx->event_pending)
        return xr_null();
    XrObjectInstance *event = xr_stdlib_record_new(isolate, "cluster", "__ClusterOutboundEvent");
    if (!event)
        return xr_null();
    xr_object_instance_set_by_key(isolate, event, "peerGeneration",
                                  xr_int((int64_t) ctx->node->generation_token));
    xr_object_instance_set_by_key(isolate, event, "status", xr_int(ctx->event_status));
    return xr_object_instance_value(event);
}

bool cluster_node_start_io(struct XrCluster *cluster, XrClusterNode *node, XrValue inbound_handler,
                           XrValue outbound_handler) {
    if (!cluster || !node || !cluster->isolate ||
        !xr_closure_from_callback_arg(cluster->isolate, inbound_handler, "cluster inbound") ||
        !xr_closure_from_callback_arg(cluster->isolate, outbound_handler, "cluster outbound"))
        return false;
    if (atomic_exchange(&node->writer_running, true))
        return false;
    if (atomic_exchange(&node->reader_running, true)) {
        atomic_store(&node->writer_running, false);
        return false;
    }

    XrWriterContext *writer_ctx = (XrWriterContext *) xr_calloc(1, sizeof(XrWriterContext));
    XrReaderContext *reader_ctx = (XrReaderContext *) xr_calloc(1, sizeof(XrReaderContext));
    if (!writer_ctx || !reader_ctx) {
        xr_free(writer_ctx);
        xr_free(reader_ctx);
        atomic_store(&node->writer_running, false);
        atomic_store(&node->reader_running, false);
        return false;
    }
    writer_ctx->cluster = cluster;
    writer_ctx->node = node;
    writer_ctx->outbound_handler = outbound_handler;
    writer_ctx->handler_owner_heap = xr_current_coro_heap();
    xr_rc_retain_value(writer_ctx->outbound_handler);
    reader_ctx->cluster = cluster;
    reader_ctx->node = node;
    reader_ctx->inbound_handler = inbound_handler;
    reader_ctx->handler_owner_heap = xr_current_coro_heap();
    reader_ctx->pending_wire = xr_null();
    xr_rc_retain_value(reader_ctx->inbound_handler);

    node->isolate = cluster->isolate;
    atomic_store(&node->writer_exited, false);
    cluster_node_retain(node);
    cluster_runtime_retain(cluster);
    XrCoroutine *writer = xr_coro_create_vm_cfunc(
        cluster->isolate, cluster_writer_drive, writer_ctx,
        (XrCoroContextDestroy) cluster_writer_context_destroy, "cluster_writer");
    if (!writer) {
        atomic_store(&node->reader_running, false);
        xr_rc_release_value(reader_ctx->handler_owner_heap, reader_ctx->inbound_handler);
        xr_free(reader_ctx);
        return false;
    }

    cluster_node_retain(node);
    cluster_runtime_retain(cluster);
    XrCoroutine *reader = xr_coro_create_vm_cfunc(
        cluster->isolate, cluster_reader_drive, reader_ctx,
        (XrCoroContextDestroy) cluster_reader_context_destroy, "cluster_reader");
    if (!reader) {
        xr_coro_destroy(writer);
        return false;
    }

    XrRuntime *runtime = (XrRuntime *) cluster->isolate->vm.scheduler;
    if (!runtime) {
        xr_coro_destroy(writer);
        xr_coro_destroy(reader);
        return false;
    }

    /* Publish the pair as one scheduler batch. Individual local-runnext spawns
     * can strand the second callback behind a long-lived blocking native loop;
     * batch placement assigns both lanes to stealable/background queues. */
    XrCoroutine *io_coros[2] = {writer, reader};
    xr_runtime_spawn_batch(runtime, io_coros, 2);
    return true;
}
