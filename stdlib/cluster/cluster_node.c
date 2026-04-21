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

/*
 * xr_socket_read lives in src/coro/xsocket.h. We cannot pull that
 * header here because it drags include/xray_platform.h, which defines
 * `static inline void xr_random_bytes(...)` — conflicts with
 * stdlib/crypto/crypto.h's `int xr_random_bytes(...)` already included
 * above. Forward-declare just the one entry point we need; the real
 * signature is checked at link time against xsocket.c.
 */
extern int xr_socket_read(struct XrayIsolate *X, int fd, char *buf, size_t len);
extern void xr_socket_set_read_timeout(struct XrayIsolate *X, int fd, int timeout_ms);
extern void xr_socket_set_write_timeout(struct XrayIsolate *X, int fd, int timeout_ms);

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

/*
 * proof = HMAC-SHA256(key = secret, data = nonce)
 *
 * The previous v1 scheme used SHA256(secret || nonce) which has two
 * issues compared to a MAC:
 *
 *   1. It is not a keyed MAC — just a prefixed hash. An attacker who
 *      learns a proof / nonce pair cannot forge, but any subtle hash
 *      misuse (e.g. length extension on variant constructions) is a
 *      gun pointed at our own foot.
 *   2. The concatenation is ambiguous: `secret="ab", nonce="cd"` and
 *      `secret="abc", nonce="d"` hash to the same digest. We limit the
 *      secret length so this is not exploitable today, but future
 *      variable-length secrets or nonce formats would re-introduce the
 *      hazard.
 *
 * HMAC-SHA256 eliminates both. xr_hmac_sha256 already owns the 64-byte
 * scratch buffer internally so we no longer stage `secret || nonce` on
 * the stack — the caller's secret never leaves the HMAC inner context.
 */
void xr_cluster_compute_proof(const char *secret, const uint8_t *nonce,
                               uint8_t *proof_out) {
    size_t secret_len = strlen(secret);
    xr_hmac_sha256((const uint8_t *)secret, secret_len,
                   nonce, XR_NONCE_SIZE,
                   proof_out);
}

/*
 * Constant-time buffer comparison for handshake proofs.
 *
 * memcmp short-circuits on the first byte mismatch, leaking partial
 * information via timing. For 32-byte proofs the signal is tiny but we
 * have nothing to gain from being sloppy on the security boundary.
 * Keep this as a translation-unit static since the rest of cluster has
 * no other constant-time needs right now.
 */
