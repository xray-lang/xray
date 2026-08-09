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
#include "../../src/base/xchecks.h"
#include "../../src/os/os_random.h"
#include "../../src/os/os_time.h"

#include "../../src/os/os_net.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void cluster_node_close(XrClusterNode *node);

/* ========== Time Utility ========== */

int64_t cluster_now_ms(void) {
    return (int64_t) xr_time_monotonic_ms();
}

/* ========== Output Queue ========== */

static void cluster_outq_init(XrOutputQueue *q) {
    q->head = NULL;
    q->tail = NULL;
    q->total_bytes = 0;
    q->frame_count = 0;
    q->high_watermark = 4 * 1024 * 1024;  // 4MB
    q->low_watermark = 1 * 1024 * 1024;   // 1MB
    atomic_store(&q->is_full, false);
    atomic_store(&q->pending_frames, 0);
    // Create pipe for writer wakeup notification
    int rc = pipe(q->notify_pipe);
    XR_DCHECK(rc == 0, "pipe() failed for writer notification");
    if (rc != 0) {
        q->notify_pipe[0] = q->notify_pipe[1] = -1;
    }
    /*
     * Both ends non-blocking:
     *   - Write end: enqueue (outq_notify) never blocks even if the
     *     pipe already has pending bytes.
     *   - Read end: the writer coroutine drains via xr_socket_read,
     *     which requires a non-blocking fd so it can suspend via
     *     netpoll instead of pinning the worker thread in a raw
     *     read(2) syscall.
     */
    if (q->notify_pipe[1] >= 0)
        fcntl(q->notify_pipe[1], F_SETFL, O_NONBLOCK);
    if (q->notify_pipe[0] >= 0)
        fcntl(q->notify_pipe[0], F_SETFL, O_NONBLOCK);
    xr_amutex_init(&q->lock);
}

/*
 * Close the writer-facing side of the notify pipe early. Used by
 * cluster_node_shutdown to wake any coroutine yielded on
 * xr_socket_read(notify_pipe[0]) with a clean EOF before we tear
 * down the rest of the node state. Calling this multiple times is
 * safe — it guards on the fd being >= 0.
 *
 * This is split out from cluster_outq_destroy specifically because the
 * destroy path wants to close the *read* end only after the writer
 * coroutine has exited; closing the read end while netpoll still
 * has the fd registered is a use-after-close on the PollDesc.
 */
static void cluster_outq_close_write_end(XrOutputQueue *q) {
    if (q->notify_pipe[1] >= 0) {
        close(q->notify_pipe[1]);
        q->notify_pipe[1] = -1;
    }
}

static void cluster_outq_destroy(XrOutputQueue *q) {
    XrOutFrame *f = q->head;
    while (f) {
        XrOutFrame *next = f->next;
        if (f->owned)
            xr_free(f->data);
        xr_free(f);
        f = next;
    }
    q->head = q->tail = NULL;
    q->total_bytes = 0;
    q->frame_count = 0;
    atomic_store(&q->pending_frames, 0);
    // Close both ends. Write end may already be -1 from an earlier
    // cluster_outq_close_write_end during writer teardown; close is
    // idempotent against our own -1 guard.
    if (q->notify_pipe[1] >= 0)
        close(q->notify_pipe[1]);
    if (q->notify_pipe[0] >= 0)
        close(q->notify_pipe[0]);
    q->notify_pipe[0] = q->notify_pipe[1] = -1;
}

// Signal writer coroutine that data is available
static inline void outq_notify(XrOutputQueue *q) {
    if (q->notify_pipe[1] >= 0) {
        uint8_t byte = 1;
        // Ignore EAGAIN (pipe already has data)
        (void) write(q->notify_pipe[1], &byte, 1);
    }
}

// Internal helper: enqueue a frame node into the queue
static void outq_enqueue_locked(XrOutputQueue *q, XrOutFrame *f) {
    if (q->tail) {
        q->tail->next = f;
    } else {
        q->head = f;
    }
    q->tail = f;
    q->total_bytes += f->len;
    q->frame_count++;
    atomic_fetch_add(&q->pending_frames, 1);
    if (q->total_bytes >= q->high_watermark) {
        atomic_store(&q->is_full, true);
    }
}

