/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_internal.h - Private distributed cluster runtime/data-plane API
 *
 * KEY CONCEPT:
 *   Decentralized cluster transport for opaque service envelopes.
 *   Each node is identified by a unique name. Nodes connect via TCP
 *   with challenge-response handshake (SHA-256).
 *
 * WHY THIS DESIGN:
 *   - Transport frames never encode or inspect Xray values
 *   - Xray owns contract codecs and RPC lifecycle above this boundary
 *   - at-most-once delivery: consistent with Go/Erlang/NATS semantics
 */

#ifndef XR_CLUSTER_INTERNAL_H
#define XR_CLUSTER_INTERNAL_H

#include "../../src/base/xmalloc.h"
#include "../../src/coro/xchannel.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/io/xnet_transport.h"
#include "../../src/io/xtls_provider.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <limits.h>
#include "../../src/os/os_thread.h"

#ifdef _WIN32
#include <fcntl.h>
#if defined(_MSC_VER)
#include <corecrt_io.h>
#else
#include <io.h>
#endif

static inline int xr_cluster_pipe(int fds[2]) {
    return _pipe(fds, 4096, _O_BINARY);
}

static inline int xr_cluster_read(int fd, void *buf, size_t len) {
    size_t capped = len > (size_t) UINT_MAX ? (size_t) UINT_MAX : len;
    return _read(fd, buf, (unsigned int) capped);
}

static inline int xr_cluster_write(int fd, const void *buf, size_t len) {
    size_t capped = len > (size_t) UINT_MAX ? (size_t) UINT_MAX : len;
    return _write(fd, buf, (unsigned int) capped);
}

static inline int xr_cluster_fcntl_noop(int fd, int cmd, int flags) {
    (void) fd;
    (void) cmd;
    (void) flags;
    return 0;
}

#ifndef F_SETFL
#define F_SETFL 0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif

#define pipe(fds) xr_cluster_pipe((fds))
#define read(fd, buf, len) xr_cluster_read((fd), (buf), (len))
#define write(fd, buf, len) xr_cluster_write((fd), (buf), (len))
#define close(fd) _close((fd))
#define fcntl(fd, cmd, flags) xr_cluster_fcntl_noop((fd), (cmd), (flags))
#else
#include <unistd.h>
#include <fcntl.h>
#endif

/* ========== Forward Declarations ========== */

struct XrVMRuntime;
struct XrChannel;
typedef struct XrCluster XrCluster;
typedef struct XrClusterDiscovery XrClusterDiscovery;

typedef enum XrClusterDelivery {
    XR_CLUSTER_DELIVERY_ACCEPTED = 0,
    XR_CLUSTER_DELIVERY_INVALID_TOPIC,
    XR_CLUSTER_DELIVERY_INVALID_ENVELOPE,
    XR_CLUSTER_DELIVERY_UNAVAILABLE,
    XR_CLUSTER_DELIVERY_OVERLOADED,
    XR_CLUSTER_DELIVERY_DISCONNECTED,
} XrClusterDelivery;

/* ========== Cluster Wire Protocol ========== */

typedef enum {
    XR_FRAME_HANDSHAKE_REQ = 0x01,
    XR_FRAME_HANDSHAKE_ACK = 0x02,
    XR_FRAME_HANDSHAKE_DONE = 0x03,
    XR_FRAME_HANDSHAKE_ERR = 0x04,
    XR_FRAME_HEARTBEAT_PING = 0x05,
    XR_FRAME_HEARTBEAT_PONG = 0x06,
    XR_FRAME_TRANSPORT_ENVELOPE = 0x07,
    XR_FRAME_CORO_MONITOR = 0x08,
    XR_FRAME_CORO_DEMONITOR = 0x09,
    XR_FRAME_CORO_EXIT = 0x0A,
} XrFrameType;

