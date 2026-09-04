/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_node.c - Cluster peer transport bindings
 *
 * KEY CONCEPT:
 *   The VM bindings acquire a generation-safe peer resource lease and hand it
 *   to the generic framed I/O provider. Protocol, retry, overload and
 *   disconnect policy remain in cluster.xr.
 */

#include "cluster_internal.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/vm/xvm_closure.h"

/* ========== Generation-safe Peer I/O Leases ========== */

XrValue cluster_peer_read_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 3 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]) ||
        !xr_closure_from_callback_arg(X, args[2], "cluster.__readPeer"))
        return xr_int(XR_CLUSTER_PEER_READ_PROVIDER_ERROR);
    if (XR_TO_INT(args[1]) <= 0 || XR_TO_INT(args[1]) > UINT32_MAX)
        return xr_int(XR_CLUSTER_PEER_READ_INVALID_LIMIT);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster || !atomic_load(&cluster->running)) {
        cluster_runtime_release(cluster);
        return xr_int(XR_CLUSTER_PEER_READ_RUNTIME_STOPPED);
    }

    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    XrClusterNode *node = cluster_node_acquire(cluster, generation);
    if (!node) {
        cluster_runtime_release(cluster);
        return xr_int(XR_CLUSTER_PEER_READ_STALE);
    }

    XrClusterPeerIoLease lease = {
        .conn = node->conn,
        .queue = node->outq,
        .owner = node,
        .release_owner = cluster_node_release_lease,
        .runtime_owner = cluster,
        .release_runtime_owner = cluster_runtime_release_lease,
        .frames_recv = &node->metrics.frames_recv,
        .bytes_recv = &node->metrics.bytes_recv,
    };
    return xr_int(
        xr_cluster_peer_read_start(X, &lease, generation, (uint32_t) XR_TO_INT(args[1]), args[2]));
}

XrValue cluster_peer_write_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_INT(args[0]) ||
        !xr_closure_from_callback_arg(X, args[1], "cluster.__writePeer"))
        return xr_int(XR_CLUSTER_PEER_WRITE_PROVIDER_ERROR);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster || !atomic_load(&cluster->running)) {
        cluster_runtime_release(cluster);
        return xr_int(XR_CLUSTER_PEER_WRITE_RUNTIME_STOPPED);
    }

    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    XrClusterNode *node = cluster_node_acquire(cluster, generation);
    if (!node) {
        cluster_runtime_release(cluster);
        return xr_int(XR_CLUSTER_PEER_WRITE_STALE);
    }

    XrClusterPeerIoLease lease = {
        .conn = node->conn,
        .queue = node->outq,
        .owner = node,
        .release_owner = cluster_node_release_lease,
        .runtime_owner = cluster,
        .release_runtime_owner = cluster_runtime_release_lease,
        .frames_sent = &node->metrics.frames_sent,
        .bytes_sent = &node->metrics.bytes_sent,
        .send_errors = &node->metrics.send_errors,
        .queue_full_events = &node->metrics.slow_consumer_events,
    };
    return xr_int(xr_cluster_peer_write_start(X, &lease, generation, args[1]));
}