static int cluster_outq_push(XrOutputQueue *q, const uint8_t *data, uint32_t len) {
    if (atomic_load(&q->is_full))
        return -1;

    XrOutFrame *f = (XrOutFrame *) xr_malloc(sizeof(XrOutFrame));
    if (!f)
        return -1;
    f->data = (uint8_t *) xr_malloc(len);
    if (!f->data) {
        xr_free(f);
        return -1;
    }
    memcpy(f->data, data, len);
    f->len = len;
    f->owned = true;
    f->next = NULL;

    xr_amutex_lock(&q->lock);
    outq_enqueue_locked(q, f);
    xr_amutex_unlock(&q->lock);
    outq_notify(q);
    return 0;
}

// Zero-copy push: takes ownership of the data pointer (caller must have malloc'd it)
static int cluster_outq_push_nocopy(XrOutputQueue *q, uint8_t *data, uint32_t len) {
    if (atomic_load(&q->is_full))
        return -1;

    XrOutFrame *f = (XrOutFrame *) xr_malloc(sizeof(XrOutFrame));
    if (!f)
        return -1;
    f->data = data;
    f->len = len;
    f->owned = true;  // we own it (caller transferred ownership)
    f->next = NULL;

    xr_amutex_lock(&q->lock);
    outq_enqueue_locked(q, f);
    xr_amutex_unlock(&q->lock);
    outq_notify(q);
    return 0;
}

static XrOutFrame *cluster_outq_pop_all(XrOutputQueue *q) {
    xr_amutex_lock(&q->lock);
    XrOutFrame *batch = q->head;
    q->head = NULL;
    q->tail = NULL;
    q->total_bytes = 0;
    q->frame_count = 0;
    atomic_store(&q->is_full, false);
    xr_amutex_unlock(&q->lock);
    return batch;
}

/* ========== Phi Accrual Failure Detector ========== */

void cluster_phi_init(XrPhiDetector *det) {
    memset(det, 0, sizeof(XrPhiDetector));
    det->mean = 5000.0;  // assume 5s heartbeat interval initially
    det->variance = 100.0;
    det->sum = 0.0;
    det->sum_sq = 0.0;
}

void cluster_phi_record_heartbeat(XrPhiDetector *det, int64_t now_ms) {
    if (det->last_heartbeat_ts > 0) {
        double interval = (double) (now_ms - det->last_heartbeat_ts);

        // O(1) incremental update: subtract old sample if ring buffer full
        if (det->sample_count >= XR_PHI_WINDOW_SIZE) {
            double old_val = det->intervals[det->write_idx];
            det->sum -= old_val;
            det->sum_sq -= old_val * old_val;
        } else {
            det->sample_count++;
        }

        det->intervals[det->write_idx] = interval;
        det->write_idx = (det->write_idx + 1) % XR_PHI_WINDOW_SIZE;

        det->sum += interval;
        det->sum_sq += interval * interval;

        det->mean = det->sum / det->sample_count;
        det->variance = (det->sum_sq / det->sample_count) - (det->mean * det->mean);
        if (det->variance < 1.0)
            det->variance = 1.0;  // avoid zero from fp drift
    }
    det->last_heartbeat_ts = now_ms;
}

double cluster_phi_value(XrPhiDetector *det, int64_t now_ms) {
    if (det->last_heartbeat_ts == 0 || det->sample_count < 2)
        return 0.0;

    double elapsed = (double) (now_ms - det->last_heartbeat_ts);
    double stddev = sqrt(det->variance);
    if (stddev < 1.0)
        stddev = 1.0;

    // phi = -log10(1 - CDF(elapsed))
    // Using normal distribution CDF approximation
    double y = (elapsed - det->mean) / stddev;
    // Logistic approximation to normal CDF: 1 / (1 + exp(-1.7155 * y))
    double p_later = 1.0 / (1.0 + exp(1.7155 * y));
    if (p_later < 1e-15)
        p_later = 1e-15;
    return -log10(p_later);
}

/* ========== Node Lifecycle ========== */

