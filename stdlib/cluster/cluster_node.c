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

/* ========== Generation-safe Peer I/O Leases ========== */

XrCFuncResult cluster_peer_read_fn(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]) || XR_TO_INT(args[1]) <= 0 ||
        XR_TO_INT(args[1]) > UINT32_MAX) {
        *result = xr_int(XR_CLUSTER_PEER_READ_RESOURCE_UNAVAILABLE);
        return XR_CFUNC_DONE;
    }
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster || !atomic_load(&cluster->running)) {
        cluster_runtime_release(cluster);
        *result = xr_int(XR_CLUSTER_PEER_READ_RUNTIME_STOPPED);
        return XR_CFUNC_DONE;
    }

    XrClusterNode *node = cluster_node_acquire(cluster, (uint64_t) XR_TO_INT(args[0]));
    if (!node) {
        cluster_runtime_release(cluster);
        *result = xr_int(XR_CLUSTER_PEER_READ_STALE);
        return XR_CFUNC_DONE;
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
    return xr_cluster_peer_read_frame(X, &lease, (uint32_t) XR_TO_INT(args[1]), result);
}

XrCFuncResult cluster_peer_write_fn(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    if (argc < 1 || !XR_IS_INT(args[0])) {
        *result = xr_int(XR_CLUSTER_PEER_WRITE_RESOURCE_UNAVAILABLE);
        return XR_CFUNC_DONE;
    }
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster || !atomic_load(&cluster->running)) {
        cluster_runtime_release(cluster);
        *result = xr_int(XR_CLUSTER_PEER_WRITE_RUNTIME_STOPPED);
        return XR_CFUNC_DONE;
    }

    XrClusterNode *node = cluster_node_acquire(cluster, (uint64_t) XR_TO_INT(args[0]));
    if (!node) {
        cluster_runtime_release(cluster);
        *result = xr_int(XR_CLUSTER_PEER_WRITE_STALE);
        return XR_CFUNC_DONE;
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
    return xr_cluster_peer_write_batch(X, &lease, result);
}
