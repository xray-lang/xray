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

#include "cluster_node.h"
#include "cluster.h"
#include "../crypto/crypto.h"
#include "../../src/coro/xchannel.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/vm/xvm_internal.h"
#include "../../src/base/xchecks.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <errno.h>
#ifndef XR_PLATFORM_WINDOWS
#include <sys/uio.h>
#endif

// xr_coro_create_native is not declared in any header
extern struct XrCoroutine *xr_coro_create_native(struct XrayIsolate *X, void (*func)(void*), void *arg,
                                                  const char *name);

/* ========== Time Utility ========== */

int64_t xr_cluster_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ========== Proof Computation ========== */

void xr_cluster_compute_proof(const char *secret, const uint8_t *nonce,
                               uint8_t *proof_out) {
    // proof = SHA256(secret || nonce)
    // secret max 63B + nonce 16B = 79B max, safe for stack
    size_t secret_len = strlen(secret);
    size_t total = secret_len + XR_NONCE_SIZE;
    uint8_t input[80];
    memcpy(input, secret, secret_len);
    memcpy(input + secret_len, nonce, XR_NONCE_SIZE);
    xr_sha256(input, total, proof_out);
}

/* ========== Output Queue ========== */

void xr_outq_init(XrOutputQueue *q) {
    q->head = NULL;
    q->tail = NULL;
    q->total_bytes = 0;
    q->frame_count = 0;
    q->high_watermark = 4 * 1024 * 1024;  // 4MB
    q->low_watermark  = 1 * 1024 * 1024;  // 1MB
    atomic_store(&q->is_full, false);
    // Create pipe for writer wakeup notification
    int rc = pipe(q->notify_pipe);
    XR_DCHECK(rc == 0, "pipe() failed for writer notification");
    if (rc != 0) { q->notify_pipe[0] = q->notify_pipe[1] = -1; }
    // Write end non-blocking: enqueue never blocks if pipe is full
    if (q->notify_pipe[1] >= 0)
        fcntl(q->notify_pipe[1], F_SETFL, O_NONBLOCK);
    xr_spinlock_init(&q->lock);
}

void xr_outq_destroy(XrOutputQueue *q) {
    XrOutFrame *f = q->head;
    while (f) {
        XrOutFrame *next = f->next;
        if (f->owned) free(f->data);
        free(f);
        f = next;
    }
    q->head = q->tail = NULL;
    q->total_bytes = 0;
    q->frame_count = 0;
    // Close notification pipe
    if (q->notify_pipe[0] >= 0) close(q->notify_pipe[0]);
    if (q->notify_pipe[1] >= 0) close(q->notify_pipe[1]);
    q->notify_pipe[0] = q->notify_pipe[1] = -1;
}