#define XR_FRAME_HEADER_SIZE 4
#define XR_FRAME_MAX_PAYLOAD (16 * 1024 * 1024)
#define XR_CLUSTER_ENVELOPE_HEADER_SIZE 64
#define XR_NONCE_SIZE 16
#define XR_PROOF_SIZE 32
#define XR_CLUSTER_HANDSHAKE_VERSION 6
#define XR_CLUSTER_HANDSHAKE_TIMEOUT_MS 5000
#define XR_TOPIC_DEFAULT_HOP_LIMIT 3
#define XR_NODE_NAME_MAX 63
#define XR_CORO_NAME_MAX 127

typedef struct {
    uint8_t version;
    char name[XR_NODE_NAME_MAX + 1];
    uint8_t nonce[XR_NONCE_SIZE];
    uint32_t flags;
} XrFrameHandshakeReq;

typedef struct {
    uint8_t version;
    char name[XR_NODE_NAME_MAX + 1];
    uint8_t nonce[XR_NONCE_SIZE];
    uint8_t proof[XR_PROOF_SIZE];
    uint32_t flags;
} XrFrameHandshakeAck;

typedef struct {
    uint8_t proof[XR_PROOF_SIZE];
} XrFrameHandshakeDone;

typedef struct {
    int64_t timestamp;
} XrFrameHeartbeat;

int cluster_frame_write(uint8_t *buf, uint8_t frame_type, const uint8_t *payload,
                        uint32_t payload_len);
int cluster_frame_write_transport(uint8_t *buf, size_t buf_size, uint8_t hop_limit,
                                  const char *topic, uint8_t topic_len, const uint8_t *envelope,
                                  uint32_t envelope_len);
int cluster_frame_encode_handshake_req(uint8_t *buf, size_t buf_size,
                                       const XrFrameHandshakeReq *req);
int cluster_frame_encode_handshake_ack(uint8_t *buf, size_t buf_size,
                                       const XrFrameHandshakeAck *ack);
int cluster_frame_encode_handshake_done(uint8_t *buf, size_t buf_size,
                                        const XrFrameHandshakeDone *done);
int cluster_frame_encode_heartbeat(uint8_t *buf, size_t buf_size, uint8_t type, int64_t timestamp);
int cluster_frame_read_header(const uint8_t *data, size_t data_len, uint8_t *frame_type,
                              uint32_t *payload_len);
int cluster_frame_decode_handshake_req(const uint8_t *payload, uint32_t len,
                                       XrFrameHandshakeReq *req);
int cluster_frame_decode_handshake_ack(const uint8_t *payload, uint32_t len,
                                       XrFrameHandshakeAck *ack);
int cluster_frame_decode_handshake_done(const uint8_t *payload, uint32_t len,
                                        XrFrameHandshakeDone *done);
int cluster_frame_decode_heartbeat(const uint8_t *payload, uint32_t len, int64_t *timestamp);
int cluster_frame_encode_coro_monitor(uint8_t *buf, size_t buf_size, uint8_t frame_type,
                                      const char *coro_name);
int cluster_frame_encode_coro_exit(uint8_t *buf, size_t buf_size, const char *coro_name,
                                   const char *reason);
int cluster_frame_decode_coro_monitor(const uint8_t *payload, uint32_t len, char *coro_name,
                                      size_t name_size);
int cluster_frame_decode_coro_exit(const uint8_t *payload, uint32_t len, char *coro_name,
                                   size_t name_size, char *reason, size_t reason_size);

typedef struct XrFrameBuf {
    uint8_t stack[4096];
    uint8_t *data;
    bool heap;
} XrFrameBuf;

static inline void cluster_frame_buf_init(XrFrameBuf *fb, size_t needed) {
    if (needed <= sizeof(fb->stack)) {
        fb->data = fb->stack;
        fb->heap = false;
    } else {
        fb->data = (uint8_t *) xr_malloc(needed);
        fb->heap = true;
    }
}

static inline void cluster_frame_buf_free(XrFrameBuf *fb) {
    if (fb->heap && fb->data) {
        xr_free(fb->data);
        fb->data = NULL;
    }
}

/* ========== Node State ========== */

typedef enum {
    XR_NODE_IDLE,
    XR_NODE_CONNECTING,
    XR_NODE_HANDSHAKING,
    XR_NODE_CONNECTED,
    XR_NODE_CLOSING
} XrNodeState;