XrClusterNode *cluster_node_new(const char *name, const char *host, uint16_t port) {
    XrClusterNode *node = (XrClusterNode *) xr_calloc(1, sizeof(XrClusterNode));
    if (!node)
        return NULL;

    atomic_store(&node->ref_count, 1);
    atomic_store(&node->shutdown_started, false);
    if (name) {
        strncpy(node->name, name, XR_NODE_NAME_MAX);
        node->name[XR_NODE_NAME_MAX] = '\0';
    }
    if (host) {
        strncpy(node->host, host, sizeof(node->host) - 1);
    }
    node->port = port;
    node->state = XR_NODE_IDLE;
    node->conn = NULL;
    node->last_heartbeat_sent = 0;
    node->last_heartbeat_recv = 0;
    node->missed_heartbeats = 0;
    cluster_outq_init(&node->outq);
    atomic_store(&node->writer_running, false);
    atomic_store(&node->writer_exited, false);
    atomic_store(&node->reader_running, false);
    cluster_phi_init(&node->phi);
    node->next = NULL;
    return node;
}

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
     *   2. Close the write end of notify_pipe (via the dedicated
     *      cluster_outq_close_write_end helper). xr_socket_read on the
     *      read end then returns 0 (EOF), wakes the coroutine, the
     *      loop breaks, and the writer sets writer_exited.
     *   3. Close the peer socket. Any in-flight send fails cleanly;
     *      the writer loop's early checks on node->conn bail out.
     *   4. The writer and reader each own a reference. The last release
     *      destroys the queue and node only after both coroutines have
     *      returned, so shutdown never waits on a worker that may be
     *      needed to run those coroutines.
     *
     * The bounded wait is 500ms total at 1ms granularity; in the
     * common case the writer exits within the first or second poll.
     * If we truly time out (pathological scheduler starvation) we
     * proceed anyway — the kernel's fd close will eventually wake
     * any stuck reader with EBADF.
     */
    atomic_store(&node->writer_running, false);
    cluster_outq_close_write_end(&node->outq);
    cluster_node_close(node);
}

static void cluster_node_destroy(XrClusterNode *node) {
    XR_DCHECK(atomic_load(&node->shutdown_started), "cluster node destroyed before shutdown");
    if (node->conn) {
        xr_io_close(node->conn);
        node->conn = NULL;
    }
    cluster_outq_destroy(&node->outq);
    xr_free(node);
}

void cluster_node_release(XrClusterNode *node) {
    if (!node)
        return;
    uint32_t previous = atomic_fetch_sub(&node->ref_count, 1);
    XR_DCHECK(previous > 0, "cluster node reference underflow");
    if (previous == 1) {
        cluster_node_shutdown(node);
        cluster_node_destroy(node);
    }
}

static void cluster_node_close(XrClusterNode *node) {
    if (!node)
        return;
    node->state = XR_NODE_CLOSING;
    /* Keep XrIOConn allocated until the last reader/writer reference exits.
     * shutdown(2) wakes concurrent socket I/O without letting the fd or the
     * connection object be reused underneath an in-flight coroutine. */
    if (node->conn && node->conn->fd >= 0)
        (void) shutdown(node->conn->fd, XR_SHUT_RDWR);
}

/* ========== Frame Send/Recv ========== */

// Enqueue pre-built frame data for async writing
int cluster_node_enqueue(XrClusterNode *node, const uint8_t *data, uint32_t len) {
    if (!node || atomic_load(&node->shutdown_started) || node->state == XR_NODE_CLOSING)
        return -1;
    return cluster_outq_push(&node->outq, data, len);
}

// Async send — encode frame and enqueue for writer coroutine
// Uses zero-copy for large frames (>4KB) to avoid extra memcpy
int cluster_node_send_frame(XrClusterNode *node, uint8_t frame_type, const uint8_t *payload,
                            uint32_t payload_len) {
    if (!node || atomic_load(&node->shutdown_started) || !node->conn ||
        node->state == XR_NODE_CLOSING)
        return -1;

    uint32_t frame_size = 4 + 1 + payload_len;

    if (frame_size <= 4096) {
        // Small frame: encode to stack, copy into queue
        uint8_t stack_buf[4096];
        int wrote = cluster_frame_write(stack_buf, frame_type, payload, payload_len);
        if (wrote < 0)
            return -1;
        return cluster_node_enqueue(node, stack_buf, (uint32_t) wrote);
    } else {
        // Large frame: encode to heap, transfer ownership (zero-copy)
        uint8_t *frame = (uint8_t *) xr_malloc(frame_size);
        if (!frame)
            return -1;
        int wrote = cluster_frame_write(frame, frame_type, payload, payload_len);
        if (wrote < 0) {
            xr_free(frame);
            return -1;
        }
        int rc = cluster_outq_push_nocopy(&node->outq, frame, (uint32_t) wrote);
        if (rc != 0) {
            xr_free(frame);
            return -1;
        }
        return 0;
    }
}

