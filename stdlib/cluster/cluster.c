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
#include "../../src/io/xnet_provider.h"
#include "../../src/io/xcluster_discovery_provider.h"
#include "../../src/runtime/object/xbuffer.h"
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

#include <string.h>
#include <limits.h>

/* ========== xray Function Bindings ========== */

static XrValue cluster_stop_fn(XrVMRuntime *X, XrValue *args, int argc);

/* Peer generations cross isolate-local cluster restarts. A retiring transport
 * continuation therefore cannot address a replacement peer through a reused
 * numeric token. Zero permanently marks process-lifetime exhaustion. */
static _Atomic(uint64_t) cluster_next_peer_generation = 1;

static XrValue cluster_recently_departed_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(true);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_bool(true);
    XrString *name = XR_TO_STRING(args[0]);
    bool contained = xr_tombstone_registry_contains(cluster->tombstones, name->data,
                                                     (int64_t) xr_time_monotonic_ms());
    cluster_runtime_release(cluster);
    return xr_bool(contained);
}

static XrValue cluster_health_snapshot_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_ARRAY(args[0]) || !XR_IS_INT(args[1]))
        return xr_null();
    XrArray *heartbeat_wire = XR_TO_ARRAY(args[0]);
    if (heartbeat_wire->elem_type != XR_ELEM_U8 || heartbeat_wire->length <= 0 ||
        !heartbeat_wire->data)
        return xr_null();

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
    int64_t sent_at_ms = XR_TO_INT(args[1]);
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *node = cluster->nodes; node; node = node->next) {
        if (node->state != XR_NODE_CONNECTED)
            continue;
        XrObjectInstance *snapshot =
            xr_object_instance_new_with_class(xr_current_coro(X), snapshot_class);
        if (!snapshot) {
            complete = false;
            break;
        }
        bool fields_set =
            xr_object_instance_set_by_key(X, snapshot, "peerGeneration",
                                          xr_int((int64_t) node->generation_token)) &&
            xr_object_instance_set_by_key(X, snapshot, "lastReceivedAtMs",
                                          xr_int(node->last_heartbeat_recv)) &&
            xr_object_instance_set_by_key(X, snapshot, "missedHeartbeats",
                                          xr_int((int64_t) node->missed_heartbeats)) &&
            xr_object_instance_set_by_key(X, snapshot, "samples",
                                          xr_int((int64_t) node->phi.sample_count)) &&
            xr_object_instance_set_by_key(X, snapshot, "mean", xr_float(node->phi.mean)) &&
            xr_object_instance_set_by_key(X, snapshot, "variance", xr_float(node->phi.variance)) &&
            xr_object_instance_set_by_key(X, snapshot, "detectorLastHeartbeatMs",
                                          xr_int(node->phi.last_heartbeat_ts));
        XrValue snapshot_value = xr_object_instance_value(snapshot);
        if (!fields_set ||
            xr_array_push_owned_checked(snapshots_value, snapshot_value) != XR_ARRAY_PUSH_OK) {
            xr_rc_release_value(xr_current_coro_heap(), snapshot_value);
            complete = false;
            break;
        }
    }
    if (complete) {
        for (XrClusterNode *node = cluster->nodes; node; node = node->next) {
            if (node->state == XR_NODE_CONNECTED && node->conn &&
                xr_cluster_output_queue_push_copy(node->outq,
                                                  (const uint8_t *) heartbeat_wire->data,
                                                  (uint32_t) heartbeat_wire->length) == 0)
                node->last_heartbeat_sent = sent_at_ms;
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
    if (argc < 6 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_INT(args[2]) ||
        !XR_IS_INT(args[3]) || !XR_IS_BOOL(args[4]) || !XR_IS_INT(args[5]))
        return xr_bool(false);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    int64_t expected_missed = XR_TO_INT(args[2]);
    int64_t next_missed = XR_TO_INT(args[3]);
    if (!cluster || expected_missed < 0 || expected_missed > UINT32_MAX || next_missed < 0 ||
        next_missed > UINT32_MAX) {
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }

    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    int64_t expected_last_received = XR_TO_INT(args[1]);
    bool disconnect = XR_TO_BOOL(args[4]);
    XrClusterNode *detached = NULL;
    bool applied = false;
    xr_amutex_lock(&cluster->nodes_lock);
    XrClusterNode **cursor = &cluster->nodes;
    while (*cursor) {
        XrClusterNode *node = *cursor;
        if (node->generation_token == generation && node->state == XR_NODE_CONNECTED &&
            node->last_heartbeat_recv == expected_last_received &&
            node->missed_heartbeats == (uint32_t) expected_missed) {
            if (disconnect) {
                *cursor = node->next;
                node->next = NULL;
                detached = node;
            } else {
                node->missed_heartbeats = (uint32_t) next_missed;
            }
            applied = true;
            break;
        }
        cursor = &node->next;
    }
    xr_amutex_unlock(&cluster->nodes_lock);

    if (detached) {
        (void) xr_tombstone_registry_add(cluster->tombstones, detached->name, XR_TO_INT(args[5]));
        xr_monitor_registry_notify_node(cluster->monitors, cluster->isolate, detached->name);
        cluster_node_shutdown(detached);
        cluster_node_release(detached);
    }
    cluster_runtime_release(cluster);
    return xr_bool(applied);
}

static XrValue cluster_track_listener_fn(XrVMRuntime *X, XrValue *args, int argc) {
    XrNetListener *listener = NULL;
    if (argc > 0 && XR_IS_PTR(args[0]) && XR_HEAP_TYPE(args[0]) == XR_TINSTANCE) {
        listener = (XrNetListener *) XR_VALUE_GCPTR(args[0]);
        if (!listener->klass || listener->klass->builtin_kind != XR_BK_NETLISTENER)
            listener = NULL;
    }
    if (!listener || xr_net_listener_is_closed(listener))
        return xr_bool(false);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_bool(false);
    xr_amutex_lock(&cluster->nodes_lock);
    bool tracked = atomic_load(&cluster->running) && cluster->listener == NULL;
    if (tracked)
        cluster->listener = listener;
    xr_amutex_unlock(&cluster->nodes_lock);
    cluster_runtime_release(cluster);
    return xr_bool(tracked);
}

static XrCFuncResult cluster_join_tls_fn(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
#ifdef XR_ENABLE_TLS
    XrCFuncResult status = xr_net_tls_handshake_with_context(
        X, args, argc, result, cluster && cluster->tls_enabled ? cluster->tls_client_ctx : NULL);
    cluster_runtime_release(cluster);
    return status;
#else
    cluster_runtime_release(cluster);
    XrNetConn *conn = argc > 0 && args ? xr_net_conn_from_value(args[0]) : NULL;
    if (conn)
        xr_net_conn_close(conn);
    *result = xr_int(XR_NETERR_TLS);
    return XR_CFUNC_DONE;
#endif
}

static XrCFuncResult cluster_accept_tls_fn(XrVMRuntime *X, XrValue *args, int argc,
                                           XrValue *result) {
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
#ifdef XR_ENABLE_TLS
    XrCFuncResult status = xr_net_tls_server_handshake_with_context(
        X, args, argc, result, cluster && cluster->tls_enabled ? cluster->tls_server_ctx : NULL);
    cluster_runtime_release(cluster);
    return status;
#else
    cluster_runtime_release(cluster);
    XrNetConn *conn = argc > 0 && args ? xr_net_conn_from_value(args[0]) : NULL;
    if (conn)
        xr_net_conn_close(conn);
    *result = xr_int(XR_NETERR_TLS);
    return XR_CFUNC_DONE;
#endif
}

static XrValue cluster_adopt_peer_fn(XrVMRuntime *X, XrValue *args, int argc) {
    XrNetConn *handle = argc > 0 ? xr_net_conn_from_value(args[0]) : NULL;
    XrClosure *inbound_handler =
        argc > 6 ? xr_closure_from_callback_arg(X, args[6], "cluster.__adoptPeer") : NULL;
    XrClosure *outbound_handler =
        argc > 7 ? xr_closure_from_callback_arg(X, args[7], "cluster.__adoptPeer") : NULL;
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster || argc < 8 || !handle || !inbound_handler || !outbound_handler ||
        !XR_IS_STRING(args[1]) || !XR_IS_STRING(args[2]) || !XR_IS_INT(args[3]) ||
        !XR_IS_INT(args[4]) || !XR_IS_INT(args[5]) || !atomic_load(&cluster->running)) {
        if (handle)
            xr_net_conn_close(handle);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }

    XrString *name = XR_TO_STRING(args[1]);
    XrString *host = XR_TO_STRING(args[2]);
    int64_t port = XR_TO_INT(args[3]);
    int64_t flags = XR_TO_INT(args[4]);
    int64_t heartbeat_interval_ms = XR_TO_INT(args[5]);
    XrClusterNode *node = (XrClusterNode *) xr_calloc(1, sizeof(*node));
    if (!node) {
        xr_net_conn_close(handle);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    atomic_store(&node->ref_count, 1);
    atomic_store(&node->shutdown_started, false);
    strncpy(node->name, name->data, XR_NODE_NAME_MAX);
    node->name[XR_NODE_NAME_MAX] = '\0';
    strncpy(node->host, host->data, sizeof(node->host) - 1);
    node->port = (uint16_t) port;
    node->state = XR_NODE_IDLE;
    node->outq = xr_cluster_output_queue_new(cluster->output_queue_high_watermark);
    if (!node->outq) {
        xr_free(node);
        xr_net_conn_close(handle);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    xr_phi_detector_init(&node->phi, (double) heartbeat_interval_ms);

    node->conn = xr_io_conn_take_net_handle(handle, XR_CLUSTER_HANDSHAKE_TIMEOUT_MS);
    if (!node->conn) {
        cluster_node_release(node);
        xr_net_conn_close(handle);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    node->state = XR_NODE_CONNECTED;
    node->flags = (uint32_t) flags;
    node->last_heartbeat_recv = (int64_t) xr_time_monotonic_ms();
    uint64_t next_generation =
        atomic_load_explicit(&cluster_next_peer_generation, memory_order_relaxed);
    while (next_generation != 0) {
        uint64_t next = next_generation == UINT64_MAX ? 0 : next_generation + 1;
        if (atomic_compare_exchange_weak_explicit(&cluster_next_peer_generation, &next_generation,
                                                  next, memory_order_relaxed,
                                                  memory_order_relaxed))
            break;
    }
    node->generation_token = next_generation;
    if (node->generation_token == 0) {
        cluster_node_shutdown(node);
        cluster_node_release(node);
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }

    /* Source admitted the peer before transferring its socket. Publish that
     * decision atomically so two concurrent handshakes cannot install the same
     * name and stop cannot acquire a transport after detaching the list. */
    bool published = false;
    xr_amutex_lock(&cluster->nodes_lock);
    if (atomic_load(&cluster->running)) {
        published = true;
        for (XrClusterNode *existing = cluster->nodes; existing; existing = existing->next) {
            if (strcmp(existing->name, node->name) == 0) {
                published = false;
                break;
            }
        }
        if (published) {
            node->next = cluster->nodes;
            cluster->nodes = node;
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
    XrValue generation = xr_int((int64_t) node->generation_token);
    const uint8_t arg_mode = XR_TRANSFER_SHARE;
    XrCoroutine *writer = runtime ? xr_coro_create_vm_closure_owned(
                                        X, outbound_handler, &generation, &arg_mode, 1,
                                        "cluster_writer", NULL, 0)
                                  : NULL;
    XrCoroutine *reader = writer ? xr_coro_create_vm_closure_owned(
                                       X, inbound_handler, &generation, &arg_mode, 1,
                                       "cluster_reader", NULL, 0)
                                 : NULL;
    if (!reader) {
        if (writer)
            xr_coro_destroy(writer);
        if (cluster_node_remove(cluster, node)) {
            cluster_node_shutdown(node);
            cluster_node_release(node);
        }
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    /* Source supplied both loops after admission. This leaf only publishes
     * their scheduler shells as one batch so neither transport direction can
     * monopolize the adopting worker's local run-next slot. */
    XrCoroutine *io_coros[2] = {writer, reader};
    xr_runtime_spawn_batch(runtime, io_coros, 2);
    cluster_runtime_release(cluster);
    return xr_bool(true);
}

// The pure-Xray public wrapper normalizes ClusterConfig into scalar values so
// both backends consume one representation-independent runtime boundary.
static XrValue cluster_start_primitive(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 10 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_STRING(args[2]) ||
        !XR_IS_BOOL(args[3]) || !XR_IS_STRING(args[4]) || !XR_IS_STRING(args[5]) ||
        !XR_IS_STRING(args[6]) || !XR_IS_BOOL(args[7]) || !XR_IS_INT(args[8]) ||
        !XR_IS_INT(args[9]))
        return xr_bool(false);

    int64_t packed_limits_value = XR_TO_INT(args[8]);
    uint64_t packed_limits = packed_limits_value > 0 ? (uint64_t) packed_limits_value : 0;
    uint64_t output_queue_high_watermark = packed_limits >> 32;
    uint32_t topic_fanout_max = (uint32_t) packed_limits;
    if (output_queue_high_watermark == 0 || output_queue_high_watermark > SIZE_MAX ||
        topic_fanout_max == 0 || XR_TO_INT(args[9]) <= 0)
        return xr_bool(false);

    /* Xray owns the validated configuration and start ordering. This leaf
     * materializes only resources that source cannot represent: TLS contexts,
     * locked registries and the isolate-local provider slot. */
    XrCluster *cluster = (XrCluster *) xr_calloc(1, sizeof(*cluster));
    if (!cluster)
        return xr_bool(false);
    atomic_store(&cluster->ref_count, 1);
    cluster->isolate = X;
    xr_amutex_init(&cluster->nodes_lock);

    if (XR_TO_BOOL(args[3])) {
        const char *ca_file = XR_TO_STRING(args[4])->data;
        const char *cert_file = XR_TO_STRING(args[5])->data;
        const char *key_file = XR_TO_STRING(args[6])->data;
        bool insecure = XR_TO_BOOL(args[7]);
        cluster->tls_client_ctx = xr_tls_context_new_client();
        if (!cluster->tls_client_ctx ||
            (ca_file[0] && xr_tls_context_load_ca(cluster->tls_client_ctx, ca_file) != 0)) {
            cluster_runtime_release(cluster);
            return xr_bool(false);
        }
        if (insecure)
            xr_tls_context_set_verify(cluster->tls_client_ctx, false);
        if (cert_file[0] && key_file[0]) {
            cluster->tls_server_ctx = xr_tls_context_new_server(cert_file, key_file);
            if (!cluster->tls_server_ctx) {
                cluster_runtime_release(cluster);
                return xr_bool(false);
            }
            if (!insecure && ca_file[0]) {
                if (xr_tls_context_load_ca(cluster->tls_server_ctx, ca_file) != 0) {
                    cluster_runtime_release(cluster);
                    return xr_bool(false);
                }
                xr_tls_context_set_verify(cluster->tls_server_ctx, true);
            }
        }
        cluster->tls_enabled = true;
    }

    cluster->topics = xr_topic_registry_new_vm(X, topic_fanout_max);
    cluster->monitors = xr_monitor_registry_new();
    cluster->tombstones = xr_tombstone_registry_new(16, XR_TO_INT(args[9]));
    if (!cluster->topics || !cluster->monitors || !cluster->tombstones) {
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    cluster->output_queue_high_watermark = (size_t) output_queue_high_watermark;
    atomic_store(&cluster->running, true);
    bool published =
        xr_isolate_provider_publish(X, &X->cluster, cluster, cluster_stop_fn);
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
    XrCluster *cluster =
        (XrCluster *) xr_isolate_provider_detach(X, &X->cluster, cluster_stop_fn);
    if (!cluster)
        return xr_null();
    atomic_store(&cluster->running, false);

    /* The source accept coroutine owns the listener. Closing the borrowed
     * provider alias only wakes its pending accept; the coroutine drops the
     * handle after observing the stopped generation. */
    xr_amutex_lock(&cluster->nodes_lock);
    XrNetListener *listener = cluster->listener;
    cluster->listener = NULL;
    XrClusterNode *node = cluster->nodes;
    cluster->nodes = NULL;
    xr_amutex_unlock(&cluster->nodes_lock);
    if (listener)
        xr_net_listener_close(listener);
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
        if (node->generation_token != generation || node->state != XR_NODE_CONNECTED)
            continue;
        node->last_heartbeat_recv = received_at_ms;
        node->missed_heartbeats = 0;
        if (rtt_ms >= 0)
            node->metrics.last_rtt_ms = rtt_ms;
        xr_phi_detector_record(&node->phi, received_at_ms);
        observed = true;
        break;
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    cluster_runtime_release(cluster);
    return xr_bool(observed);
}

static XrValue cluster_deliver_inbound_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_ARRAY(args[1]))
        return xr_null();
    XrArray *envelope = XR_TO_ARRAY(args[1]);
    if (envelope->elem_type != XR_ELEM_U8 || envelope->length < 0 ||
        (envelope->length > 0 && !envelope->data))
        return xr_null();
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_null();
    if (!atomic_load(&cluster->running)) {
        cluster_runtime_release(cluster);
        return xr_null();
    }
    (void) xr_topic_registry_deliver(cluster->topics, XR_TO_STRING(args[0])->data,
                                     (const uint8_t *) envelope->data,
                                     (uint32_t) envelope->length);
    cluster_runtime_release(cluster);
    return xr_null();
}

static XrValue cluster_broadcast_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_ARRAY(args[1]))
        return xr_null();
    XrArray *wire = XR_TO_ARRAY(args[1]);
    if (wire->elem_type != XR_ELEM_U8 || wire->length <= 0 || !wire->data)
        return xr_null();
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_null();
    if (!atomic_load(&cluster->running)) {
        cluster_runtime_release(cluster);
        return xr_null();
    }
    XrObjectInstance *result = xr_stdlib_record_new(X, "cluster", "__ClusterDeliveryStats");
    if (!result) {
        cluster_runtime_release(cluster);
        return xr_null();
    }
    XrClusterDeliveryStats stats = cluster_transport_broadcast(
        cluster, (uint64_t) XR_TO_INT(args[0]), (const uint8_t *) wire->data,
        (uint32_t) wire->length);
    xr_object_instance_set_by_key(X, result, "accepted", xr_int(stats.accepted));
    xr_object_instance_set_by_key(X, result, "full", xr_int(stats.full));
    xr_object_instance_set_by_key(X, result, "stopped", xr_int(stats.stopped));
    xr_object_instance_set_by_key(X, result, "resource", xr_int(stats.resource));
    cluster_runtime_release(cluster);
    return xr_object_instance_value(result);
}

static XrValue cluster_notify_remote_monitor_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 3 || !XR_IS_INT(args[0]) || !XR_IS_STRING(args[1]) || !XR_IS_STRING(args[2]))
        return xr_bool(false);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_bool(false);
    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    XrClusterNode *node = NULL;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *candidate = cluster->nodes; candidate; candidate = candidate->next) {
        if (candidate->generation_token == generation && candidate->state == XR_NODE_CONNECTED) {
            cluster_node_retain(candidate);
            node = candidate;
            break;
        }
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    if (!node) {
        cluster_runtime_release(cluster);
        return xr_bool(false);
    }
    xr_monitor_registry_notify_remote(cluster->monitors, cluster->isolate, node->name,
                                      XR_TO_STRING(args[1])->data, XR_TO_STRING(args[2])->data);
    cluster_node_release(node);
    cluster_runtime_release(cluster);
    return xr_bool(true);
}