// Signal writer coroutine that data is available
static inline void outq_notify(XrOutputQueue *q) {
    if (q->notify_pipe[1] >= 0) {
        uint8_t byte = 1;
        // Ignore EAGAIN (pipe already has data)
        (void)write(q->notify_pipe[1], &byte, 1);
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
    if (q->total_bytes >= q->high_watermark) {
        atomic_store(&q->is_full, true);
    }
}

int xr_outq_push(XrOutputQueue *q, const uint8_t *data, uint32_t len) {
    if (atomic_load(&q->is_full)) return -1;

    XrOutFrame *f = (XrOutFrame *)malloc(sizeof(XrOutFrame));
    if (!f) return -1;
    f->data = (uint8_t *)malloc(len);
    if (!f->data) { free(f); return -1; }
    memcpy(f->data, data, len);
    f->len = len;
    f->owned = true;
    f->next = NULL;

    xr_spinlock_lock(&q->lock);
    outq_enqueue_locked(q, f);
    xr_spinlock_unlock(&q->lock);
    outq_notify(q);
    return 0;
}

// Zero-copy push: takes ownership of the data pointer (caller must have malloc'd it)
int xr_outq_push_nocopy(XrOutputQueue *q, uint8_t *data, uint32_t len) {
    if (atomic_load(&q->is_full)) return -1;

    XrOutFrame *f = (XrOutFrame *)malloc(sizeof(XrOutFrame));
    if (!f) return -1;
    f->data = data;
    f->len = len;
    f->owned = true;  // we own it (caller transferred ownership)
    f->next = NULL;

    xr_spinlock_lock(&q->lock);
    outq_enqueue_locked(q, f);
    xr_spinlock_unlock(&q->lock);
    outq_notify(q);
    return 0;
}

XrOutFrame *xr_outq_pop(XrOutputQueue *q) {
    xr_spinlock_lock(&q->lock);
    XrOutFrame *f = q->head;
    if (f) {
        q->head = f->next;
        if (!q->head) q->tail = NULL;
        q->total_bytes -= f->len;
        q->frame_count--;
        if (q->total_bytes <= q->low_watermark) {
            atomic_store(&q->is_full, false);
        }
    }
    xr_spinlock_unlock(&q->lock);
    return f;
}

XrOutFrame *xr_outq_pop_all(XrOutputQueue *q) {
    xr_spinlock_lock(&q->lock);
    XrOutFrame *batch = q->head;
    q->head = NULL;
    q->tail = NULL;
    q->total_bytes = 0;
    q->frame_count = 0;
    atomic_store(&q->is_full, false);
    xr_spinlock_unlock(&q->lock);
    return batch;
}

/* ========== Phi Accrual Failure Detector ========== */

void xr_phi_init(XrPhiDetector *det) {
    memset(det, 0, sizeof(XrPhiDetector));
    det->mean = 5000.0;      // assume 5s heartbeat interval initially
    det->variance = 100.0;
    det->sum = 0.0;
    det->sum_sq = 0.0;
}

void xr_phi_record_heartbeat(XrPhiDetector *det, int64_t now_ms) {
    if (det->last_heartbeat_ts > 0) {
        double interval = (double)(now_ms - det->last_heartbeat_ts);

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
        if (det->variance < 1.0) det->variance = 1.0; // avoid zero from fp drift
    }
    det->last_heartbeat_ts = now_ms;
}

double xr_phi_value(XrPhiDetector *det, int64_t now_ms) {
    if (det->last_heartbeat_ts == 0 || det->sample_count < 2)
        return 0.0;

    double elapsed = (double)(now_ms - det->last_heartbeat_ts);
    double stddev = sqrt(det->variance);
    if (stddev < 1.0) stddev = 1.0;

    // phi = -log10(1 - CDF(elapsed))
    // Using normal distribution CDF approximation
    double y = (elapsed - det->mean) / stddev;
    // Logistic approximation to normal CDF: 1 / (1 + exp(-1.7155 * y))
    double p_later = 1.0 / (1.0 + exp(1.7155 * y));
    if (p_later < 1e-15) p_later = 1e-15;
    return -log10(p_later);
}

/* ========== Node Lifecycle ========== */

XrClusterNode *xr_cluster_node_new(const char *name, const char *host, uint16_t port) {
    XrClusterNode *node = (XrClusterNode *)calloc(1, sizeof(XrClusterNode));
    if (!node) return NULL;

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
    node->pending_first = NULL;
    node->pending_count = 0;
    xr_spinlock_init(&node->pending_lock);
    xr_outq_init(&node->outq);
    atomic_store(&node->writer_running, false);
    xr_phi_init(&node->phi);
    node->next = NULL;
    return node;
}

void xr_cluster_node_free(XrClusterNode *node) {
    if (!node) return;

    // Stop writer coroutine first
    atomic_store(&node->writer_running, false);

    xr_cluster_node_close(node);

    // Free output queue
    xr_outq_destroy(&node->outq);

    // Free pending requests
    XrPendingRequest *pr = node->pending_first;
    while (pr) {
        XrPendingRequest *next = pr->next;
        if (pr->response_ch) xr_channel_close(pr->response_ch);
        free(pr);
        pr = next;
    }
    free(node);
}

void xr_cluster_node_close(XrClusterNode *node) {
    if (!node) return;
    if (node->conn) {
        xr_io_close(node->conn);
        node->conn = NULL;
    }
    node->state = XR_NODE_CLOSING;
}

/* ========== Frame Send/Recv ========== */

// Synchronous send — used only during handshake (before writer coroutine starts)
int xr_cluster_node_send_frame_sync(XrClusterNode *node, uint8_t frame_type,
                                     const uint8_t *payload, uint32_t payload_len) {
    if (!node || !node->conn || node->state == XR_NODE_CLOSING) return -1;

    uint32_t frame_size = 4 + 1 + payload_len;
    uint8_t stack_buf[4096];
    uint8_t *frame = (frame_size <= sizeof(stack_buf))
        ? stack_buf : (uint8_t *)malloc(frame_size);
    if (!frame) return -1;

    int wrote = xr_frame_write(frame, frame_type, payload, payload_len);
    if (wrote < 0) {
        if (frame != stack_buf) free(frame);
        return -1;
    }

    int rc = xr_io_write_all(node->conn, frame, (size_t)wrote);
    if (frame != stack_buf) free(frame);
    if (rc == wrote) {
        atomic_fetch_add(&node->metrics.frames_sent, 1);
        atomic_fetch_add(&node->metrics.bytes_sent, (uint64_t)wrote);
        return 0;
    }
    atomic_fetch_add(&node->metrics.send_errors, 1);
    return -1;
}

// Enqueue pre-built frame data for async writing
int xr_cluster_node_enqueue(XrClusterNode *node,
                             const uint8_t *data, uint32_t len) {
    if (!node || node->state == XR_NODE_CLOSING) return -1;
    return xr_outq_push(&node->outq, data, len);
}

// Async send — encode frame and enqueue for writer coroutine
// Uses zero-copy for large frames (>4KB) to avoid extra memcpy
int xr_cluster_node_send_frame(XrClusterNode *node, uint8_t frame_type,
                                const uint8_t *payload, uint32_t payload_len) {
    if (!node || !node->conn || node->state == XR_NODE_CLOSING) return -1;

    uint32_t frame_size = 4 + 1 + payload_len;

    if (frame_size <= 4096) {
        // Small frame: encode to stack, copy into queue
        uint8_t stack_buf[4096];
        int wrote = xr_frame_write(stack_buf, frame_type, payload, payload_len);
        if (wrote < 0) return -1;
        return xr_cluster_node_enqueue(node, stack_buf, (uint32_t)wrote);
    } else {
        // Large frame: encode to heap, transfer ownership (zero-copy)
        uint8_t *frame = (uint8_t *)malloc(frame_size);
        if (!frame) return -1;
        int wrote = xr_frame_write(frame, frame_type, payload, payload_len);
        if (wrote < 0) { free(frame); return -1; }
        int rc = xr_outq_push_nocopy(&node->outq, frame, (uint32_t)wrote);
        if (rc != 0) { free(frame); return -1; }
        return 0;
    }
}

int xr_cluster_node_recv_frame(XrClusterNode *node,
                                uint8_t *frame_type_out,
                                uint8_t *buf, uint32_t buf_size,
                                uint32_t *payload_len_out) {
    if (!node || !node->conn) return -1;

    // Read 4-byte length header
    uint8_t header[XR_FRAME_HEADER_SIZE + 1]; // 4B length + 1B type
    int n = xr_io_read_full(node->conn, header, 5);
    if (n != 5) return -1;

    uint32_t total_payload;
    if (xr_frame_read_header(header, 5, frame_type_out, &total_payload) != 0)
        return -1;

    // total_payload is payload size (after type byte)
    if (total_payload > buf_size) return -1;
    *payload_len_out = total_payload;

    if (total_payload > 0) {
        n = xr_io_read_full(node->conn, buf, total_payload);
        if (n != (int)total_payload) return -1;
    }

    return 0;
}

/* ========== Writer Coroutine ========== */

#define XR_WRITER_MAX_IOV 64

void xr_cluster_node_writer_loop(void *arg) {
    XrClusterNode *node = (XrClusterNode *)arg;
    if (!node) return;

    while (atomic_load(&node->writer_running) &&
           node->state == XR_NODE_CONNECTED && node->conn) {

        // Pop all queued frames in one lock acquisition
        XrOutFrame *batch = xr_outq_pop_all(&node->outq);
        if (!batch) {
            // Blocking read on pipe — wakes when data enqueued
            uint8_t drain[64];
            (void)read(node->outq.notify_pipe[0], drain, sizeof(drain));
            continue;
        }

        // Batch frames into iovec for writev (reduces syscalls)
        if (node->conn && node->state == XR_NODE_CONNECTED) {
#ifndef XR_PLATFORM_WINDOWS
            struct iovec iov[XR_WRITER_MAX_IOV];
            XrOutFrame *ptrs[XR_WRITER_MAX_IOV];
            XrOutFrame *f = batch;

            while (f) {
                int count = 0;
                size_t total_bytes = 0;

                // Collect up to XR_WRITER_MAX_IOV frames
                while (f && count < XR_WRITER_MAX_IOV) {
                    iov[count].iov_base = f->data;
                    iov[count].iov_len = f->len;
                    ptrs[count] = f;
                    total_bytes += f->len;
                    count++;
                    f = f->next;
                }

                // writev all at once
                int rc = xr_io_writev(node->conn, iov, count);
                if (rc == (int)total_bytes) {
                    atomic_fetch_add(&node->metrics.frames_sent, (uint64_t)count);
                    atomic_fetch_add(&node->metrics.bytes_sent, total_bytes);
                } else {
                    atomic_fetch_add(&node->metrics.send_errors, 1);
                }

                // Free the batch
                for (int i = 0; i < count; i++) {
                    if (ptrs[i]->owned) free(ptrs[i]->data);
                    free(ptrs[i]);
                }
            }
#else
            // Windows fallback: sequential writes
            XrOutFrame *f = batch;
            while (f) {
                XrOutFrame *next = f->next;
                int rc = xr_io_write_all(node->conn, f->data, f->len);
                if (rc == (int)f->len) {
                    atomic_fetch_add(&node->metrics.frames_sent, 1);
                    atomic_fetch_add(&node->metrics.bytes_sent, (uint64_t)f->len);
                } else {
                    atomic_fetch_add(&node->metrics.send_errors, 1);
                }
                if (f->owned) free(f->data);
                free(f);
                f = next;
            }
#endif
        } else {
            // Connection lost — free remaining frames
            XrOutFrame *f = batch;
            while (f) {
                XrOutFrame *next = f->next;
                if (f->owned) free(f->data);
                free(f);
                f = next;
            }
        }
    }
}

void xr_cluster_node_start_writer(XrClusterNode *node, XrayIsolate *X) {
    if (!node || !X) return;
    if (atomic_load(&node->writer_running)) return; // already started

    atomic_store(&node->writer_running, true);
    XrCoroutine *coro = xr_coro_create_native(X, xr_cluster_node_writer_loop,
                                               node, "cluster_writer");
    if (coro) {
        xr_coro_spawn(X, coro);
    } else {
        atomic_store(&node->writer_running, false);
    }
}

/* ========== Slow Consumer Detection ========== */

bool xr_cluster_node_is_slow(XrClusterNode *node) {
    if (!node) return false;
    return atomic_load(&node->outq.is_full);
}

/* ========== Heartbeat ========== */

int xr_cluster_node_send_ping(XrClusterNode *node) {
    int64_t now = xr_cluster_now_ms();
    uint8_t frame[32];
    int len = xr_frame_encode_heartbeat(frame, sizeof(frame),
                                         XR_FRAME_HEARTBEAT_PING, now);
    if (len < 0) return -1;

    // Enqueue heartbeat via output queue (async)
    int rc = xr_cluster_node_enqueue(node, frame, (uint32_t)len);
    if (rc == 0) {
        node->last_heartbeat_sent = now;
        return 0;
    }
    return -1;
}

/* ========== Client-Side Handshake ========== */

int xr_cluster_node_connect(XrCluster *cluster, XrClusterNode *node) {
    if (!cluster || !node) return -1;

    node->state = XR_NODE_CONNECTING;

    // TCP connect (coroutine-friendly)
    node->conn = xr_io_connect(node->host, node->port, 10000);
    if (!node->conn) {
        node->state = XR_NODE_IDLE;
        return -1;
    }

    node->state = XR_NODE_HANDSHAKING;

    // Step 1: Send HANDSHAKE_REQ
    XrFrameHandshakeReq req;
    memset(&req, 0, sizeof(req));
    req.version = 1;
    strncpy(req.name, cluster->self_name, XR_NODE_NAME_MAX);
    xr_random_bytes(req.nonce, XR_NONCE_SIZE);
    req.flags = 0x01;

    uint8_t frame_buf[512];
    int flen = xr_frame_encode_handshake_req(frame_buf, sizeof(frame_buf), &req);
    if (flen < 0 || xr_io_write_all(node->conn, frame_buf, (size_t)flen) != flen) {
        xr_cluster_node_close(node);
        node->state = XR_NODE_IDLE;
        return -1;
    }

    // Step 2: Receive HANDSHAKE_ACK
    uint8_t recv_buf[512];
    uint8_t frame_type;
    uint32_t payload_len;
    if (xr_cluster_node_recv_frame(node, &frame_type, recv_buf, sizeof(recv_buf),
                                    &payload_len) != 0 ||
        frame_type != XR_FRAME_HANDSHAKE_ACK) {
        xr_cluster_node_close(node);
        node->state = XR_NODE_IDLE;
        return -1;
    }

    XrFrameHandshakeAck ack;
    if (xr_frame_decode_handshake_ack(recv_buf, payload_len, &ack) != 0 ||
        ack.version != 1) {
        xr_cluster_node_close(node);
        node->state = XR_NODE_IDLE;
        return -1;
    }

    // Verify proof_b = SHA256(secret + nonce_a)
    uint8_t expected_proof[XR_PROOF_SIZE];
    xr_cluster_compute_proof(cluster->secret, req.nonce, expected_proof);
    if (memcmp(ack.proof, expected_proof, XR_PROOF_SIZE) != 0) {
        xr_cluster_node_close(node);
        node->state = XR_NODE_IDLE;
        return -1;
    }

    // Copy peer name
    strncpy(node->name, ack.name, XR_NODE_NAME_MAX);
    node->name[XR_NODE_NAME_MAX] = '\0';
    node->flags = ack.flags;

    // Step 3: Send HANDSHAKE_DONE with proof_a = SHA256(secret + nonce_b)
    XrFrameHandshakeDone done;
    xr_cluster_compute_proof(cluster->secret, ack.nonce, done.proof);

    flen = xr_frame_encode_handshake_done(frame_buf, sizeof(frame_buf), &done);
    if (flen < 0 || xr_io_write_all(node->conn, frame_buf, (size_t)flen) != flen) {
        xr_cluster_node_close(node);
        node->state = XR_NODE_IDLE;
        return -1;
    }

    // Handshake complete
    node->state = XR_NODE_CONNECTED;
    node->last_heartbeat_recv = xr_cluster_now_ms();
    return 0;
}

/* ========== Server-Side Handshake ========== */

XrClusterNode *xr_cluster_node_accept(XrCluster *cluster, XrIOConn *conn) {
    if (!cluster || !conn) return NULL;

    // Step 1: Receive HANDSHAKE_REQ
    uint8_t recv_buf[512];
    uint8_t frame_type;
    uint32_t payload_len;

    // Temporarily wrap conn for recv
    XrClusterNode temp;
    memset(&temp, 0, sizeof(temp));
    temp.conn = conn;
    temp.state = XR_NODE_HANDSHAKING;

    if (xr_cluster_node_recv_frame(&temp, &frame_type, recv_buf, sizeof(recv_buf),
                                    &payload_len) != 0 ||
        frame_type != XR_FRAME_HANDSHAKE_REQ) {
        return NULL;
    }

    XrFrameHandshakeReq req;
    if (xr_frame_decode_handshake_req(recv_buf, payload_len, &req) != 0 ||
        req.version != 1) {
        return NULL;
    }

    // Step 2: Send HANDSHAKE_ACK
    XrFrameHandshakeAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.version = 1;
    strncpy(ack.name, cluster->self_name, XR_NODE_NAME_MAX);
    xr_random_bytes(ack.nonce, XR_NONCE_SIZE);
    xr_cluster_compute_proof(cluster->secret, req.nonce, ack.proof);
    ack.flags = 0x01;

    uint8_t frame_buf[512];
    int flen = xr_frame_encode_handshake_ack(frame_buf, sizeof(frame_buf), &ack);
    if (flen < 0 || xr_io_write_all(conn, frame_buf, (size_t)flen) != flen) {
        return NULL;
    }

    // Step 3: Receive HANDSHAKE_DONE
    if (xr_cluster_node_recv_frame(&temp, &frame_type, recv_buf, sizeof(recv_buf),
                                    &payload_len) != 0 ||
        frame_type != XR_FRAME_HANDSHAKE_DONE) {
        return NULL;
    }

    XrFrameHandshakeDone done;
    if (xr_frame_decode_handshake_done(recv_buf, payload_len, &done) != 0) {
        return NULL;
    }

    // Verify proof_a = SHA256(secret + nonce_b)
    uint8_t expected_proof[XR_PROOF_SIZE];
    xr_cluster_compute_proof(cluster->secret, ack.nonce, expected_proof);
    if (memcmp(done.proof, expected_proof, XR_PROOF_SIZE) != 0) {
        return NULL;
    }

    // Create node
    XrClusterNode *node = xr_cluster_node_new(req.name, NULL, 0);
    if (!node) return NULL;

    node->conn = conn;
    node->state = XR_NODE_CONNECTED;
    node->flags = req.flags;
    node->last_heartbeat_recv = xr_cluster_now_ms();

    return node;
}

/* ========== Pending Request API ========== */

XrChannel *xr_cluster_node_add_pending(XrClusterNode *node, uint64_t request_id,
                                        XrayIsolate *X) {
    XR_DCHECK(node != NULL, "node must not be NULL");
    XR_DCHECK(X != NULL, "isolate must not be NULL");
    if (!node || !X) return NULL;

    XrPendingRequest *pr = (XrPendingRequest *)calloc(1, sizeof(XrPendingRequest));
    if (!pr) return NULL;

    // Create a buffered(1) channel so sender doesn't block
    XrChannel *ch = xr_channel_new(X, 1);
    if (!ch) {
        free(pr);
        return NULL;
    }

    pr->request_id = request_id;
    pr->response_ch = ch;
    pr->next = NULL;

    xr_spinlock_lock(&node->pending_lock);
    if (node->pending_count >= XR_MAX_PENDING_REQUESTS) {
        xr_spinlock_unlock(&node->pending_lock);
        free(pr);
        return NULL;
    }
    pr->next = node->pending_first;
    node->pending_first = pr;
    node->pending_count++;
    xr_spinlock_unlock(&node->pending_lock);

    return ch;
}

XrChannel *xr_cluster_node_take_pending(XrClusterNode *node, uint64_t request_id) {
    if (!node) return NULL;

    xr_spinlock_lock(&node->pending_lock);
    XrPendingRequest **pp = &node->pending_first;
    while (*pp) {
        if ((*pp)->request_id == request_id) {
            XrPendingRequest *pr = *pp;
            *pp = pr->next;
            node->pending_count--;
            XrChannel *ch = pr->response_ch;
            xr_spinlock_unlock(&node->pending_lock);
            free(pr);
            return ch;
        }
        pp = &(*pp)->next;
    }
    xr_spinlock_unlock(&node->pending_lock);
    return NULL;
}
