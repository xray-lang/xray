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
 *   Generation-keyed cluster transport for opaque service envelopes. Xray
 *   owns peer identity and authenticated admission above this resource layer.
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
#include "../../src/io/xcluster_peer_transport.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/io/xnet_transport.h"

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
    XrIOConn *conn;
    int64_t last_heartbeat_recv;
    uint64_t generation_token;

    XrClusterOutputQueue *outq;

    XrNodeMetrics metrics;

    struct XrClusterNode *next;
} XrClusterNode;

XrValue cluster_peer_read_fn(struct XrVMRuntime *isolate, XrValue *args, int argc);
XrValue cluster_peer_write_fn(struct XrVMRuntime *isolate, XrValue *args, int argc);

/* ========== Cluster State ========== */

typedef struct XrCluster {
    _Atomic(uint32_t) ref_count;
    struct XrVMRuntime *isolate;

    // Connected nodes (linked list, protected by nodes_lock)
    XrClusterNode *nodes;
    XrAdaptiveMutex nodes_lock;

    /* Provider capacity selected by cluster.xr. */
    size_t output_queue_high_watermark;
    // Running state
    _Atomic(bool) running;

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
        if (candidate->generation_token == generation && candidate->conn) {
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
 * only opaque peer resources; source policy never enters this primitive. */
static inline void cluster_runtime_retain(void *provider) {
    XrCluster *cluster = (XrCluster *) provider;
    if (cluster)
        atomic_fetch_add(&cluster->ref_count, 1);
}

/* The generic isolate boundary keeps slot lookup and the strong-reference
 * increment in one critical section. */
#define XR_CLUSTER_RUNTIME_ACQUIRE(isolate, out_cluster)                                           \
    do {                                                                                           \
        (out_cluster) = (XrCluster *) xr_isolate_provider_acquire((isolate), &(isolate)->cluster,  \
                                                                  cluster_runtime_retain);         \
    } while (0)

static inline void cluster_runtime_release(XrCluster *cluster) {
    if (!cluster)
        return;
    uint32_t previous = atomic_fetch_sub(&cluster->ref_count, 1);
    XR_DCHECK(previous > 0, "cluster reference underflow");
    if (previous != 1)
        return;

    XR_DCHECK(cluster->nodes == NULL, "cluster release requires a detached node list");
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
            removed = true;
            break;
        }
        link = &(*link)->next;
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    return removed;
}

/* ========== Cluster Info API ========== */

// Native snapshot helpers return raw cluster resource and transport facts.

#endif