static int cluster_proof_equal(const uint8_t *a, const uint8_t *b) {
    uint8_t diff = 0;
    for (size_t i = 0; i < XR_PROOF_SIZE; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
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
    xr_mutex_init(&q->lock);
}

/*
 * Close the writer-facing side of the notify pipe early. Used by
 * xr_cluster_node_free to wake any coroutine yielded on
 * xr_socket_read(notify_pipe[0]) with a clean EOF before we tear
 * down the rest of the node state. Calling this multiple times is
 * safe — it guards on the fd being >= 0.
 *
 * This is split out from xr_outq_destroy specifically because the
 * destroy path wants to close the *read* end only after the writer
 * coroutine has exited; closing the read end while netpoll still
 * has the fd registered is a use-after-close on the PollDesc.
 */
void xr_outq_close_write_end(XrOutputQueue *q) {
    if (q->notify_pipe[1] >= 0) {
        close(q->notify_pipe[1]);
        q->notify_pipe[1] = -1;
    }
}

void xr_outq_destroy(XrOutputQueue *q) {
    XrOutFrame *f = q->head;
    while (f) {
        XrOutFrame *next = f->next;
        if (f->owned) xr_free(f->data);
        xr_free(f);
        f = next;
    }
    q->head = q->tail = NULL;
    q->total_bytes = 0;
    q->frame_count = 0;
    // Close both ends. Write end may already be -1 from an earlier
    // xr_outq_close_write_end during writer teardown; close is
    // idempotent against our own -1 guard.
    if (q->notify_pipe[1] >= 0) close(q->notify_pipe[1]);
    if (q->notify_pipe[0] >= 0) close(q->notify_pipe[0]);
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

    XrOutFrame *f = (XrOutFrame *)xr_malloc(sizeof(XrOutFrame));
    if (!f) return -1;
    f->data = (uint8_t *)xr_malloc(len);
    if (!f->data) { xr_free(f); return -1; }
    memcpy(f->data, data, len);
    f->len = len;
    f->owned = true;
    f->next = NULL;

    xr_mutex_lock(&q->lock);
    outq_enqueue_locked(q, f);
    xr_mutex_unlock(&q->lock);
    outq_notify(q);
    return 0;
}

// Zero-copy push: takes ownership of the data pointer (caller must have malloc'd it)
int xr_outq_push_nocopy(XrOutputQueue *q, uint8_t *data, uint32_t len) {
    if (atomic_load(&q->is_full)) return -1;

    XrOutFrame *f = (XrOutFrame *)xr_malloc(sizeof(XrOutFrame));
    if (!f) return -1;
    f->data = data;
    f->len = len;
    f->owned = true;  // we own it (caller transferred ownership)
    f->next = NULL;

    xr_mutex_lock(&q->lock);
    outq_enqueue_locked(q, f);
    xr_mutex_unlock(&q->lock);
    outq_notify(q);
    return 0;
}

XrOutFrame *xr_outq_pop(XrOutputQueue *q) {
    xr_mutex_lock(&q->lock);
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
    xr_mutex_unlock(&q->lock);
    return f;
}

XrOutFrame *xr_outq_pop_all(XrOutputQueue *q) {
    xr_mutex_lock(&q->lock);
    XrOutFrame *batch = q->head;
    q->head = NULL;
    q->tail = NULL;
    q->total_bytes = 0;
    q->frame_count = 0;
    atomic_store(&q->is_full, false);
    xr_mutex_unlock(&q->lock);
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
    XrClusterNode *node = (XrClusterNode *)xr_calloc(1, sizeof(XrClusterNode));
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
    memset(node->pending_buckets, 0, sizeof(node->pending_buckets));
    node->pending_count = 0;
    xr_mutex_init(&node->pending_lock);
    xr_outq_init(&node->outq);
    atomic_store(&node->writer_running, false);
    atomic_store(&node->writer_exited, false);
    xr_phi_init(&node->phi);
    node->next = NULL;
    return node;
}

void xr_cluster_node_free(XrClusterNode *node) {
    if (!node) return;

    /*
     * Teardown sequence — order matters because the writer coroutine
     * may be yielded inside xr_socket_read(notify_pipe[0]):
     *
     *   1. Clear writer_running so any writer iteration after the
     *      next wake observes the stop signal.
     *   2. Close the write end of notify_pipe (via the dedicated
     *      xr_outq_close_write_end helper). xr_socket_read on the
     *      read end then returns 0 (EOF), wakes the coroutine, the
     *      loop breaks, and the writer sets writer_exited.
     *   3. Close the peer socket. Any in-flight send fails cleanly;
     *      the writer loop's early checks on node->conn bail out.
     *   4. Spin-wait (bounded) for writer_exited. If the writer was
     *      never spawned (e.g. failed start_writer, or a pre-connect
     *      free path from cluster_join / reconnect), writer_exited
     *      stays false but writer_running is already false, so the
     *      wait is a no-op aside from the bounded timeout.
     *   5. xr_outq_destroy — safe now because the writer has stopped
     *      dereferencing notify_pipe[0]; closing pipe[0] here will
     *      not race the netpoll PollDesc for that fd.
     *   6. Free pending requests and the node struct.
     *
     * The bounded wait is 500ms total at 1ms granularity; in the
     * common case the writer exits within the first or second poll.
     * If we truly time out (pathological scheduler starvation) we
     * proceed anyway — the kernel's fd close will eventually wake
     * any stuck reader with EBADF.
     */
    atomic_store(&node->writer_running, false);
    xr_outq_close_write_end(&node->outq);
    xr_cluster_node_close(node);

    if (node->isolate) {
        // Writer was spawned — wait for the exit flag it flips on return.
        for (int i = 0; i < 500 && !atomic_load(&node->writer_exited); i++) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }

    xr_outq_destroy(&node->outq);

    // Free pending requests across every bucket chain. No lock needed
    // here: xr_cluster_node_free runs only after readers and writers
    // have observed the stopped state and exited, so the table is quiesced.
    for (int i = 0; i < XR_PENDING_BUCKETS; i++) {
        XrPendingRequest *pr = node->pending_buckets[i];
        while (pr) {
            XrPendingRequest *next = pr->next;
            if (pr->response_ch) xr_channel_close(pr->response_ch);
            xr_free(pr);
            pr = next;
        }
        node->pending_buckets[i] = NULL;
    }
    xr_free(node);
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
        ? stack_buf : (uint8_t *)xr_malloc(frame_size);
    if (!frame) return -1;

    int wrote = xr_frame_write(frame, frame_type, payload, payload_len);
    if (wrote < 0) {
        if (frame != stack_buf) xr_free(frame);
        return -1;
    }

    int rc = xr_io_write_all(node->conn, frame, (size_t)wrote);
    if (frame != stack_buf) xr_free(frame);
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
        uint8_t *frame = (uint8_t *)xr_malloc(frame_size);
        if (!frame) return -1;
        int wrote = xr_frame_write(frame, frame_type, payload, payload_len);
        if (wrote < 0) { xr_free(frame); return -1; }
        int rc = xr_outq_push_nocopy(&node->outq, frame, (uint32_t)wrote);
        if (rc != 0) { xr_free(frame); return -1; }
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

    /*
     * Bind the worker's thread-local isolate so xr_socket_read (and
     * any downstream xr_io_write the batch path invokes) can resolve a
     * runtime to yield against. node->isolate was recorded in
     * xr_cluster_node_start_writer; NULL here just means we skip the
     * coroutine-friendly drain and fall back to a raw nonblocking read
     * (which returns EAGAIN and spins — acceptable in the degraded
     * path since it implies the node was never properly attached).
     */
    if (node->isolate) xr_io_set_isolate(node->isolate);

    while (atomic_load(&node->writer_running) &&
           node->state == XR_NODE_CONNECTED && node->conn) {

        // Pop all queued frames in one lock acquisition
        XrOutFrame *batch = xr_outq_pop_all(&node->outq);
        if (!batch) {
            /*
             * Drain the notify pipe. xr_socket_read on a non-blocking
             * fd suspends the coroutine via netpoll when no byte is
             * ready — unlike the original raw read(2), this does NOT
             * pin the worker thread. The coroutine wakes either when
             * outq_notify writes a byte (new frame queued) or when
             * the read end is closed during node teardown. A short
             * drain buffer is fine because the wake signal is just a
             * level trigger; if multiple notifies coalesced into one
             * wake we'll pop the batch in the next iteration.
             */
            uint8_t drain[64];
            int pipe_fd = node->outq.notify_pipe[0];
            int n;
            if (node->isolate) {
                n = xr_socket_read(node->isolate, pipe_fd,
                                   (char *)drain, sizeof(drain));
            } else {
                n = (int)read(pipe_fd, drain, sizeof(drain));
            }
            // EOF (pipe[1] closed during teardown) or unrecoverable
            // error: break the loop; writer_running will soon flip.
            if (n == 0) break;
            // n < 0 with EAGAIN/timeout just means "no frames yet" —
            // the caller will observe writer_running/state at the top
            // of the loop. Positive n is normal drain progress.
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
                    if (ptrs[i]->owned) xr_free(ptrs[i]->data);
                    xr_free(ptrs[i]);
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
                if (f->owned) xr_free(f->data);
                xr_free(f);
                f = next;
            }
#endif
        } else {
            // Connection lost — free remaining frames
            XrOutFrame *f = batch;
            while (f) {
                XrOutFrame *next = f->next;
                if (f->owned) xr_free(f->data);
                xr_free(f);
                f = next;
            }
        }
    }

    /*
     * Announce exit to xr_cluster_node_free so its teardown wait on
     * writer_exited can proceed to xr_outq_destroy. Must be the very
     * last statement in this function — once the flag is set, the
     * caller may close notify_pipe[0] at any time and any further
     * dereference of node->outq would be a use-after-close.
     */
    atomic_store(&node->writer_exited, true);
}

