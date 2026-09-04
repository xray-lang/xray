/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster.c - Cluster module top-level initialization and xray bindings
 *
 * KEY CONCEPT:
 *   Manages the cluster lifecycle (one per isolate) and provides xray-level
 *   function bindings (cluster.start, cluster.join, cluster.stop, etc.)
 */

#include "cluster.h"
#include "cluster_internal.h"
#include "../../stdlib/common.h"
#include "../../src/io/xnet_transport.h"
#include "../../src/io/xnet_handle.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/module/xstdlib_runtime_cache.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/vm/xvm.h"
#include "../../src/vm/xvm_closure.h"
#include "../../src/vm/xvm_coro_api.h"
#include "../../src/base/xchecks.h"

#include <limits.h>

/* ========== xray Function Bindings ========== */

static XrValue cluster_stop_fn(XrVMRuntime *X, XrValue *args, int argc);

static XrValue cluster_health_snapshot_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrClass *snapshot_class =
        xr_stdlib_record_class_get(X, "cluster", "__ClusterHealthPeerSnapshot");
    if (!snapshot_class)
        return xr_null();
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster || !atomic_load(&cluster->running)) {
        cluster_runtime_release(cluster);
        return xr_null();
    }
    XrArray *snapshots = xr_array_new(NULL);
    if (!snapshots) {
        cluster_runtime_release(cluster);
        return xr_null();
    }
    XrValue snapshots_value = xr_value_from_array(snapshots);
    bool complete = true;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *node = cluster->nodes; node; node = node->next) {
        XrObjectInstance *snapshot =
            xr_object_instance_new_with_class(xr_current_coro(X), snapshot_class);
        if (!snapshot) {
            complete = false;
            break;
        }
        bool fields_set = xr_object_instance_set_by_key(X, snapshot, "peerGeneration",
                                                        xr_int((int64_t) node->generation_token)) &&
                          xr_object_instance_set_by_key(X, snapshot, "lastReceivedAtMs",
                                                        xr_int(node->last_heartbeat_recv));
        XrValue snapshot_value = xr_object_instance_value(snapshot);
        if (!fields_set ||
            xr_array_push_owned_checked(snapshots_value, snapshot_value) != XR_ARRAY_PUSH_OK) {
            xr_rc_release_value(xr_current_coro_heap(), snapshot_value);
            complete = false;
            break;
        }
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    cluster_runtime_release(cluster);
    if (!complete) {
        xr_rc_release_value(xr_current_coro_heap(), snapshots_value);
        return xr_null();
    }
    return snapshots_value;
}

static XrValue cluster_health_apply_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_bool(false);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster) {
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }

    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    int64_t expected_last_received = XR_TO_INT(args[1]);
    XrClusterNode *detached = NULL;
    bool applied = false;
    xr_amutex_lock(&cluster->nodes_lock);
    XrClusterNode **cursor = &cluster->nodes;
    while (*cursor) {
        XrClusterNode *node = *cursor;
        if (node->generation_token == generation &&
            node->last_heartbeat_recv == expected_last_received) {
            *cursor = node->next;
            node->next = NULL;
            detached = node;
            applied = true;
            break;
        }
        cursor = &node->next;
    }
    xr_amutex_unlock(&cluster->nodes_lock);

    if (detached) {
        cluster_node_shutdown(detached);
        cluster_node_release(detached);
    }
    cluster_runtime_release(cluster);
    return xr_bool(applied);
}

