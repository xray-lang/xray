/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_node.h - Cluster node connection management
 *
 * KEY CONCEPT:
 *   Each remote node is represented by XrClusterNode, holding the TCP
 *   connection, heartbeat state, and output queue. Nodes form a linked
 *   list managed by the XrCluster singleton.
 *
 * WHY THIS DESIGN:
 *   - Single TCP connection per node pair (multiplexed)
 *   - Output queue with busy limit for backpressure
 *   - Challenge-response handshake with SHA-256
 */

#ifndef XR_CLUSTER_NODE_H
#define XR_CLUSTER_NODE_H

#include "cluster_proto.h"
#include "../net/io.h"
#include "../../src/coro/xchannel.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ========== Node State ========== */

typedef enum {
    XR_NODE_IDLE,
    XR_NODE_CONNECTING,
    XR_NODE_HANDSHAKING,
    XR_NODE_CONNECTED,
    XR_NODE_CLOSING
} XrNodeState;

/* ========== Output Queue (async write + backpressure) ========== */

/*
 * Queued frame waiting to be written by the dedicated writer coroutine.
 * All send operations enqueue here instead of calling xr_io_write_all directly.
 * This eliminates frame interleaving from concurrent coroutines and enables
 * backpressure via high/low watermarks.
 */
typedef struct XrOutFrame {
    uint8_t *data;
    uint32_t len;
    bool     owned;         // if true, data is owned and freed by queue
    struct XrOutFrame *next;
} XrOutFrame;

typedef struct XrOutputQueue {
    XrOutFrame *head;
    XrOutFrame *tail;
    int64_t     total_bytes;      // total queued bytes
    int         frame_count;      // queued frame count
    int64_t     high_watermark;   // backpressure on  (default 4MB)
    int64_t     low_watermark;    // backpressure off (default 1MB)
    _Atomic(bool) is_full;        // above high watermark
    int         notify_pipe[2];   // pipe for writer wakeup (-1 if unavailable)
    XrSpinlock  lock;
} XrOutputQueue;

/* ========== Node Metrics ========== */

typedef struct XrNodeMetrics {
    _Atomic(uint64_t) frames_sent;
    _Atomic(uint64_t) frames_recv;
    _Atomic(uint64_t) bytes_sent;
    _Atomic(uint64_t) bytes_recv;
    _Atomic(uint64_t) send_errors;
    _Atomic(uint64_t) slow_consumer_events;
    int64_t           last_rtt_ms;   // from heartbeat PONG
} XrNodeMetrics;

/* ========== Phi Accrual Failure Detector ========== */

#define XR_PHI_WINDOW_SIZE 100

typedef struct XrPhiDetector {
    double   intervals[XR_PHI_WINDOW_SIZE]; // heartbeat interval samples
    int      sample_count;
    int      write_idx;          // ring buffer index
    double   mean;               // running mean
    double   variance;           // running variance
    double   sum;                // running sum for O(1) update
    double   sum_sq;             // running sum of squares for O(1) update
    int64_t  last_heartbeat_ts;  // last heartbeat timestamp (ms)
} XrPhiDetector;

/* ========== Pending Request (for safe connection multiplexing) ========== */

/*
 * When a coroutine sends a request (SERVICE_CALL, CHANNEL_RECV_REQ) and
 * needs to wait for a response, it registers a pending request with a
 * temp Channel. The process_node loop reads all frames and dispatches
 * responses to the correct Channel via request_id lookup.
 *
 * This eliminates the socket read conflict between multiple coroutines.
 */
typedef struct XrPendingRequest {
    uint64_t request_id;
    struct XrChannel *response_ch;      // Unbuffered channel, caller blocks on recv
    struct XrPendingRequest *next;
} XrPendingRequest;

#define XR_MAX_PENDING_REQUESTS 256

/* ========== Cluster Node ========== */

typedef struct XrClusterNode {
    char          name[XR_NODE_NAME_MAX + 1];
    char          host[256];
    uint16_t      port;
    XrNodeState   state;
    XrIOConn     *conn;
    int64_t       last_heartbeat_sent;
    int64_t       last_heartbeat_recv;
    uint32_t      flags;
    uint32_t      missed_heartbeats;

    // Pending request table (protected by pending_lock)
    XrPendingRequest *pending_first;
    int               pending_count;
    XrSpinlock        pending_lock;

    // Output queue (async writes via dedicated writer coroutine)
    XrOutputQueue    outq;
    _Atomic(bool)    writer_running;  // writer loop control

    // Metrics
    XrNodeMetrics    metrics;

    // Phi Accrual failure detector
    XrPhiDetector    phi;

    struct XrClusterNode *next;
} XrClusterNode;