void xr_cluster_node_start_writer(XrClusterNode *node, XrayIsolate *X) {
    if (!node || !X) return;
    if (atomic_load(&node->writer_running)) return; // already started

    // Record the isolate so the writer loop can drive xr_socket_read
    // on notify_pipe[0] (coroutine-aware suspend instead of thread block).
    node->isolate = X;
    atomic_store(&node->writer_running, true);
    XrCoroutine *coro = xr_coro_create_native(X, xr_cluster_node_writer_loop,
                                               node, "cluster_writer");
    if (coro) {
        xr_coro_spawn(X, coro);
    } else {
        atomic_store(&node->writer_running, false);
    }
}

/*
 * Frame-processing reader coroutine.
 *
 * Ownership contract:
 *   - The reader does NOT own the XrClusterNode struct. It runs until
 *     the peer disconnects (recv_frame returns -1) or the cluster is
 *     stopped (c->running cleared).
 *   - Cleanup of the node (remove from cluster list, mark dead, fire
 *     monitors, free struct) happens in the heartbeat thread's
 *     xr_cluster_check_heartbeats path when phi/missed-heartbeat
 *     thresholds trip. That thread already owns the removal lock
 *     discipline; duplicating it in the reader would race against
 *     writer_running teardown.
 *
 * We therefore just exit the frame loop on disconnect, letting the
 * heartbeat thread garbage-collect the node. This also means outbound
 * sends that race with the disconnect will eventually notice the
 * writer coroutine failing and back off via the normal reconnect API.
 */