static XrValue cluster_adopt_peer_fn(XrVMRuntime *X, XrValue *args, int argc) {
    XrNetConn *handle = argc > 0 ? xr_net_conn_from_value(args[0]) : NULL;
    XrClosure *inbound_handler =
        argc > 3 ? xr_closure_from_callback_arg(X, args[3], "cluster.__adoptPeer") : NULL;
    XrClosure *outbound_handler =
        argc > 4 ? xr_closure_from_callback_arg(X, args[4], "cluster.__adoptPeer") : NULL;
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster || argc < 5 || !handle || !inbound_handler || !outbound_handler ||
        !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]) || XR_TO_INT(args[1]) <= 0 ||
        !atomic_load(&cluster->running)) {
        if (handle)
            xr_net_conn_close(handle);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }

    uint64_t generation = (uint64_t) XR_TO_INT(args[1]);
    int64_t connected_at_ms = XR_TO_INT(args[2]);
    XrClusterNode *node = (XrClusterNode *) xr_calloc(1, sizeof(*node));
    if (!node) {
        xr_net_conn_close(handle);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    atomic_store(&node->ref_count, 1);
    atomic_store(&node->shutdown_started, false);
    node->outq = xr_cluster_output_queue_new(cluster->output_queue_high_watermark);
    if (!node->outq) {
        xr_free(node);
        xr_net_conn_close(handle);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    node->conn = xr_io_conn_take_net_handle(handle);
    if (!node->conn) {
        cluster_node_release(node);
        xr_net_conn_close(handle);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    node->last_heartbeat_recv = connected_at_ms;
    node->generation_token = generation;

    /* Source reserved the peer identity before transferring its socket. This
     * registry enforces only the resource invariant that one generation owns at
     * most one transport, and stop cannot acquire it after detaching the list. */
    bool published = false;
    xr_amutex_lock(&cluster->nodes_lock);
    if (atomic_load(&cluster->running)) {
        published = true;
        for (XrClusterNode *existing = cluster->nodes; existing; existing = existing->next) {
            if (existing->generation_token == node->generation_token) {
                published = false;
                break;
            }
        }
        if (published) {
            node->next = cluster->nodes;
            cluster->nodes = node;
            /* The list owns the construction reference. Keep a separate
             * bootstrap lease until both source coroutine shells have either
             * been published or rolled back; stop may detach the list as soon
             * as nodes_lock is released. */
            cluster_node_retain(node);
        }
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    if (!published) {
        cluster_node_shutdown(node);
        cluster_node_release(node);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    XrRuntime *runtime = (XrRuntime *) X->vm.scheduler;
    XrValue generation_value = xr_int((int64_t) node->generation_token);
    const uint8_t arg_mode = XR_TRANSFER_SHARE;
    XrCoroutine *writer =
        runtime ? xr_coro_create_vm_closure_owned(X, outbound_handler, &generation_value, &arg_mode,
                                                  1, "cluster_writer", NULL, 0)
                : NULL;
    XrCoroutine *reader =
        writer ? xr_coro_create_vm_closure_owned(X, inbound_handler, &generation_value, &arg_mode,
                                                 1, "cluster_reader", NULL, 0)
               : NULL;
    if (!reader) {
        if (writer)
            xr_coro_destroy(writer);
        if (cluster_node_remove(cluster, node)) {
            cluster_node_shutdown(node);
            cluster_node_release(node);
        }
        cluster_node_release(node);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    /* Source supplied both loops after admission. This leaf only publishes
     * their scheduler shells as one batch so neither transport direction can
     * monopolize the adopting worker's local run-next slot. */
    XrCoroutine *io_coros[2] = {writer, reader};
    xr_runtime_spawn_batch(runtime, io_coros, 2);
    cluster_node_release(node);
    cluster_runtime_release(cluster);
    return xr_bool(true);
}

// The pure-Xray public wrapper normalizes ClusterConfig into scalar values so
// both backends consume one representation-independent runtime boundary.
static XrValue cluster_start_primitive(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);

    int64_t output_queue_high_watermark_value = XR_TO_INT(args[0]);
    uint64_t output_queue_high_watermark =
        output_queue_high_watermark_value > 0 ? (uint64_t) output_queue_high_watermark_value : 0;
    if (output_queue_high_watermark == 0 || output_queue_high_watermark > SIZE_MAX)
        return xr_bool(false);

    /* Xray owns validated configuration, TLS resources and start ordering.
     * This leaf materializes only the locked peer table and isolate-local
     * provider slot. */
    XrCluster *cluster = (XrCluster *) xr_calloc(1, sizeof(*cluster));
    if (!cluster)
        return xr_bool(false);
    atomic_store(&cluster->ref_count, 1);
    cluster->isolate = X;
    xr_amutex_init(&cluster->nodes_lock);

    cluster->output_queue_high_watermark = (size_t) output_queue_high_watermark;
    atomic_store(&cluster->running, true);
    bool published = xr_isolate_provider_publish(X, &X->cluster, cluster, cluster_stop_fn);
    if (!published) {
        atomic_store(&cluster->running, false);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    return xr_bool(true);
}

// cluster.stop()
static XrValue cluster_stop_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    /* Detach transfers the slot's owning reference to this caller and blocks
     * later acquisitions of the retired provider generation. */
    XrCluster *cluster = (XrCluster *) xr_isolate_provider_detach(X, &X->cluster, cluster_stop_fn);
    if (!cluster)
        return xr_null();
    atomic_store(&cluster->running, false);

    xr_amutex_lock(&cluster->nodes_lock);
    XrClusterNode *node = cluster->nodes;
    cluster->nodes = NULL;
    xr_amutex_unlock(&cluster->nodes_lock);
    while (node) {
        XrClusterNode *next = node->next;
        node->next = NULL;
        cluster_node_shutdown(node);
        cluster_node_release(node);
        node = next;
    }

    /* Transport coroutines retain independent provider references. */
    cluster_runtime_release(cluster);
    return xr_null();
}

/* ========== Inbound provider projections ========== */

static XrValue cluster_peer_enqueue_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_ARRAY(args[1]))
        return xr_int(XR_CLUSTER_OUTPUT_INVALID);
    XrArray *wire = XR_TO_ARRAY(args[1]);
    if (wire->elem_type != XR_ELEM_U8 || wire->length < 0 || (wire->length > 0 && !wire->data))
        return xr_int(XR_CLUSTER_OUTPUT_INVALID);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster || !atomic_load(&cluster->running)) {
        cluster_runtime_release(cluster);
        return xr_int(XR_CLUSTER_OUTPUT_STOPPED);
    }
    XrClusterNode *node = cluster_node_acquire(cluster, (uint64_t) XR_TO_INT(args[0]));
    if (!node) {
        cluster_runtime_release(cluster);
        return xr_int(XR_CLUSTER_OUTPUT_STOPPED);
    }
    XrClusterOutputPushResult queued = xr_cluster_output_queue_push_copy(
        node->outq, (const uint8_t *) wire->data, (uint32_t) wire->length);
    cluster_node_release(node);
    cluster_runtime_release(cluster);
    return xr_int(queued);
}

static XrValue cluster_observe_heartbeat_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 3 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]))
        return xr_bool(false);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_bool(false);
    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    int64_t received_at_ms = XR_TO_INT(args[1]);
    int64_t rtt_ms = XR_TO_INT(args[2]);
    bool observed = false;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *node = cluster->nodes; node; node = node->next) {
        if (node->generation_token != generation)
            continue;
        if (received_at_ms >= node->last_heartbeat_recv) {
            node->last_heartbeat_recv = received_at_ms;
            if (rtt_ms >= 0)
                node->metrics.last_rtt_ms = rtt_ms;
        }
        observed = true;
        break;
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    cluster_runtime_release(cluster);
    return xr_bool(observed);
}