static XrValue cluster_detach_peer_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_null();
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_null();
    uint64_t generation = (uint64_t) XR_TO_INT(args[0]);
    XrClusterNode *detached = NULL;
    XrValue name = xr_null();
    xr_amutex_lock(&cluster->nodes_lock);
    XrClusterNode **link = &cluster->nodes;
    while (*link) {
        XrClusterNode *candidate = *link;
        if (candidate->generation_token == generation) {
            name = xrs_string_value_c(X, candidate->name);
            if (!XR_IS_NULL(name)) {
                *link = candidate->next;
                candidate->next = NULL;
                detached = candidate;
            }
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
    return name;
}

static XrValue cluster_notify_node_down_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_null();
    xr_monitor_registry_notify_node(cluster->monitors, cluster->isolate,
                                    XR_TO_STRING(args[0])->data);
    cluster_runtime_release(cluster);
    return xr_null();
}

static XrValue cluster_publish_local_primitive(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]))
        return xr_null();
    const uint8_t *envelope = NULL;
    size_t envelope_len = 0;
    if (!xr_buffer_bytes(args[1], &envelope, &envelope_len) || envelope_len > UINT32_MAX)
        return xr_null();
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_null();
    if (!atomic_load(&cluster->running)) {
        cluster_runtime_release(cluster);
        return xr_null();
    }
    XrObjectInstance *result = xr_stdlib_record_new(X, "cluster", "__ClusterDeliveryStats");
    if (!result) {
        cluster_runtime_release(cluster);
        return xr_null();
    }
    XrString *topic = XR_TO_STRING(args[0]);
    XrTopicDeliveryStats stats =
        xr_topic_registry_deliver(cluster->topics, topic->data, envelope, (uint32_t) envelope_len);
    xr_object_instance_set_by_key(X, result, "accepted", xr_int(stats.accepted));
    xr_object_instance_set_by_key(X, result, "full", xr_int(stats.full));
    xr_object_instance_set_by_key(X, result, "stopped", xr_int(stats.stopped));
    xr_object_instance_set_by_key(X, result, "resource", xr_int(stats.resource));
    cluster_runtime_release(cluster);
    return xr_object_instance_value(result);
}