typedef struct XrOutFrame {
    uint8_t *data;
    uint32_t len;
    bool owned;
    struct XrOutFrame *next;
} XrOutFrame;

typedef struct XrOutputQueue {
    XrOutFrame *head;
    XrOutFrame *tail;
    int64_t total_bytes;
    int frame_count;
    int64_t high_watermark;
    int64_t low_watermark;
    _Atomic(bool) is_full;
    _Atomic(uint64_t) pending_frames;
    int notify_pipe[2];
    XrAdaptiveMutex lock;
} XrOutputQueue;

typedef struct XrNodeMetrics {
    _Atomic(uint64_t) frames_sent;
    _Atomic(uint64_t) frames_recv;
    _Atomic(uint64_t) bytes_sent;
    _Atomic(uint64_t) bytes_recv;
    _Atomic(uint64_t) send_errors;
    _Atomic(uint64_t) slow_consumer_events;
    int64_t last_rtt_ms;
} XrNodeMetrics;

#define XR_PHI_WINDOW_SIZE 100

typedef struct XrPhiDetector {
    double intervals[XR_PHI_WINDOW_SIZE];
    int sample_count;
    int write_idx;
    double mean;
    double variance;
    double sum;
    double sum_sq;
    int64_t last_heartbeat_ts;
} XrPhiDetector;

typedef struct XrClusterNode {
    _Atomic(uint32_t) ref_count;
    _Atomic(bool) shutdown_started;
    char name[XR_NODE_NAME_MAX + 1];
    char host[256];
    uint16_t port;
    XrNodeState state;
    XrIOConn *conn;
    int64_t last_heartbeat_sent;
    int64_t last_heartbeat_recv;
    uint32_t flags;
    uint32_t missed_heartbeats;

    struct XrVMRuntime *isolate;

    XrOutputQueue outq;
    _Atomic(bool) writer_running;
    _Atomic(bool) writer_exited;
    _Atomic(bool) reader_running;

    XrNodeMetrics metrics;
    XrPhiDetector phi;

    struct XrClusterNode *next;
} XrClusterNode;

XrClusterNode *cluster_node_new(const char *name, const char *host, uint16_t port);
void cluster_node_retain(XrClusterNode *node);
void cluster_node_shutdown(XrClusterNode *node);
void cluster_node_release(XrClusterNode *node);
int cluster_node_enqueue(XrClusterNode *node, const uint8_t *data, uint32_t len);
int cluster_node_send_frame(XrClusterNode *node, uint8_t frame_type, const uint8_t *payload,
                            uint32_t payload_len);
int cluster_node_send_transport_frame(XrClusterNode *node, uint8_t hop_limit, const char *topic,
                                      uint8_t topic_len, const uint8_t *envelope,
                                      uint32_t envelope_len);
int cluster_conn_read_try(XrIOConn *conn, uint8_t *data, size_t len, int *wait_events);
int cluster_conn_write_try(XrIOConn *conn, const uint8_t *data, size_t len, int *wait_events);
void cluster_compute_proof(const char *secret, const uint8_t *nonce, uint8_t *proof_out);
bool cluster_proof_equal(const uint8_t *a, const uint8_t *b);
int cluster_node_send_ping(XrClusterNode *node);
bool cluster_node_start_io(struct XrCluster *cluster, XrClusterNode *node);
void cluster_phi_init(XrPhiDetector *det);
void cluster_phi_record_heartbeat(XrPhiDetector *det, int64_t now_ms);
double cluster_phi_value(XrPhiDetector *det, int64_t now_ms);
bool cluster_node_is_slow(XrClusterNode *node);
int64_t cluster_now_ms(void);

/* ========== Forward Declarations ========== */

typedef struct XrRemoteCoroMonitor XrRemoteCoroMonitor;

/* ========== Cluster State ========== */

#define XR_TOPIC_PATTERN_MAX 127

struct XrTopicTrieNode;  // forward decl — definition in cluster_topic.c