int cluster_node_send_transport_frame(XrClusterNode *node, uint8_t hop_limit, const char *topic,
                                      uint8_t topic_len, const uint8_t *envelope,
                                      uint32_t envelope_len) {
    if (!node || atomic_load(&node->shutdown_started) || !node->conn ||
        node->state == XR_NODE_CLOSING || !topic || topic_len == 0 || !envelope)
        return -1;

    size_t frame_len = (size_t) XR_FRAME_HEADER_SIZE + 3u + topic_len + envelope_len;
    if (frame_len > UINT32_MAX)
        return -1;
    uint8_t *frame = (uint8_t *) xr_malloc(frame_len);
    if (!frame)
        return -1;
    int wrote = cluster_frame_write_transport(frame, frame_len, hop_limit, topic, topic_len,
                                              envelope, envelope_len);
    if (wrote < 0) {
        xr_free(frame);
        return -1;
    }
    int rc = cluster_outq_push_nocopy(&node->outq, frame, (uint32_t) wrote);
    if (rc != 0) {
        xr_free(frame);
        return -1;
    }
    return 0;
}

/* ========== Writer Coroutine ========== */

typedef struct XrWriterContext {
    XrClusterNode *node;
    XrOutFrame *frames;
    size_t offset;
} XrWriterContext;

static void cluster_writer_drop_frames(XrWriterContext *ctx) {
    while (ctx && ctx->frames) {
        XrOutFrame *frame = ctx->frames;
        ctx->frames = frame->next;
        if (frame->owned)
            xr_free(frame->data);
        xr_free(frame);
        atomic_fetch_sub(&ctx->node->outq.pending_frames, 1);
    }
    if (ctx)
        ctx->offset = 0;
}

static void cluster_writer_context_destroy(void *context) {
    XrWriterContext *ctx = (XrWriterContext *) context;
    if (!ctx)
        return;
    cluster_writer_drop_frames(ctx);
    atomic_store(&ctx->node->writer_running, false);
    atomic_store(&ctx->node->writer_exited, true);
    cluster_node_release(ctx->node);
    xr_free(ctx);
}

/*
 * One non-blocking connection write. wait_events reports the readiness
 * direction required by TLS; plain TCP always waits for writability.
 */
int cluster_conn_write_try(XrIOConn *conn, const uint8_t *data, size_t len, int *wait_events) {
    if (!conn || conn->fd < 0)
        return -3;
    if (conn->is_tls) {
        int n = xr_tls_conn_write_try(conn->tls, data, len);
        if (n == -1) {
            *wait_events = XR_WAIT_WRITE;
            return -1;
        }
        if (n == -2) {
            *wait_events = XR_WAIT_READ;
            return -1;
        }
        return n > 0 ? n : -3;
    }

    XrIOTryResult result = xr_socket_write_try(conn->X, conn->fd, (const char *) data, len);
    if (!result.ready) {
        *wait_events = XR_WAIT_WRITE;
        return -1;
    }
    return result.error == 0 ? result.value : -3;
}

static XrCFuncResult cluster_writer_drive(XrVMRuntime *X, XrWriterContext *ctx, XrValue *result);

static XrCFuncResult cluster_writer_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                             void *context, XrValue *result) {
    (void) resume_value;
    if (status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR)
        return XR_CFUNC_DONE;
    return cluster_writer_drive(X, (XrWriterContext *) context, result);
}