typedef struct XrReaderContext {
    struct XrCluster *cluster;
    XrClusterNode    *node;
} XrReaderContext;

static void cluster_reader_loop(void *arg) {
    XrReaderContext *ctx = (XrReaderContext *)arg;
    if (!ctx) return;

    XrCluster *cluster = ctx->cluster;
    XrClusterNode *node = ctx->node;

    /* Released after we capture cluster/node — the context itself
     * lives only long enough to hand the two pointers over. */
    xr_free(ctx);

    if (cluster && node) {
        xr_cluster_process_node(cluster, node);
    }

    if (node) atomic_store(&node->reader_running, false);
}

void xr_cluster_node_start_reader(struct XrCluster *cluster, XrClusterNode *node) {
    if (!cluster || !node || !cluster->isolate) return;
    if (atomic_load(&node->reader_running)) return;

    XrReaderContext *ctx = (XrReaderContext *)xr_malloc(sizeof(XrReaderContext));
    if (!ctx) return;
    ctx->cluster = cluster;
    ctx->node    = node;

    atomic_store(&node->reader_running, true);
    XrCoroutine *coro = xr_coro_create_native(cluster->isolate, cluster_reader_loop,
                                               ctx, "cluster_reader");
    if (coro) {
        xr_coro_spawn(cluster->isolate, coro);
    } else {
        atomic_store(&node->reader_running, false);
        xr_free(ctx);
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

/*
 * Arm / clear read+write deadlines on a conn for the duration of the
 * handshake. Passing timeout_ms==0 clears both deadlines so subsequent
 * heartbeat / app traffic on the conn runs unbounded (the writer
 * coroutine + reader coroutine install their own shorter deadlines as
 * needed).
 *
 * Safe to call with cluster->isolate == NULL or conn->fd < 0 —
 * xr_socket_set_{read,write}_timeout short-circuit on bad arguments.
 * We guard on tls_enabled here so the timeout is armed on the real
 * underlying socket fd, which is what netpoll tracks for both TLS
 * and plain connections (conn->fd is the socket fd in both cases,
 * per stdlib/net/io.c).
 */
static void cluster_handshake_set_deadline(XrCluster *cluster,
                                           XrIOConn *conn, int timeout_ms) {
    if (!cluster || !cluster->isolate || !conn || conn->fd < 0) return;
    xr_socket_set_read_timeout(cluster->isolate,  conn->fd, timeout_ms);
    xr_socket_set_write_timeout(cluster->isolate, conn->fd, timeout_ms);
}

/* ========== Client-Side Handshake ========== */

int xr_cluster_node_connect(XrCluster *cluster, XrClusterNode *node) {
    if (!cluster || !node) return -1;

    node->state = XR_NODE_CONNECTING;

    // Connect (coroutine-friendly). When TLS is enabled on the cluster we
    // use the per-cluster client context so the node validates the peer
    // against the operator-supplied CA instead of the global system trust
    // store — this also makes pinned-CA + mTLS deployments possible.
    // Falls back to the legacy plain-TCP path when TLS is disabled so no
    // existing deployments are disrupted.
    if (cluster->tls_enabled && cluster->tls_client_ctx) {
        node->conn = xr_io_connect_tls_with_ctx(cluster->tls_client_ctx,
                                                node->host, node->port,
                                                10000);
    } else {
        node->conn = xr_io_connect(node->host, node->port, 10000);
    }
    if (!node->conn) {
        node->state = XR_NODE_IDLE;
        return -1;
    }

    node->state = XR_NODE_HANDSHAKING;

    /*
     * Arm read+write deadlines on the fresh conn so a peer that goes
     * silent mid-handshake cannot pin this coroutine indefinitely.
     * Cleared on every exit path below (success or failure) — the
     * writer / reader coroutines that run after the handshake
     * install their own timeouts as needed.
     */
    cluster_handshake_set_deadline(cluster, node->conn,
                                   XR_CLUSTER_HANDSHAKE_TIMEOUT_MS);

    // Step 1: Send HANDSHAKE_REQ
    XrFrameHandshakeReq req;
    memset(&req, 0, sizeof(req));
    req.version = XR_CLUSTER_HANDSHAKE_VERSION;
    strncpy(req.name, cluster->self_name, XR_NODE_NAME_MAX);
    xr_random_bytes(req.nonce, XR_NONCE_SIZE);
    req.flags = 0x01;

    uint8_t frame_buf[512];
    int flen = xr_frame_encode_handshake_req(frame_buf, sizeof(frame_buf), &req);
    if (flen < 0 || xr_io_write_all(node->conn, frame_buf, (size_t)flen) != flen) {
        cluster_handshake_set_deadline(cluster, node->conn, 0);
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
        cluster_handshake_set_deadline(cluster, node->conn, 0);
        xr_cluster_node_close(node);
        node->state = XR_NODE_IDLE;
        return -1;
    }

    XrFrameHandshakeAck ack;
    if (xr_frame_decode_handshake_ack(recv_buf, payload_len, &ack) != 0 ||
        ack.version != XR_CLUSTER_HANDSHAKE_VERSION) {
        cluster_handshake_set_deadline(cluster, node->conn, 0);
        xr_cluster_node_close(node);
        node->state = XR_NODE_IDLE;
        return -1;
    }

    // Verify proof_b = HMAC-SHA256(secret, nonce_a). Constant-time compare
    // to avoid leaking information about the first mismatching byte.
    uint8_t expected_proof[XR_PROOF_SIZE];
    xr_cluster_compute_proof(cluster->secret, req.nonce, expected_proof);
    if (!cluster_proof_equal(ack.proof, expected_proof)) {
        xr_secure_wipe(expected_proof, sizeof(expected_proof));
        xr_secure_wipe(&req, sizeof(req));
        xr_secure_wipe(&ack, sizeof(ack));
        cluster_handshake_set_deadline(cluster, node->conn, 0);
        xr_cluster_node_close(node);
        node->state = XR_NODE_IDLE;
        return -1;
    }

    // Copy peer name
    strncpy(node->name, ack.name, XR_NODE_NAME_MAX);
    node->name[XR_NODE_NAME_MAX] = '\0';
    node->flags = ack.flags;

    // Step 3: Send HANDSHAKE_DONE with proof_a = HMAC-SHA256(secret, nonce_b)
    XrFrameHandshakeDone done;
    xr_cluster_compute_proof(cluster->secret, ack.nonce, done.proof);

    flen = xr_frame_encode_handshake_done(frame_buf, sizeof(frame_buf), &done);
    if (flen < 0 || xr_io_write_all(node->conn, frame_buf, (size_t)flen) != flen) {
        xr_secure_wipe(&done, sizeof(done));
        xr_secure_wipe(expected_proof, sizeof(expected_proof));
        xr_secure_wipe(&req, sizeof(req));
        xr_secure_wipe(&ack, sizeof(ack));
        cluster_handshake_set_deadline(cluster, node->conn, 0);
        xr_cluster_node_close(node);
        node->state = XR_NODE_IDLE;
        return -1;
    }

    // Handshake complete — erase cryptographic material from stack then
    // clear deadlines so subsequent traffic runs unbounded.
    xr_secure_wipe(expected_proof, sizeof(expected_proof));
    xr_secure_wipe(&req, sizeof(req));
    xr_secure_wipe(&ack, sizeof(ack));
    xr_secure_wipe(&done, sizeof(done));
    cluster_handshake_set_deadline(cluster, node->conn, 0);

    node->state = XR_NODE_CONNECTED;
    node->last_heartbeat_recv = xr_cluster_now_ms();
    return 0;
}

/* ========== Server-Side Handshake ========== */

XrClusterNode *xr_cluster_node_accept(XrCluster *cluster, XrIOConn *conn) {
    if (!cluster || !conn) return NULL;

    /*
     * Arm handshake deadlines immediately — before the first recv —
     * so a peer that TCP-connects and then sits silent cannot pin the
     * accept loop. The accept loop processes one handshake inline
     * per socket, so a single stalled peer would otherwise freeze all
     * further inbound connections. Cleared on every exit path below
     * (success or failure) so downstream traffic (heartbeat / app
     * frames) runs unbounded on the same conn.
     */
    cluster_handshake_set_deadline(cluster, conn,
                                   XR_CLUSTER_HANDSHAKE_TIMEOUT_MS);

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
        cluster_handshake_set_deadline(cluster, conn, 0);
        return NULL;
    }

    XrFrameHandshakeReq req;
    if (xr_frame_decode_handshake_req(recv_buf, payload_len, &req) != 0 ||
        req.version != XR_CLUSTER_HANDSHAKE_VERSION) {
        cluster_handshake_set_deadline(cluster, conn, 0);
        return NULL;
    }

    // Step 2: Send HANDSHAKE_ACK
    XrFrameHandshakeAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.version = XR_CLUSTER_HANDSHAKE_VERSION;
    strncpy(ack.name, cluster->self_name, XR_NODE_NAME_MAX);
    xr_random_bytes(ack.nonce, XR_NONCE_SIZE);
    xr_cluster_compute_proof(cluster->secret, req.nonce, ack.proof);
    ack.flags = 0x01;

    uint8_t frame_buf[512];
    int flen = xr_frame_encode_handshake_ack(frame_buf, sizeof(frame_buf), &ack);
    if (flen < 0 || xr_io_write_all(conn, frame_buf, (size_t)flen) != flen) {
        cluster_handshake_set_deadline(cluster, conn, 0);
        return NULL;
    }

    // Step 3: Receive HANDSHAKE_DONE
    if (xr_cluster_node_recv_frame(&temp, &frame_type, recv_buf, sizeof(recv_buf),
                                    &payload_len) != 0 ||
        frame_type != XR_FRAME_HANDSHAKE_DONE) {
        cluster_handshake_set_deadline(cluster, conn, 0);
        return NULL;
    }

    XrFrameHandshakeDone done;
    if (xr_frame_decode_handshake_done(recv_buf, payload_len, &done) != 0) {
        cluster_handshake_set_deadline(cluster, conn, 0);
        return NULL;
    }

    // Verify proof_a = HMAC-SHA256(secret, nonce_b). Constant-time compare.
    uint8_t expected_proof[XR_PROOF_SIZE];
    xr_cluster_compute_proof(cluster->secret, ack.nonce, expected_proof);
    if (!cluster_proof_equal(done.proof, expected_proof)) {
        xr_secure_wipe(expected_proof, sizeof(expected_proof));
        xr_secure_wipe(&req, sizeof(req));
        xr_secure_wipe(&ack, sizeof(ack));
        xr_secure_wipe(&done, sizeof(done));
        cluster_handshake_set_deadline(cluster, conn, 0);
        return NULL;
    }

    // Create node
    XrClusterNode *node = xr_cluster_node_new(req.name, NULL, 0);
    if (!node) {
        xr_secure_wipe(expected_proof, sizeof(expected_proof));
        xr_secure_wipe(&req, sizeof(req));
        xr_secure_wipe(&ack, sizeof(ack));
        xr_secure_wipe(&done, sizeof(done));
        cluster_handshake_set_deadline(cluster, conn, 0);
        return NULL;
    }

    // Handshake complete — erase cryptographic material from stack then
    // clear deadlines before handing off the conn.
    xr_secure_wipe(expected_proof, sizeof(expected_proof));
    xr_secure_wipe(&req, sizeof(req));
    xr_secure_wipe(&ack, sizeof(ack));
    xr_secure_wipe(&done, sizeof(done));
    cluster_handshake_set_deadline(cluster, conn, 0);

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

    XrPendingRequest *pr = (XrPendingRequest *)xr_calloc(1, sizeof(XrPendingRequest));
    if (!pr) return NULL;

    // Create a buffered(1) channel so sender doesn't block
    XrChannel *ch = xr_channel_new(X, 1);
    if (!ch) {
        xr_free(pr);
        return NULL;
    }

    pr->request_id = request_id;
    pr->response_ch = ch;
    pr->next = NULL;

    /* Bucket selection uses low bits of request_id. Request IDs are
     * monotonic atomic integers (see XrCluster.next_request_id), so
     * the low bits are uniformly distributed even without mixing —
     * no hash function needed. `& (BUCKETS - 1)` requires a power-of-two
     * XR_PENDING_BUCKETS and saves a div on the hot path. */
    uint32_t bucket = (uint32_t)(request_id & (XR_PENDING_BUCKETS - 1));

    xr_mutex_lock(&node->pending_lock);
    if (node->pending_count >= XR_MAX_PENDING_REQUESTS) {
        xr_mutex_unlock(&node->pending_lock);
        xr_free(pr);
        return NULL;
    }
    pr->next = node->pending_buckets[bucket];
    node->pending_buckets[bucket] = pr;
    node->pending_count++;
    xr_mutex_unlock(&node->pending_lock);

    return ch;
}

XrChannel *xr_cluster_node_take_pending(XrClusterNode *node, uint64_t request_id) {
    if (!node) return NULL;

    uint32_t bucket = (uint32_t)(request_id & (XR_PENDING_BUCKETS - 1));

    xr_mutex_lock(&node->pending_lock);
    XrPendingRequest **pp = &node->pending_buckets[bucket];
    while (*pp) {
        if ((*pp)->request_id == request_id) {
            XrPendingRequest *pr = *pp;
            *pp = pr->next;
            node->pending_count--;
            XrChannel *ch = pr->response_ch;
            xr_mutex_unlock(&node->pending_lock);
            xr_free(pr);
            return ch;
        }
        pp = &(*pp)->next;
    }
    xr_mutex_unlock(&node->pending_lock);
    return NULL;
}