typedef struct XrCluster {
    _Atomic(uint32_t) ref_count;
    _Atomic(bool) stop_started;
    char self_name[XR_NODE_NAME_MAX + 1];
    uint16_t listen_port;
    char secret[64];
    int listen_fd;
    struct XrVMRuntime *isolate;

    // Connected nodes (linked list, protected by nodes_lock)
    XrClusterNode *nodes;
    int node_count;
    XrAdaptiveMutex nodes_lock;

    /*
     * Topic Pub/Sub registry.
     *
     * Route lookups go through a NATS-style segment trie (see
     * cluster_topic.c) instead of the old flat hash of subscriptions.
     * The trie makes send() cost O(topic_depth) instead of
     * O(total_subscriptions) — critical for applications that maintain
     * thousands of subscriptions of which only a handful match any
     * given message. The root node is embedded to keep the hot path
     * a single dereference.
     *
     * topic_root is always live between start_ex and stop; stop
     * recursively destroys the tree and resets it back to an empty
     * root.
     */
    struct XrTopicTrieNode *topic_root;  // trie root; NULL before init
    int topic_sub_count;
    XrAdaptiveMutex topics_lock;

    // Heartbeat configuration
    int heartbeat_interval_ms;  // default 5000
    int heartbeat_timeout_ms;   // default 15000 (3x interval)
    int max_missed_heartbeats;  // default 3

    // Dead node tombstones prevent immediate rejoin of recently departed nodes.
    struct {
        char name[XR_NODE_NAME_MAX + 1];
        int64_t time;
    } *tombstones;  // dynamic array
    int tombstone_count;
    int tombstone_cap;
    XrAdaptiveMutex dead_nodes_lock;

    // Node monitors (CSP-style: Channel receives notification on disconnect)
    struct XrNodeMonitor *monitors;
    int monitor_count;
    XrAdaptiveMutex monitors_lock;

    // Running state
    _Atomic(bool) running;

    /*
     * Heartbeat coroutine — spawned in cluster_runtime_start, yields on the
     * shared timer wheel between ticks, and observes running to exit.
     *
     * heartbeat_running is flipped to false by the coroutine on exit so
     * cluster_runtime_stop can wait briefly before freeing the cluster
     * state the coroutine still references. Lives in the same style as
     * accept_coro_spawned / accept_running below.
     */
    bool heartbeat_coro_spawned;
    _Atomic(bool) heartbeat_running;

    /*
     * Inbound-accept coroutine state.
     *
     *   accept_coro_spawned — true once cluster_runtime_start successfully
     *                         spawned the accept coroutine. Prevents
     *                         double-spawn and lets cluster_runtime_stop know
     *                         whether to wait for it at teardown.
     *   accept_running      — flipped to false by the coro on exit so
     *                         cluster_runtime_stop can spin-wait briefly and
     *                         avoid tearing down node state while the
     *                         accept path is still inside
     *                         cluster_node_accept.
     */
    bool accept_coro_spawned;
    _Atomic(bool) accept_running;

    // Remote coroutine monitors (linked list)
    XrRemoteCoroMonitor *remote_coro_monitors;

    // LAN auto-discovery (NULL if not enabled)
    XrClusterDiscovery *discovery;

    /*
     * Optional inter-node TLS wrap (see cluster_runtime_start).
     *
     *   tls_enabled     — flip to turn on TLS for every inbound and
     *                     outbound cluster connection.
     *   tls_client_ctx  — used by the outgoing join state machine when TLS is on.
     *                     Built at start_ex time with caller-supplied CA
     *                     bundle, optional client cert/key (for mTLS), and
     *                     optional verify_peer toggle.
     *   tls_server_ctx  — used by cluster_spawn_inbound to wrap inbound fds
     *                     with xr_tls_conn_new before the incremental server
     *                     handshake. NULL if the
     *                     operator did not supply a cert+key pair; in
     *                     that case tls_enabled + NULL tls_server_ctx
     *                     causes the accept loop to refuse all inbound
     *                     connections rather than silently downgrade.
     *
     * These fields stay NULL/false when TLS is not requested.
     */
    bool tls_enabled;
    XrTlsContext *tls_client_ctx;
    XrTlsContext *tls_server_ctx;
} XrCluster;