static XrCFuncResult cluster_writer_drive(XrVMRuntime *X, XrWriterContext *ctx, XrValue *result) {
    (void) result;
    XrClusterNode *node = ctx ? ctx->node : NULL;
    if (!node || !atomic_load(&node->writer_running) || node->state != XR_NODE_CONNECTED ||
        !node->conn)
        return XR_CFUNC_DONE;

    for (int operations = 0; operations < 64; operations++) {
        if (!ctx->frames)
            ctx->frames = cluster_outq_pop_all(&node->outq);

        if (!ctx->frames) {
            uint8_t drain[64];
            XrIOTryResult read_result =
                xr_socket_read_try(X, node->outq.notify_pipe[0], (char *) drain, sizeof(drain));
            if (!read_result.ready) {
                return xr_yield_for_io(X, node->outq.notify_pipe[0], XR_WAIT_READ, -1,
                                       cluster_writer_continue, ctx, result);
            }
            if (read_result.error != 0 || read_result.value == 0)
                return XR_CFUNC_DONE;
            continue;
        }

        XrOutFrame *frame = ctx->frames;
        int wait_events = XR_WAIT_WRITE;
        int n = cluster_conn_write_try(node->conn, frame->data + ctx->offset,
                                       (size_t) frame->len - ctx->offset, &wait_events);
        if (n == -1) {
            return xr_yield_for_io(X, node->conn->fd, wait_events, -1, cluster_writer_continue, ctx,
                                   result);
        }
        if (n <= 0) {
            atomic_fetch_add(&node->metrics.send_errors, 1);
            cluster_writer_drop_frames(ctx);
            cluster_node_shutdown(node);
            return XR_CFUNC_DONE;
        }

        ctx->offset += (size_t) n;
        atomic_fetch_add(&node->metrics.bytes_sent, (uint64_t) n);
        if (ctx->offset == frame->len) {
            ctx->frames = frame->next;
            ctx->offset = 0;
            if (frame->owned)
                xr_free(frame->data);
            xr_free(frame);
            atomic_fetch_sub(&node->outq.pending_frames, 1);
            atomic_fetch_add(&node->metrics.frames_sent, 1);
        }
    }

    return xr_yield(X, cluster_writer_continue, ctx);
}

static XrCFuncResult cluster_writer_entry(XrVMRuntime *X, void *context, XrValue *result) {
    return cluster_writer_drive(X, (XrWriterContext *) context, result);
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
    bool finished;
} XrReaderContext;

static void cluster_reader_finish(XrReaderContext *ctx) {
    if (!ctx || ctx->finished)
        return;
    ctx->finished = true;
    if (atomic_load(&ctx->cluster->running))
        cluster_monitor_fire(ctx->cluster, ctx->node->name);
    if (cluster_node_remove(ctx->cluster, ctx->node)) {
        cluster_node_shutdown(ctx->node);
        cluster_node_release(ctx->node);
    } else {
        cluster_node_shutdown(ctx->node);
    }
}

static void cluster_reader_context_destroy(void *context) {
    XrReaderContext *ctx = (XrReaderContext *) context;
    if (!ctx)
        return;
    cluster_reader_finish(ctx);
    xr_free(ctx->payload);
    atomic_store(&ctx->node->reader_running, false);
    cluster_node_release(ctx->node);
    cluster_runtime_release(ctx->cluster);
    xr_free(ctx);
}

int cluster_conn_read_try(XrIOConn *conn, uint8_t *data, size_t len, int *wait_events) {
    if (!conn || conn->fd < 0)
        return -3;
    if (conn->is_tls) {
        int n = xr_tls_conn_read_try(conn->tls, data, len);
        if (n == -1) {
            *wait_events = XR_WAIT_READ;
            return -1;
        }
        if (n == -2) {
            *wait_events = XR_WAIT_WRITE;
            return -1;
        }
        return n >= 0 ? n : -3;
    }

    XrIOTryResult result = xr_socket_read_try(conn->X, conn->fd, (char *) data, len);
    if (!result.ready) {
        *wait_events = XR_WAIT_READ;
        return -1;
    }
    return result.error == 0 ? result.value : -3;
}

static XrCFuncResult cluster_reader_drive(XrVMRuntime *X, XrReaderContext *ctx, XrValue *result);

static XrCFuncResult cluster_reader_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                             void *context, XrValue *result) {
    (void) resume_value;
    XrReaderContext *ctx = (XrReaderContext *) context;
    if (status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR) {
        cluster_reader_finish(ctx);
        return XR_CFUNC_DONE;
    }
    return cluster_reader_drive(X, ctx, result);
}