static XrValue cluster_detach_peer_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_bool(false);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_bool(false);
    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    XrClusterNode *detached = NULL;
    xr_amutex_lock(&cluster->nodes_lock);
    XrClusterNode **link = &cluster->nodes;
    while (*link) {
        XrClusterNode *candidate = *link;
        if (candidate->generation_token == generation) {
            *link = candidate->next;
            candidate->next = NULL;
            detached = candidate;
            break;
        }
        link = &candidate->next;
    }
    xr_amutex_unlock(&cluster->nodes_lock);

    if (detached) {
        cluster_node_shutdown(detached);
        cluster_node_release(detached);
    }
    cluster_runtime_release(cluster);
    return xr_bool(detached != NULL);
}

/* ========== Cluster Info API ========== */

static XrValue cluster_runtime_snapshot_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrClass *info_class = xr_stdlib_record_class_get(X, "cluster", "__ClusterRuntimeSnapshot");
    XrClass *node_class = xr_stdlib_record_class_get(X, "cluster", "__ClusterNodeSnapshot");
    if (!info_class || !node_class)
        return xr_null();
    XrCluster *c = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, c);
    if (!c)
        return xr_null();
    XrObjectInstance *info = xr_object_instance_new_with_class(NULL, info_class);
    if (!info) {
        cluster_runtime_release(c);
        return xr_null();
    }

    // Node list with metrics
    XrArray *node_arr = xr_array_new(NULL);
    if (node_arr) {
        xr_amutex_lock(&c->nodes_lock);
        XrClusterNode *node = c->nodes;
        while (node) {
            XrObjectInstance *nj = xr_object_instance_new_with_class(NULL, node_class);
            if (nj) {
                xr_object_instance_set_by_key(X, nj, "peerGeneration",
                                              xr_int((int64_t) node->generation_token));
                xr_object_instance_set_by_key(X, nj, "lastReceivedAtMs",
                                              xr_int(node->last_heartbeat_recv));
                /*
                 * Per-node metrics snapshot. All counters are
                 * atomic _Atomic(uint64_t) so the load is wait-free
                 * and consistent per-field (no struct-level tearing
                 * because each load is independent). A
                 * whole-metrics-block observation is NOT atomic —
                 * bytes_sent may advance after frames_sent is read,
                 * producing a momentarily-impossible ratio; the
                 * tradeoff is acceptable for a diagnostic JSON.
                 */
                xr_object_instance_set_by_key(
                    X, nj, "framesSent", xr_int((int64_t) atomic_load(&node->metrics.frames_sent)));
                xr_object_instance_set_by_key(
                    X, nj, "framesReceived",
                    xr_int((int64_t) atomic_load(&node->metrics.frames_recv)));
                xr_object_instance_set_by_key(
                    X, nj, "bytesSent", xr_int((int64_t) atomic_load(&node->metrics.bytes_sent)));
                xr_object_instance_set_by_key(
                    X, nj, "bytesReceived",
                    xr_int((int64_t) atomic_load(&node->metrics.bytes_recv)));
                // send_errors: writev short/fail counter — high values
                // flag a slow or lossy link; correlate with the slow
                // flag below.
                xr_object_instance_set_by_key(
                    X, nj, "sendErrors", xr_int((int64_t) atomic_load(&node->metrics.send_errors)));
                // slow_consumer_events: total times this peer hit the
                // source-configured high watermark since start. Each
                // event corresponds to one outq_bytes >= high_watermark
                // transition in cluster_node.
                xr_object_instance_set_by_key(
                    X, nj, "slowConsumerEvents",
                    xr_int((int64_t) atomic_load(&node->metrics.slow_consumer_events)));
                xr_object_instance_set_by_key(X, nj, "rttMs", xr_int(node->metrics.last_rtt_ms));
                xr_object_instance_set_by_key(X, nj, "outQueueBytes",
                                              xr_int(xr_cluster_output_queue_bytes(node->outq)));
                xr_object_instance_set_by_key(X, nj, "outQueueFrames",
                                              xr_int(xr_cluster_output_queue_frames(node->outq)));
                xr_object_instance_set_by_key(X, nj, "slow",
                                              xr_bool(xr_cluster_output_queue_is_full(node->outq)));

                xr_array_push(node_arr, xr_object_instance_value(nj));
            } else {
                xr_amutex_unlock(&c->nodes_lock);
                cluster_runtime_release(c);
                return xr_null();
            }
            node = node->next;
        }
        xr_amutex_unlock(&c->nodes_lock);
        xr_object_instance_set_by_key(X, info, "nodes", xr_value_from_array(node_arr));
    } else {
        cluster_runtime_release(c);
        return xr_null();
    }

    XrValue result = xr_object_instance_value(info);
    cluster_runtime_release(c);
    return result;
}

#define XR_STDLIB_VM_BIND_MODULE_CLUSTER 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_CLUSTER
