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

#include "../../src/base/xchecks.h"
#include "../../src/base/xmalloc.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xcluster_output_queue.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xmonitor_registry.h"
#include "../../src/coro/xphi_detector.h"
#include "../../src/coro/xtombstone_registry.h"
#include "../../src/coro/xtopic_registry.h"
#include "../../src/io/xcluster_wire.h"
#include "../../src/io/xcluster_peer_transport.h"
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

typedef struct XrClusterBroadcastStats {
    int64_t connected;
    int64_t accepted;
} XrClusterBroadcastStats;

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
    uint64_t generation_token;

    XrClusterOutputQueue *outq;

    XrNodeMetrics metrics;
    XrPhiDetector phi;

    struct XrClusterNode *next;
} XrClusterNode;

XrCFuncResult cluster_peer_read_fn(struct XrVMRuntime *isolate, XrValue *args, int argc,
                                   XrValue *result);
XrCFuncResult cluster_peer_write_fn(struct XrVMRuntime *isolate, XrValue *args, int argc,
                                    XrValue *result);

/* ========== Cluster State ========== */

typedef struct XrCluster {
    _Atomic(uint32_t) ref_count;
    _Atomic(bool) stop_started;
    _Atomic(uint64_t) next_peer_generation;
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

/* ========== Opaque Provider Resource Primitives ========== */

/* These operations carry no admission or disconnect policy. They only keep a
 * peer allocation alive across native I/O and close its queue/socket exactly
 * once after the registry owner has detached it. */
static inline void cluster_node_retain(XrClusterNode *node) {
    if (node)
        atomic_fetch_add_explicit(&node->ref_count, 1, memory_order_relaxed);
}

static inline void cluster_node_shutdown(XrClusterNode *node) {
    if (!node)
        return;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&node->shutdown_started, &expected, true,
                                                 memory_order_acq_rel, memory_order_acquire))
        return;
    xr_cluster_output_queue_stop(node->outq);
    node->state = XR_NODE_CLOSING;
    if (node->conn && node->conn->fd >= 0)
        (void) shutdown(node->conn->fd, XR_SHUT_RDWR);
}

static inline void cluster_node_release(XrClusterNode *node) {
    if (!node)
        return;
    uint32_t previous = atomic_fetch_sub_explicit(&node->ref_count, 1, memory_order_acq_rel);
    XR_DCHECK(previous > 0, "cluster node reference underflow");
    if (previous != 1)
        return;
    cluster_node_shutdown(node);
    XR_DCHECK(atomic_load_explicit(&node->shutdown_started, memory_order_acquire),
              "cluster node destroyed before shutdown");
    xr_io_close(node->conn);
    node->conn = NULL;
    xr_cluster_output_queue_destroy(node->outq);
    xr_free(node);
}

static inline void cluster_node_release_lease(void *owner) {
    cluster_node_release((XrClusterNode *) owner);
}

/* The caller must hold a cluster runtime reference. The returned peer lease
 * survives registry detachment until cluster_node_release() balances it. */
static inline XrClusterNode *cluster_node_acquire(XrCluster *cluster, uint64_t generation) {
    if (!cluster)
        return NULL;
    XrClusterNode *node = NULL;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *candidate = cluster->nodes; candidate; candidate = candidate->next) {
        if (candidate->generation_token == generation && candidate->state == XR_NODE_CONNECTED &&
            candidate->conn) {
            cluster_node_retain(candidate);
            node = candidate;
            break;
        }
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    return node;
}

/* Transport coroutines retain the provider runtime independently of the
 * source-owned start/stop generation. The last transport reference reclaims
 * only opaque registries and TLS contexts; source policy never enters this
 * primitive. */
static inline void cluster_runtime_retain(XrCluster *cluster) {
    if (cluster)
        atomic_fetch_add(&cluster->ref_count, 1);
}

/* Keep slot lookup and the strong-reference increment in one critical
 * section. The macro expands only where XrVMRuntime is complete and avoids a
 * cluster-specific dependency in the core runtime layer. */
#define XR_CLUSTER_RUNTIME_ACQUIRE(isolate, out_cluster)                                         \
    do {                                                                                         \
        xr_amutex_lock(&(isolate)->cluster_slot_lock);                                            \
        (out_cluster) = (XrCluster *) (isolate)->cluster;                                         \
        cluster_runtime_retain(out_cluster);                                                      \
        xr_amutex_unlock(&(isolate)->cluster_slot_lock);                                          \
    } while (0)

static inline void cluster_runtime_release(XrCluster *cluster) {
    if (!cluster)
        return;
    uint32_t previous = atomic_fetch_sub(&cluster->ref_count, 1);
    XR_DCHECK(previous > 0, "cluster reference underflow");
    if (previous != 1)
        return;

    XR_DCHECK(cluster->nodes == NULL, "cluster release requires a detached node list");
    XR_DCHECK(cluster->listener == NULL, "cluster release requires a detached listener");
    xr_topic_registry_destroy(cluster->topics);
    xr_monitor_registry_destroy(cluster->monitors);
    xr_tombstone_registry_destroy(cluster->tombstones);
    if (cluster->tls_client_ctx)
        xr_tls_context_free(cluster->tls_client_ctx);
    if (cluster->tls_server_ctx)
        xr_tls_context_free(cluster->tls_server_ctx);
    xr_free(cluster);
}

static inline void cluster_runtime_release_lease(void *owner) {
    cluster_runtime_release((XrCluster *) owner);
}

/* A provider callback may race source stop or another terminal callback. The
 * pointer match is the whole operation: policy selected the generation before
 * reaching this locked primitive. */
static inline bool cluster_node_remove(XrCluster *cluster, XrClusterNode *node) {
    if (!cluster || !node)
        return false;

    bool removed = false;
    xr_amutex_lock(&cluster->nodes_lock);
    XrClusterNode **link = &cluster->nodes;
    while (*link) {
        if (*link == node) {
            *link = node->next;
            node->next = NULL;
            cluster->node_count--;
            removed = true;
            break;
        }
        link = &(*link)->next;
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    return removed;
}

/* ========== Topic Pub/Sub ========== */

#define XR_CLUSTER_SUBSCRIPTION_CAPACITY_MAX (1024u * 1024u)

/* The source layer supplies a complete, validated wire frame and decides
 * whether one peer generation is excluded. The native projection owns only
 * the locked peer walk and each synchronized queue admission result, so it
 * stays beside the node and queue representations that give those operations
 * meaning instead of exposing another transport-policy boundary. */
static inline XrClusterBroadcastStats cluster_transport_broadcast(XrCluster *cluster,
                                                                  uint64_t excluded_generation,
                                                                  const uint8_t *wire,
                                                                  uint32_t wire_length) {
    XrClusterBroadcastStats stats = {0};
    if (!cluster || !wire || wire_length == 0)
        return stats;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *node = cluster->nodes; node; node = node->next) {
        if ((excluded_generation != 0 && node->generation_token == excluded_generation) ||
            node->state != XR_NODE_CONNECTED)
            continue;
        stats.connected++;
        if (xr_cluster_output_queue_push_copy(node->outq, wire, wire_length) == 0)
            stats.accepted++;
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    return stats;
}

/* ========== Cluster Info API ========== */

// Returns Json with full cluster state, node metrics, phi values
// Exposed as cluster.info() in xray

#endif
