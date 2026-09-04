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
#include "../../src/coro/xcluster_output_queue.h"
#include "../../src/coro/xmonitor_registry.h"
#include "../../src/coro/xphi_detector.h"
#include "../../src/coro/xtombstone_registry.h"
#include "../../src/coro/xtopic_registry.h"
#include "../../src/io/xcluster_wire.h"
#include "../../src/io/xcluster_auth.h"
#include "../../src/io/xcluster_handshake.h"
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

/* ========== Forward Declarations ========== */

struct XrVMRuntime;
struct XrChannel;
typedef struct XrCluster XrCluster;

typedef enum XrClusterDelivery {
    XR_CLUSTER_DELIVERY_ACCEPTED = 0,
    XR_CLUSTER_DELIVERY_INVALID_TOPIC,
    XR_CLUSTER_DELIVERY_INVALID_ENVELOPE,
    XR_CLUSTER_DELIVERY_UNAVAILABLE,
    XR_CLUSTER_DELIVERY_OVERLOADED,
    XR_CLUSTER_DELIVERY_DISCONNECTED,
} XrClusterDelivery;

/* ========== Cluster Wire Protocol ========== */

#define XR_ADDRESS_HOST_MAX 255
#define XR_CLUSTER_SECRET_MAX 63
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

typedef struct XrNodeMetrics {
    _Atomic(uint64_t) frames_sent;
    _Atomic(uint64_t) frames_recv;
    _Atomic(uint64_t) bytes_sent;
    _Atomic(uint64_t) bytes_recv;
    _Atomic(uint64_t) send_errors;
    _Atomic(uint64_t) slow_consumer_events;
    int64_t last_rtt_ms;
} XrNodeMetrics;

typedef struct XrClusterNode {
    _Atomic(uint32_t) ref_count;
    _Atomic(bool) shutdown_started;
    char name[XR_NODE_NAME_MAX + 1];
    char host[XR_ADDRESS_HOST_MAX + 1];
    uint16_t port;
    XrNodeState state;
    XrIOConn *conn;
    int64_t last_heartbeat_sent;
    int64_t last_heartbeat_recv;
    uint32_t flags;
    uint32_t missed_heartbeats;

    struct XrVMRuntime *isolate;

    XrClusterOutputQueue *outq;
    _Atomic(bool) writer_running;
    _Atomic(bool) writer_exited;
    _Atomic(bool) reader_running;

    XrNodeMetrics metrics;
    XrPhiDetector phi;

    struct XrClusterNode *next;
} XrClusterNode;

XrClusterNode *cluster_node_new(const char *name, const char *host, uint16_t port,
                                double expected_heartbeat_interval_ms,
                                size_t output_queue_high_watermark);
void cluster_node_retain(XrClusterNode *node);
void cluster_node_shutdown(XrClusterNode *node);
void cluster_node_release(XrClusterNode *node);
int cluster_node_enqueue(XrClusterNode *node, const uint8_t *data, uint32_t len);
int cluster_node_send_frame(XrClusterNode *node, uint8_t frame_type, const uint8_t *payload,
                            uint32_t payload_len);
int cluster_node_send_transport_frame(XrClusterNode *node, uint8_t hop_limit, const char *topic,
                                      uint8_t topic_len, const uint8_t *envelope,
                                      uint32_t envelope_len);
int cluster_node_send_ping(XrClusterNode *node);
bool cluster_node_start_io(struct XrCluster *cluster, XrClusterNode *node);
bool cluster_node_is_slow(XrClusterNode *node);

/* ========== Cluster State ========== */

typedef struct XrCluster {
    _Atomic(uint32_t) ref_count;
    _Atomic(bool) stop_started;
    struct XrVMRuntime *isolate;
    struct XrNetListener *listener; /* borrowed while the source accept loop owns it */

    // Connected nodes (linked list, protected by nodes_lock)
    XrClusterNode *nodes;
    int node_count;
    XrAdaptiveMutex nodes_lock;

    /* Synchronized channel index for the topic policy owned by cluster.xr. */
    XrTopicRegistry *topics;

    /* Provider capacity selected by cluster.xr. */
    size_t output_queue_high_watermark;
    XrTombstoneRegistry *tombstones;
    XrMonitorRegistry *monitors;

    // Running state
    _Atomic(bool) running;

    /*
     * Optional inter-node TLS wrap.
     *
     *   tls_enabled     — flip to turn on TLS for every inbound and
     *                     outbound cluster connection.
     *   tls_client_ctx  — used by the outgoing join state machine when TLS is on.
     *                     Built at start_ex time with caller-supplied CA
     *                     bundle, optional client cert/key (for mTLS), and
     *                     optional verify_peer toggle.
     *   tls_server_ctx  — used by the cluster accept TLS provider to promote
     *                     an accepted NetConn before source starts the wire
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
 * TLS options for the native cluster runtime provider.
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
 * contents are copied into OpenSSL contexts owned by the native runtime.
 */
typedef struct XrClusterTlsOptions {
    bool enabled;
    const char *ca_file;
    const char *cert_file;
    const char *key_file;
    bool insecure;
} XrClusterTlsOptions;

void cluster_runtime_retain(XrCluster *c);
void cluster_runtime_release(XrCluster *c);

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

void cluster_health_tick(XrCluster *cluster, int64_t heartbeat_timeout_ms,
                         int64_t max_missed_heartbeats, int64_t phi_min_samples,
                         double phi_threshold);

/* ========== Topic Pub/Sub ========== */

#define XR_CLUSTER_SUBSCRIPTION_CAPACITY_MAX (1024u * 1024u)

XrClusterDelivery cluster_transport_broadcast(XrCluster *cluster,
                                              struct XrClusterNode *excluded_node,
                                              uint8_t hop_limit, const char *topic,
                                              const uint8_t *envelope, uint32_t envelope_length);

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

/* ========== Remote Coroutine Monitoring ========== */

bool cluster_monitor_register_remote(XrCluster *cluster, const char *node_name,
                                     const char *coroutine_name, struct XrChannel *channel);

void cluster_monitor_handle_coro_request(XrCluster *cluster, struct XrClusterNode *node,
                                         const char *coroutine_name);

/* ========== Cluster Info API ========== */

// Returns Json with full cluster state, node metrics, phi values
// Exposed as cluster.info() in xray

#endif