static XrCFuncResult cluster_reader_drive(XrVMRuntime *X, XrReaderContext *ctx, XrValue *result) {
    if (!ctx || !atomic_load(&ctx->cluster->running) || ctx->node->state != XR_NODE_CONNECTED ||
        !ctx->node->conn) {
        cluster_reader_finish(ctx);
        return XR_CFUNC_DONE;
    }

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
        int n = cluster_conn_read_try(ctx->node->conn, target, remaining, &wait_events);
        if (n == -1) {
            return xr_yield_for_io(X, ctx->node->conn->fd, wait_events, -1, cluster_reader_continue,
                                   ctx, result);
        }
        if (n <= 0) {
            cluster_reader_finish(ctx);
            return XR_CFUNC_DONE;
        }

        atomic_fetch_add(&ctx->node->metrics.bytes_recv, (uint64_t) n);
        if (ctx->header_used < sizeof(ctx->header)) {
            ctx->header_used += (size_t) n;
            if (ctx->header_used < sizeof(ctx->header))
                continue;
            if (cluster_frame_read_header(ctx->header, sizeof(ctx->header), &ctx->frame_type,
                                          &ctx->payload_len) != 0 ||
                ctx->payload_len > XR_FRAME_MAX_PAYLOAD) {
                cluster_reader_finish(ctx);
                return XR_CFUNC_DONE;
            }
            if (ctx->payload_len > 0) {
                ctx->payload = (uint8_t *) xr_malloc(ctx->payload_len);
                if (!ctx->payload) {
                    cluster_reader_finish(ctx);
                    return XR_CFUNC_DONE;
                }
                continue;
            }
        } else {
            ctx->payload_used += (uint32_t) n;
            if (ctx->payload_used < ctx->payload_len)
                continue;
        }

        cluster_process_frame(ctx->cluster, ctx->node, ctx->frame_type, ctx->payload,
                              ctx->payload_len);
        atomic_fetch_add(&ctx->node->metrics.frames_recv, 1);
        xr_free(ctx->payload);
        ctx->payload = NULL;
        ctx->payload_len = 0;
        ctx->payload_used = 0;
        ctx->header_used = 0;
    }

    return xr_yield(X, cluster_reader_continue, ctx);
}

static XrCFuncResult cluster_reader_entry(XrVMRuntime *X, void *context, XrValue *result) {
    return cluster_reader_drive(X, (XrReaderContext *) context, result);
}

bool cluster_node_start_io(struct XrCluster *cluster, XrClusterNode *node) {
    if (!cluster || !node || !cluster->isolate)
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
    writer_ctx->node = node;
    reader_ctx->cluster = cluster;
    reader_ctx->node = node;

    node->isolate = cluster->isolate;
    atomic_store(&node->writer_exited, false);
    cluster_node_retain(node);
    XrCoroutine *writer =
        xr_coro_create_native_yieldable(cluster->isolate, cluster_writer_entry, writer_ctx,
                                        cluster_writer_context_destroy, "cluster_writer");
    if (!writer) {
        atomic_store(&node->writer_running, false);
        atomic_store(&node->writer_exited, true);
        atomic_store(&node->reader_running, false);
        xr_free(reader_ctx);
        return false;
    }

    cluster_node_retain(node);
    cluster_runtime_retain(cluster);
    XrCoroutine *reader =
        xr_coro_create_native_yieldable(cluster->isolate, cluster_reader_entry, reader_ctx,
                                        cluster_reader_context_destroy, "cluster_reader");
    if (!reader) {
        xr_coro_destroy(writer);
        atomic_store(&node->writer_running, false);
        atomic_store(&node->writer_exited, true);
        atomic_store(&node->reader_running, false);
        return false;
    }

    XrRuntime *runtime = (XrRuntime *) cluster->isolate->vm.scheduler;
    if (!runtime) {
        xr_coro_destroy(writer);
        xr_coro_destroy(reader);
        atomic_store(&node->writer_running, false);
        atomic_store(&node->writer_exited, true);
        atomic_store(&node->reader_running, false);
        return false;
    }

    /* Publish the pair as one scheduler batch. Individual local-runnext spawns
     * can strand the second callback behind a long-lived blocking native loop;
     * batch placement assigns both lanes to stealable/background queues. */
    XrCoroutine *io_coros[2] = {writer, reader};
    xr_runtime_spawn_batch(runtime, io_coros, 2);
    return true;
}

/* ========== Slow Consumer Detection ========== */

bool cluster_node_is_slow(XrClusterNode *node) {
    if (!node)
        return false;
    return atomic_load(&node->outq.is_full);
}

/* ========== Heartbeat ========== */

int cluster_node_send_ping(XrClusterNode *node) {
    int64_t now = cluster_now_ms();
    uint8_t frame[32];
    int len = cluster_frame_encode_heartbeat(frame, sizeof(frame), XR_FRAME_HEARTBEAT_PING, now);
    if (len < 0)
        return -1;

    // Enqueue heartbeat via output queue (async)
    int rc = cluster_node_enqueue(node, frame, (uint32_t) len);
    if (rc == 0) {
        node->last_heartbeat_sent = now;
        return 0;
    }
    return -1;
}