/* ========== Cluster Lifecycle API ========== */

/*
 * TLS options for cluster_runtime_start.
 *
 *   enabled          — master switch. When false the other fields are
 *                      ignored and the cluster reverts to plain TCP.
 *
 *   ca_file          — path to a PEM bundle used to verify peer
 *                      certificates. Pass NULL to fall back on the
 *                      system trust store (TLS contexts are created
 *                      with SSL_CTX_set_default_verify_paths by
 *                      default). If the path ends in '/' it is
 *                      interpreted as a directory (OpenSSL CApath).
 *
 *   cert_file,
 *   key_file         — optional server certificate and private key in
 *                      PEM format. Supplying both enables mTLS /
 *                      inbound TLS accept. Leaving them NULL builds a
 *                      client-only cluster (still useful: outgoing
 *                      join traffic is encrypted).
 *
 *   insecure         — disable peer certificate verification. Set true
 *                      only for development / self-signed sandboxes.
 *                      This is a loaded footgun in production and
 *                      should be logged by callers.
 *
 * All string pointers are borrowed for the duration of the call; the
 * contents are copied into OpenSSL contexts that live until
 * cluster_runtime_stop.
 */
typedef struct XrClusterTlsOptions {
    bool enabled;
    const char *ca_file;
    const char *cert_file;
    const char *key_file;
    bool insecure;
} XrClusterTlsOptions;

/*
 * Start a cluster with explicit TLS options. Passing `tls == NULL` starts
 * a plain TCP cluster.
 */
int cluster_runtime_start(struct XrVMRuntime *X, const char *name, uint16_t port,
                          const char *secret, const XrClusterTlsOptions *tls,
                          int heartbeat_interval_ms, int heartbeat_timeout_ms,
                          int max_missed_heartbeats);
void cluster_runtime_retain(XrCluster *c);
void cluster_runtime_release(XrCluster *c);

// Start a netpoll-driven outgoing join without blocking the caller's worker.
bool cluster_runtime_join_spawn(XrCluster *c, const char *host, uint16_t port);

// Stop the cluster and close all connections
void cluster_runtime_stop(XrCluster *c);

// Check if cluster is running
bool cluster_runtime_is_running(XrCluster *c);

// Get self node name
const char *cluster_runtime_self_name(XrCluster *c);

/* ========== Node Query API ========== */

// Find a node by name and return a retained reference. Caller releases it.
XrClusterNode *cluster_node_find(XrCluster *c, const char *name);

// Transfer the caller's node reference into the live-node list.
bool cluster_node_add(XrCluster *c, XrClusterNode *node);

// Detach the list-owned reference. The caller releases it when true.
bool cluster_node_remove(XrCluster *c, XrClusterNode *node);

/* ========== Frame Processing ========== */

// Process one fully decoded frame. Socket framing is owned by the yieldable
// native reader backend in cluster_node.c.
void cluster_process_frame(XrCluster *c, XrClusterNode *node, uint8_t frame_type,
                           const uint8_t *payload, uint32_t payload_len);

/* ========== Health & Robustness ========== */

// Check all nodes for heartbeat timeout, disconnect dead ones
void cluster_health_check_heartbeats(XrCluster *c);

// Send heartbeat pings to all connected nodes
void cluster_health_send_heartbeats(XrCluster *c);

// Dead node tombstone management
void cluster_health_mark_dead(XrCluster *c, const char *name);
bool cluster_health_is_dead(XrCluster *c, const char *name);

/* ========== Node Monitor (CSP-style fault detection) ========== */

typedef struct XrNodeMonitor {
    char node_name[XR_NODE_NAME_MAX + 1];  // "*" = monitor all nodes
    struct XrChannel *notify_ch;           // Receives node name string on disconnect
    struct XrNodeMonitor *next;
} XrNodeMonitor;

// Monitor a specific node. Returns a Channel that receives the node name
// as a string when that node disconnects. Use "*" to monitor all nodes.
struct XrChannel *cluster_monitor_node(struct XrVMRuntime *X, const char *node_name);