// xray binding: cluster.listen(pattern)
static XrValue cluster_listen_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]))
        return xr_null();

    /* listen() in cluster.xr rejects a capacity outside the bound before this
     * leaf is reached, so the bound is not restated here. */
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_null();
    XrString *pattern_str = XR_TO_STRING(args[0]);
    XrChannel *ch = xr_topic_registry_subscribe(cluster->topics, pattern_str->data,
                                                (uint32_t) XR_TO_INT(args[1]));
    if (!ch) {
        cluster_runtime_release(cluster);
        return xr_null();
    }
    cluster_runtime_release(cluster);
    return xr_value_from_channel(ch);
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
                XrString *nname = xr_string_intern(X, node->name, (uint32_t) strlen(node->name), 0);
                xr_object_instance_set_by_key(X, nj, "name", xr_string_value(nname));

                XrString *nhost = xr_string_intern(X, node->host, (uint32_t) strlen(node->host), 0);
                xr_object_instance_set_by_key(X, nj, "host", xr_string_value(nhost));
                xr_object_instance_set_by_key(X, nj, "port", xr_int(node->port));
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

                xr_object_instance_set_by_key(X, nj, "missedHeartbeats",
                                              xr_int((int64_t) node->missed_heartbeats));
                xr_object_instance_set_by_key(X, nj, "samples",
                                              xr_int((int64_t) node->phi.sample_count));
                xr_object_instance_set_by_key(X, nj, "mean", xr_float(node->phi.mean));
                xr_object_instance_set_by_key(X, nj, "variance", xr_float(node->phi.variance));
                xr_object_instance_set_by_key(X, nj, "detectorLastHeartbeatMs",
                                              xr_int(node->phi.last_heartbeat_ts));

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

    // Listener count is diagnostic and may be momentarily stale.
    xr_object_instance_set_by_key(X, info, "listeners", xr_int(xr_topic_registry_count(c->topics)));

    /*
     * Tombstone snapshot — number of nodes in the recently-dead
     * table. A non-zero value across successive calls means we have
     * peers that left the cluster within the source-selected retention window
     * and will be refused if they try to rejoin. Useful for correlating split
     * brain scenarios.
     */
    xr_object_instance_set_by_key(
        X, info, "deadNodes",
        xr_int(xr_tombstone_registry_count(c->tombstones, (int64_t) xr_time_monotonic_ms())));

    XrValue result = xr_object_instance_value(info);
    cluster_runtime_release(c);
    return result;
}