/* ========== Forward Declarations ========== */

struct XrCluster;

/* ========== Node Management API ========== */

// Create a new node entry (not yet connected)
XrClusterNode *xr_cluster_node_new(const char *name, const char *host, uint16_t port);

// Free a node and its resources
void xr_cluster_node_free(XrClusterNode *node);

// Connect to a remote node (TCP + handshake)
// Returns 0 on success, -1 on error. Coroutine-friendly (may yield).
int xr_cluster_node_connect(struct XrCluster *cluster, XrClusterNode *node);

// Accept an incoming connection and perform server-side handshake
// Returns a new XrClusterNode on success, NULL on error.
XrClusterNode *xr_cluster_node_accept(struct XrCluster *cluster, XrIOConn *conn);

// Enqueue a pre-built frame for async writing. Thread-safe.
// Returns 0 on success, -1 if queue full (backpressure).
int xr_cluster_node_enqueue(XrClusterNode *node,
                             const uint8_t *data, uint32_t len);

// Send a frame via output queue (encode + enqueue).
// Returns 0 on success, -1 on error/backpressure.
int xr_cluster_node_send_frame(XrClusterNode *node, uint8_t frame_type,
                                const uint8_t *payload, uint32_t payload_len);

// Send a frame synchronously (bypassing output queue).
// Used only during handshake before writer coroutine starts.
int xr_cluster_node_send_frame_sync(XrClusterNode *node, uint8_t frame_type,
                                     const uint8_t *payload, uint32_t payload_len);

// Read a single frame from a node connection
// Returns frame type, writes payload into provided buffer.
// Returns -1 on error/disconnect.
int xr_cluster_node_recv_frame(XrClusterNode *node,
                                uint8_t *frame_type_out,
                                uint8_t *buf, uint32_t buf_size,
                                uint32_t *payload_len_out);

// Send heartbeat ping
int xr_cluster_node_send_ping(XrClusterNode *node);

// Close node connection gracefully
void xr_cluster_node_close(XrClusterNode *node);

/* ========== Writer Coroutine ========== */

// Start the dedicated writer coroutine for a connected node.
// Must be called after handshake completes.
void xr_cluster_node_start_writer(XrClusterNode *node, struct XrayIsolate *X);

// Writer loop function (runs as native coroutine)
void xr_cluster_node_writer_loop(void *arg);

/* ========== Output Queue Helpers ========== */

void xr_outq_init(XrOutputQueue *q);
void xr_outq_destroy(XrOutputQueue *q);
int  xr_outq_push(XrOutputQueue *q, const uint8_t *data, uint32_t len);
int  xr_outq_push_nocopy(XrOutputQueue *q, uint8_t *data, uint32_t len);
XrOutFrame *xr_outq_pop(XrOutputQueue *q);
XrOutFrame *xr_outq_pop_all(XrOutputQueue *q);

/* ========== Phi Accrual Failure Detector ========== */

void   xr_phi_init(XrPhiDetector *det);
void   xr_phi_record_heartbeat(XrPhiDetector *det, int64_t now_ms);
double xr_phi_value(XrPhiDetector *det, int64_t now_ms);

/* ========== Slow Consumer Detection ========== */

bool xr_cluster_node_is_slow(XrClusterNode *node);

/* ========== Pending Request API ========== */

// Register a pending request. Returns the response Channel to block on.
// Caller must recv from the returned Channel to get the response payload.
struct XrChannel *xr_cluster_node_add_pending(
    XrClusterNode *node, uint64_t request_id, struct XrayIsolate *X);

// Find and remove a pending request by request_id.
// Returns the response Channel, or NULL if not found.
struct XrChannel *xr_cluster_node_take_pending(
    XrClusterNode *node, uint64_t request_id);

/* ========== Handshake Helpers ========== */

// Compute SHA-256 proof: SHA256(secret + nonce)
void xr_cluster_compute_proof(const char *secret, const uint8_t *nonce,
                               uint8_t *proof_out);

// Get current monotonic time in milliseconds
int64_t xr_cluster_now_ms(void);

#endif