// Fire monitors for a disconnected node (called internally)
void cluster_monitor_fire(XrCluster *c, const char *node_name);

/* ========== Topic Pub/Sub ========== */

typedef struct XrTopicSubscription {
    char pattern[XR_TOPIC_PATTERN_MAX + 1];  // e.g. "events.*" or "chat.room1"
    struct XrChannel *notify_ch;             // Delivers published values
    struct XrTopicSubscription *next;        // Hash chain
} XrTopicSubscription;

// Listen on a topic pattern. Returns a Channel that receives opaque Buffer values.
// Supports wildcard: "*" matches one segment, ">" matches remaining segments.
// Example: "events.*" matches "events.user" but not "events.user.login"
//          "events.>" matches "events.user" and "events.user.login"
#define XR_CLUSTER_SUBSCRIPTION_CAPACITY_MAX (1024u * 1024u)

struct XrChannel *cluster_transport_listen(struct XrVMRuntime *X, const char *pattern,
                                           uint32_t capacity);

// Send an opaque canonical envelope to matching local listeners and peers.
XrClusterDelivery cluster_transport_send(struct XrVMRuntime *X, const char *topic,
                                         const uint8_t *envelope, uint32_t envelope_len,
                                         uint8_t hop_limit);

/*
 * Handle an incoming opaque envelope frame from a remote node.
 *
 *   from       — the XrClusterNode the frame arrived from, used for
 *                split-horizon so we never echo the frame back to
 *                its sender. NULL is legal (locally-injected test
 *                frames etc.) but in production always non-NULL.
 *   hop_limit  — the hop count encoded in the frame's fixed header.
 *                0 means "deliver locally only, do not re-forward"
 *                Non-zero causes a decrement and a re-send to every
 *                other connected peer.
 *
 * The envelope is delivered to local listeners regardless of hop_limit.
 */
void cluster_transport_handle_frame(XrCluster *c, struct XrClusterNode *from, const char *topic,
                                    const uint8_t *envelope, uint32_t envelope_len,
                                    uint8_t hop_limit);

// Deliver to local subscribers matching the topic
XrClusterDelivery cluster_transport_deliver_local(XrCluster *c, const char *topic,
                                                  const uint8_t *envelope, uint32_t envelope_len);

/*
 * Topic trie lifecycle. cluster_topics_init must be called once
 * after topics_lock is initialised and before any subscribe path is
 * exposed. cluster_topics_destroy closes every subscriber channel,
 * recursively frees the trie, and resets topic_root to NULL — call it
 * exactly once from cluster_runtime_stop (the function tolerates a NULL
 * root, so double-stop is safe).
 */
int cluster_topics_init(XrCluster *c);
void cluster_topics_destroy(XrCluster *c);

/* ========== Remote Coroutine Monitoring ========== */

// Remote monitor entry: tracks which remote coroutine we're monitoring
typedef struct XrRemoteCoroMonitor {
    char node_name[64];           // Remote node name
    char coro_name[128];          // Remote coroutine name
    struct XrChannel *notify_ch;  // Local channel for exit notification
    struct XrRemoteCoroMonitor *next;
} XrRemoteCoroMonitor;

// Monitor a coroutine on a remote node. Returns a Channel that receives
// exit reason string when the remote coroutine terminates.
// cluster.monitor("node_name", "coro_name")
struct XrChannel *cluster_monitor_coro(struct XrVMRuntime *X, const char *node_name,
                                       const char *coro_name);

// Handle incoming CORO_EXIT frame from a remote node
void cluster_monitor_handle_coro_exit(XrCluster *c, const char *coro_name, const char *reason);

// Handle incoming CORO_MONITOR request from a remote node
void cluster_monitor_handle_coro_request(XrCluster *c, struct XrClusterNode *node,
                                         const char *coro_name);

/* ========== LAN Discovery ========== */

int cluster_discovery_start(XrCluster *c);
void cluster_discovery_stop(XrCluster *c);

/* ========== Cluster Info API ========== */

// Returns Json with full cluster state, node metrics, phi values
// Exposed as cluster.info() in xray

#endif