static XrValue cluster_register_node_monitor_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]) || !xr_value_is_channel(args[1]))
        return xr_bool(false);

    XrString *name = XR_TO_STRING(args[0]);
    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    bool registered = cluster && xr_monitor_registry_add_node(cluster->monitors, name->data,
                                                               xr_value_to_channel(args[1]));
    cluster_runtime_release(cluster);
    return xr_bool(registered);
}

static XrValue cluster_register_coro_monitor_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 3 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]) ||
        !xr_value_is_channel(args[2]))
        return xr_int(0);

    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_int(0);
    XrString *node_name = XR_TO_STRING(args[0]);
    uint64_t generation = 0;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *node = cluster->nodes; node; node = node->next) {
        if (node->state == XR_NODE_CONNECTED && strcmp(node->name, node_name->data) == 0) {
            generation = node->generation_token;
            break;
        }
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    if (generation == 0) {
        cluster_runtime_release(cluster);
        return xr_int(0);
    }

    XrString *coroutine_name = XR_TO_STRING(args[1]);
    if (!xr_monitor_registry_add_remote(cluster->monitors, node_name->data, coroutine_name->data,
                                        xr_value_to_channel(args[2]))) {
        cluster_runtime_release(cluster);
        return xr_int(0);
    }
    cluster_runtime_release(cluster);
    return xr_int((int64_t) generation);
}

static XrValue cluster_unregister_coro_monitor_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 3 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]) ||
        !xr_value_is_channel(args[2]))
        return xr_bool(false);

    XrCluster *cluster = NULL;
    XR_CLUSTER_RUNTIME_ACQUIRE(X, cluster);
    if (!cluster)
        return xr_bool(false);
    bool removed = xr_monitor_registry_remove_remote(
        cluster->monitors, XR_TO_STRING(args[0])->data, XR_TO_STRING(args[1])->data,
        xr_value_to_channel(args[2]));
    cluster_runtime_release(cluster);
    return xr_bool(removed);
}

#define XR_STDLIB_VM_BIND_MODULE_CLUSTER 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_CLUSTER
