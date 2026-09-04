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
#include "../crypto/crypto.h"  // xr_secure_wipe
#include "../../src/io/xnet_transport.h"
#include "../../src/io/xnet_handle.h"
#include "../../src/io/xnet_provider.h"
#include "../../src/io/xcluster_discovery_provider.h"
#include "../../src/runtime/object/xbuffer.h"
#include "../../src/module/xstdlib_runtime_cache.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/vm/xvm.h"
#include "../../src/base/xchecks.h"

#include <string.h>
#include <limits.h>

/* ========== Cluster Lifecycle ========== */

/*
 * Build the per-cluster TLS contexts from XrClusterTlsOptions.
 * Returns 0 on success. On failure any partially-allocated contexts are
 * freed and the corresponding XrCluster fields are left NULL.
 *
 * We keep the helper private to cluster.c because the resulting contexts
 * are owned by XrCluster; exposing creation would invite double-free
 * foot-guns from embedders. Callers get policy via XrClusterTlsOptions
 * instead of raw SSL_CTX * handles.
 */
static int build_cluster_tls(XrCluster *c, const XrClusterTlsOptions *opts) {
    // Client context backs the source-owned outbound join handshake.
    XrTlsContext *client_ctx = xr_tls_context_new_client();
    if (!client_ctx)
        return -1;

    if (opts->ca_file) {
        if (xr_tls_context_load_ca(client_ctx, opts->ca_file) != 0) {
            xr_tls_context_free(client_ctx);
            return -1;
        }
    }
    if (opts->insecure) {
        // Disable peer verification. Noisy on purpose — a failing mutual
        // auth rollout should be visible in the startup logs of every
        // affected node rather than silently downgrade.
        xr_tls_context_set_verify(client_ctx, false);
    }
    c->tls_client_ctx = client_ctx;

    // Server context is optional: only builds when cert+key supplied. Source
    // observes readiness before starting its accept loop, so an outbound-only
    // TLS node never silently downgrades inbound traffic.
    if (opts->cert_file && opts->key_file) {
        XrTlsContext *server_ctx = xr_tls_context_new_server(opts->cert_file, opts->key_file);
        if (!server_ctx) {
            xr_tls_context_free(client_ctx);
            c->tls_client_ctx = NULL;
            return -1;
        }
        // A server context that also verifies the peer cert yields
        // mutual TLS. Cluster's threat model makes peer-as-attacker
        // plausible (one compromised node), so default to verify on.
        if (!opts->insecure && opts->ca_file) {
            xr_tls_context_load_ca(server_ctx, opts->ca_file);
            xr_tls_context_set_verify(server_ctx, true);
        }
        c->tls_server_ctx = server_ctx;
    }

    c->tls_enabled = true;
    return 0;
}

static int cluster_runtime_open(XrVMRuntime *X, const char *name, uint16_t port, const char *secret,
                                const XrClusterTlsOptions *tls, int64_t heartbeat_interval_ms,
                                int64_t heartbeat_timeout_ms, int64_t max_missed_heartbeats,
                                int64_t phi_min_samples, double phi_threshold,
                                uint32_t topic_delivery_fanout_max,
                                int64_t tombstone_retention_ms) {
    if (X->cluster)
        return -1;  // already running
    if (!name)
        return -1;  // name required
    if (secret && strlen(secret) > XR_CLUSTER_SECRET_MAX)
        return -1;

    /* What counts as a legal node name is decided by validNodeName in
     * stdlib/cluster/cluster.xr, which both backends compile, so it is not
     * decided a second time here. The copy below is bounded either way. */

    XrCluster *c = (XrCluster *) xr_calloc(1, sizeof(XrCluster));
    if (!c)
        return -1;

    atomic_store(&c->ref_count, 1);
    atomic_store(&c->stop_started, false);
    strncpy(c->self_name, name, XR_NODE_NAME_MAX);
    c->self_name[XR_NODE_NAME_MAX] = '\0';
    c->listen_port = port;
    if (secret) {
        strncpy(c->secret, secret, sizeof(c->secret) - 1);
    }
    c->isolate = X;

    // TLS contexts must exist before the source accept loop starts. Failure is
    // fatal because an explicit TLS request must never become plaintext.
    if (tls && tls->enabled) {
        if (build_cluster_tls(c, tls) != 0) {
            xr_secure_wipe(c->secret, sizeof(c->secret));
            xr_free(c);
            return -1;
        }
    }

    xr_amutex_init(&c->nodes_lock);

    c->topics = xr_topic_registry_new_vm(X, topic_delivery_fanout_max);
    c->monitors = xr_monitor_registry_new();
    c->tombstones = xr_tombstone_registry_new(16, tombstone_retention_ms);
    if (!c->topics || !c->monitors || !c->tombstones) {
        if (c->tls_client_ctx)
            xr_tls_context_free(c->tls_client_ctx);
        if (c->tls_server_ctx)
            xr_tls_context_free(c->tls_server_ctx);
        xr_topic_registry_destroy(c->topics);
        xr_monitor_registry_destroy(c->monitors);
        xr_tombstone_registry_destroy(c->tombstones);
        xr_secure_wipe(c->secret, sizeof(c->secret));
        xr_free(c);
        return -1;
    }

    /* The health policy is cluster.xr's, not this file's: the schedule and phi
     * threshold are passed down so the numbers have one owner instead of a
     * default here and a constant there that can drift. */
    c->heartbeat_interval_ms = heartbeat_interval_ms;
    c->heartbeat_timeout_ms = heartbeat_timeout_ms;
    c->max_missed_heartbeats = max_missed_heartbeats;
    c->phi_min_samples = phi_min_samples;
    c->phi_threshold = phi_threshold;

    atomic_store(&c->running, true);
    X->cluster = c;
    return 0;
}

void cluster_runtime_retain(XrCluster *c) {
    XR_DCHECK(c != NULL, "cluster retain requires a runtime");
    if (c)
        atomic_fetch_add(&c->ref_count, 1);
}

static void cluster_runtime_destroy(XrCluster *c) {
    XR_DCHECK(c != NULL, "cluster destroy requires a runtime");
    XR_DCHECK(c->nodes == NULL, "cluster destroy requires a detached node list");
    XR_DCHECK(c->listener == NULL, "cluster destroy requires a detached listener");

    xr_topic_registry_destroy(c->topics);
    c->topics = NULL;
    xr_monitor_registry_destroy(c->monitors);
    c->monitors = NULL;
    xr_tombstone_registry_destroy(c->tombstones);
    c->tombstones = NULL;

    if (c->tls_client_ctx)
        xr_tls_context_free(c->tls_client_ctx);
    if (c->tls_server_ctx)
        xr_tls_context_free(c->tls_server_ctx);
    c->tls_client_ctx = NULL;
    c->tls_server_ctx = NULL;
    c->tls_enabled = false;

    xr_secure_wipe(c->secret, sizeof(c->secret));
    xr_free(c);
}

void cluster_runtime_release(XrCluster *c) {
    if (!c)
        return;
    uint32_t previous = atomic_fetch_sub(&c->ref_count, 1);
    XR_DCHECK(previous > 0, "cluster reference underflow");
    if (previous == 1)
        cluster_runtime_destroy(c);
}

static void cluster_runtime_close(XrCluster *c) {
    if (!c)
        return;
    if (atomic_exchange(&c->stop_started, true))
        return;

    atomic_store(&c->running, false);
    if (c->isolate && c->isolate->cluster == c)
        c->isolate->cluster = NULL;

    /* The source accept coroutine keeps this borrowed handle alive until it
     * observes the stopped generation. Closing it here wakes the pending
     * generic net.accept immediately; that coroutine owns the eventual drop. */
    if (c->listener) {
        xr_net_listener_close(c->listener);
        c->listener = NULL;
    }

    /* Detach the list atomically, then close nodes without holding
     * nodes_lock. Reader and writer coroutines own independent references;
     * their final release performs destruction after they return. */
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    c->nodes = NULL;
    c->node_count = 0;
    xr_amutex_unlock(&c->nodes_lock);
    while (node) {
        XrClusterNode *next = node->next;
        node->next = NULL;
        cluster_node_shutdown(node);
        cluster_node_release(node);
        node = next;
    }

    /* Release the isolate-owned reference. Peer transport coroutines keep the
     * runtime alive and the last one performs final destruction. */
    cluster_runtime_release(c);
}

/* ========== Node Management ========== */

XrClusterNode *cluster_node_find(XrCluster *c, const char *name) {
    if (!c)
        return NULL;
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (strcmp(node->name, name) == 0) {
            cluster_node_retain(node);
            xr_amutex_unlock(&c->nodes_lock);
            return node;
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);
    return NULL;
}

bool cluster_node_add(XrCluster *c, XrClusterNode *node) {
    if (!c || !node)
        return false;

    xr_amutex_lock(&c->nodes_lock);
    if (!atomic_load(&c->running)) {
        xr_amutex_unlock(&c->nodes_lock);
        return false;
    }
    for (XrClusterNode *existing = c->nodes; existing; existing = existing->next) {
        if (strcmp(existing->name, node->name) == 0) {
            xr_amutex_unlock(&c->nodes_lock);
            return false;
        }
    }
    node->next = c->nodes;
    c->nodes = node;
    c->node_count++;
    xr_amutex_unlock(&c->nodes_lock);
    return true;
}

bool cluster_node_remove(XrCluster *c, XrClusterNode *node) {
    if (!c || !node)
        return false;

    bool removed = false;
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode **pp = &c->nodes;
    while (*pp) {
        if (*pp == node) {
            *pp = node->next;
            node->next = NULL;
            c->node_count--;
            removed = true;
            break;
        }
        pp = &(*pp)->next;
    }
    xr_amutex_unlock(&c->nodes_lock);
    return removed;
}

/* ========== xray Function Bindings ========== */

static bool cluster_binding_text_fits(const XrString *text, size_t max_length, bool allow_empty) {
    if (!text || (!allow_empty && text->length == 0) || text->length > max_length)
        return false;
    return memchr(text->data, '\0', text->length) == NULL;
}

static XrValue cluster_recently_departed_fn(XrVMRuntime *X, XrValue *args, int argc) {
    XrCluster *cluster = (XrCluster *) X->cluster;
    if (!cluster || argc < 1 || !XR_IS_STRING(args[0]))
        return xr_bool(true);
    XrString *name = XR_TO_STRING(args[0]);
    if (!cluster_binding_text_fits(name, XR_NODE_NAME_MAX, false))
        return xr_bool(true);
    return xr_bool(
        xr_tombstone_registry_contains(cluster->tombstones, name->data, cluster_now_ms()));
}

static XrValue cluster_health_tick_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *cluster = (XrCluster *) X->cluster;
    if (!cluster || !atomic_load(&cluster->running))
        return xr_bool(false);
    cluster_health_tick(cluster);
    return xr_bool(true);
}

static XrValue cluster_track_listener_fn(XrVMRuntime *X, XrValue *args, int argc) {
    XrCluster *cluster = (XrCluster *) X->cluster;
    XrNetListener *listener = NULL;
    if (argc > 0 && XR_IS_PTR(args[0]) && XR_HEAP_TYPE(args[0]) == XR_TINSTANCE) {
        listener = (XrNetListener *) XR_VALUE_GCPTR(args[0]);
        if (!listener->klass || listener->klass->builtin_kind != XR_BK_NETLISTENER)
            listener = NULL;
    }
    if (!cluster || !listener || xr_net_listener_is_closed(listener) ||
        !atomic_load(&cluster->running) || cluster->listener)
        return xr_bool(false);
    cluster->listener = listener;
    return xr_bool(true);
}

static XrCFuncResult cluster_join_tls_fn(XrVMRuntime *X, XrValue *args, int argc, XrValue *result) {
    XrCluster *cluster = (XrCluster *) X->cluster;
#ifdef XR_ENABLE_TLS
    return xr_net_tls_handshake_with_context(
        X, args, argc, result, cluster && cluster->tls_enabled ? cluster->tls_client_ctx : NULL);
#else
    XrNetConn *conn = argc > 0 && args ? xr_net_conn_from_value(args[0]) : NULL;
    if (conn)
        xr_net_conn_close(conn);
    *result = xr_int(XR_NETERR_TLS);
    return XR_CFUNC_DONE;
#endif
}

static XrCFuncResult cluster_accept_tls_fn(XrVMRuntime *X, XrValue *args, int argc,
                                           XrValue *result) {
    XrCluster *cluster = (XrCluster *) X->cluster;
#ifdef XR_ENABLE_TLS
    return xr_net_tls_server_handshake_with_context(
        X, args, argc, result, cluster && cluster->tls_enabled ? cluster->tls_server_ctx : NULL);
#else
    XrNetConn *conn = argc > 0 && args ? xr_net_conn_from_value(args[0]) : NULL;
    if (conn)
        xr_net_conn_close(conn);
    *result = xr_int(XR_NETERR_TLS);
    return XR_CFUNC_DONE;
#endif
}

static XrValue cluster_adopt_peer_fn(XrVMRuntime *X, XrValue *args, int argc) {
    XrCluster *cluster = (XrCluster *) X->cluster;
    XrNetConn *handle = argc > 0 ? xr_net_conn_from_value(args[0]) : NULL;
    if (!cluster || argc < 5 || !handle || !XR_IS_STRING(args[1]) || !XR_IS_STRING(args[2]) ||
        !XR_IS_INT(args[3]) || !XR_IS_INT(args[4]) || !atomic_load(&cluster->running)) {
        if (handle)
            xr_net_conn_close(handle);
        return xr_bool(false);
    }

    XrString *name = XR_TO_STRING(args[1]);
    XrString *host = XR_TO_STRING(args[2]);
    int64_t port = XR_TO_INT(args[3]);
    int64_t flags = XR_TO_INT(args[4]);
    bool inbound = host->length == 0 && port == 0;
    if (!cluster_binding_text_fits(name, XR_NODE_NAME_MAX, false) ||
        !cluster_binding_text_fits(host, XR_ADDRESS_HOST_MAX, inbound) ||
        (!inbound && (port <= 0 || port > UINT16_MAX)) || flags < 0 ||
        (uint64_t) flags > UINT32_MAX) {
        xr_net_conn_close(handle);
        return xr_bool(false);
    }

    XrClusterNode *node = cluster_node_new(name->data, host->data, (uint16_t) port,
                                           (double) cluster->heartbeat_interval_ms);
    if (!node) {
        xr_net_conn_close(handle);
        return xr_bool(false);
    }
    node->conn = xr_io_conn_take_net_handle(handle, XR_CLUSTER_HANDSHAKE_TIMEOUT_MS);
    if (!node->conn) {
        cluster_node_release(node);
        xr_net_conn_close(handle);
        return xr_bool(false);
    }
    node->state = XR_NODE_CONNECTED;
    node->flags = (uint32_t) flags;
    node->last_heartbeat_recv = cluster_now_ms();

    if (!cluster_node_add(cluster, node)) {
        cluster_node_shutdown(node);
        cluster_node_release(node);
        return xr_bool(false);
    }
    if (!cluster_node_start_io(cluster, node)) {
        if (cluster_node_remove(cluster, node)) {
            cluster_node_shutdown(node);
            cluster_node_release(node);
        }
        return xr_bool(false);
    }
    return xr_bool(true);
}

// The pure-Xray public wrapper normalizes ClusterConfig into scalar values so
// both backends consume one representation-independent runtime boundary.
static XrValue cluster_start_primitive(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 15 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]) || !XR_IS_STRING(args[2]) ||
        !XR_IS_BOOL(args[3]) || !XR_IS_STRING(args[4]) || !XR_IS_STRING(args[5]) ||
        !XR_IS_STRING(args[6]) || !XR_IS_BOOL(args[7]) || !XR_IS_INT(args[8]) ||
        !XR_IS_INT(args[9]) || !XR_IS_INT(args[10]) || !XR_IS_INT(args[11]) ||
        !XR_IS_FLOAT(args[12]) || !XR_IS_INT(args[13]) || !XR_IS_INT(args[14]))
        return xr_bool(false);

    /* The port range is cluster.xr's rule, checked before this leaf is reached.
     * What remains here is the machine fact that the diagnostic field is
     * sixteen bits wide. */
    XrString *name = XR_TO_STRING(args[0]);
    XrString *secret_text = XR_TO_STRING(args[2]);
    if (!cluster_binding_text_fits(name, XR_NODE_NAME_MAX, false) ||
        !cluster_binding_text_fits(secret_text, XR_CLUSTER_SECRET_MAX, true))
        return xr_bool(false);
    uint16_t port = (uint16_t) XR_TO_INT(args[1]);
    const char *secret = secret_text->data;

    XrClusterTlsOptions tls_opts;
    memset(&tls_opts, 0, sizeof(tls_opts));
    const XrClusterTlsOptions *tls_ptr = NULL;
    if (XR_TO_BOOL(args[3])) {
        XrString *ca_text = XR_TO_STRING(args[4]);
        XrString *cert_text = XR_TO_STRING(args[5]);
        XrString *key_text = XR_TO_STRING(args[6]);
        if (!cluster_binding_text_fits(ca_text, SIZE_MAX, true) ||
            !cluster_binding_text_fits(cert_text, SIZE_MAX, true) ||
            !cluster_binding_text_fits(key_text, SIZE_MAX, true))
            return xr_bool(false);
        const char *ca_file = ca_text->data;
        const char *cert_file = cert_text->data;
        const char *key_file = key_text->data;
        tls_opts.enabled = true;
        tls_opts.ca_file = ca_file[0] ? ca_file : NULL;
        tls_opts.cert_file = cert_file[0] ? cert_file : NULL;
        tls_opts.key_file = key_file[0] ? key_file : NULL;
        tls_opts.insecure = XR_TO_BOOL(args[7]);
        tls_ptr = &tls_opts;
    }

    int64_t topic_fanout_max = XR_TO_INT(args[13]);
    if (XR_TO_INT(args[11]) <= 0 || topic_fanout_max <= 0 || topic_fanout_max > UINT32_MAX ||
        XR_TO_INT(args[14]) <= 0)
        return xr_bool(false);

    int rc = cluster_runtime_open(X, name->data, port, secret, tls_ptr, XR_TO_INT(args[8]),
                                  XR_TO_INT(args[9]), XR_TO_INT(args[10]), XR_TO_INT(args[11]),
                                  XR_TO_FLOAT(args[12]), (uint32_t) topic_fanout_max,
                                  XR_TO_INT(args[14]));
    return xr_bool(rc == 0);
}

// cluster.self() - returns node name
static XrValue cluster_self(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    const char *name = c ? c->self_name : "";
    XrString *str = xr_string_intern(X, name, (uint32_t) strlen(name), 0);
    return xr_string_value(str);
}

// cluster.nodes() - returns array of connected node names
static XrValue cluster_nodes(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c)
        return xr_null();

    XrArray *arr = xr_array_new(NULL);
    if (!arr)
        return xr_null();

    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        XrString *name = xr_string_intern(X, node->name, (uint32_t) strlen(node->name), 0);
        xr_array_push(arr, xr_string_value(name));
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);

    return xr_value_from_array(arr);
}

// cluster.stop()
static XrValue cluster_stop_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    cluster_runtime_close((XrCluster *) X->cluster);
    return xr_null();
}

/* ========== Frame Processing ========== */

void cluster_process_frame(XrCluster *c, XrClusterNode *node, uint8_t frame_type,
                           const uint8_t *payload, uint32_t payload_len) {
    XR_DCHECK(c != NULL, "cluster must be initialized");
    XR_DCHECK(node != NULL, "node must not be NULL");
    if (!c || !node)
        return;

    XrClusterFrameProjection projection;
    if (!cluster_frame_project(frame_type, payload, payload_len, &projection))
        return;

    switch (projection.kind) {
        case XR_CLUSTER_FRAME_HEARTBEAT_PING: {
            (void) cluster_node_enqueue(node, projection.response, projection.response_length);
            int64_t now_hb = cluster_now_ms();
            node->last_heartbeat_recv = now_hb;
            node->missed_heartbeats = 0;
            xr_phi_detector_record(&node->phi, now_hb);
            break;
        }

        case XR_CLUSTER_FRAME_HEARTBEAT_PONG: {
            int64_t now_pong = cluster_now_ms();
            node->metrics.last_rtt_ms = now_pong - projection.heartbeat_timestamp;
            node->last_heartbeat_recv = now_pong;
            node->missed_heartbeats = 0;
            xr_phi_detector_record(&node->phi, now_pong);
            break;
        }

        case XR_CLUSTER_FRAME_TRANSPORT: {
            cluster_transport_handle_frame(
                c, node, projection.transport.topic, projection.transport.envelope,
                projection.transport.envelope_length, projection.transport.hop_limit);
            break;
        }

        case XR_CLUSTER_FRAME_CORO_MONITOR: {
            cluster_monitor_handle_coro_request(c, node, projection.coro_name);
            break;
        }

        case XR_CLUSTER_FRAME_CORO_EXIT: {
            xr_monitor_registry_notify_remote(c->monitors, c->isolate, node->name,
                                              projection.coro_name, projection.coro_reason);
            break;
        }

        case XR_CLUSTER_FRAME_CORO_DEMONITOR:
            // Future: remove remote monitor
            break;

        default:
            break;
    }
}

static XrValue cluster_publish_local_primitive(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]))
        return xr_int(XR_CLUSTER_DELIVERY_INVALID_TOPIC);
    const uint8_t *envelope = NULL;
    size_t envelope_len = 0;
    if (!xr_buffer_bytes(args[1], &envelope, &envelope_len) || envelope_len > UINT32_MAX)
        return xr_int(XR_CLUSTER_DELIVERY_INVALID_ENVELOPE);
    XrCluster *cluster = (XrCluster *) X->cluster;
    if (!cluster || !atomic_load(&cluster->running))
        return xr_int(XR_CLUSTER_DELIVERY_UNAVAILABLE);
    XrString *topic = XR_TO_STRING(args[0]);
    return xr_int((int64_t) xr_topic_registry_deliver(cluster->topics, topic->data, envelope,
                                                      (uint32_t) envelope_len));
}

static XrValue cluster_publish_remote_primitive(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 3 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[2]))
        return xr_int(XR_CLUSTER_DELIVERY_INVALID_TOPIC);
    const uint8_t *envelope = NULL;
    size_t envelope_len = 0;
    XrString *topic = XR_TO_STRING(args[0]);
    if (topic->length == 0 || topic->length > XR_TOPIC_PATTERN_MAX ||
        !xr_buffer_bytes(args[1], &envelope, &envelope_len) || envelope_len > UINT32_MAX ||
        envelope_len > XR_FRAME_MAX_PAYLOAD - 2u - topic->length)
        return xr_int(XR_CLUSTER_DELIVERY_INVALID_ENVELOPE);
    XrCluster *cluster = (XrCluster *) X->cluster;
    if (!cluster || !atomic_load(&cluster->running))
        return xr_int(XR_CLUSTER_DELIVERY_UNAVAILABLE);
    return xr_int((int64_t) cluster_transport_broadcast(cluster, NULL, (uint8_t) XR_TO_INT(args[2]),
                                                        topic->data, envelope,
                                                        (uint32_t) envelope_len));
}

// xray binding: cluster.listen(pattern)
static XrValue cluster_listen_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_INT(args[1]))
        return xr_null();

    /* listen() in cluster.xr rejects a capacity outside the bound before this
     * leaf is reached, so the bound is not restated here. */
    XrCluster *cluster = (XrCluster *) X->cluster;
    if (!cluster)
        return xr_null();
    XrString *pattern_str = XR_TO_STRING(args[0]);
    XrChannel *ch = xr_topic_registry_subscribe(cluster->topics, pattern_str->data,
                                                (uint32_t) XR_TO_INT(args[1]));
    if (!ch)
        return xr_null();
    return xr_value_from_channel(ch);
}

/* ========== Cluster Info API ========== */

static XrObjectInstance *cluster_object_new(XrVMRuntime *X, const char *name) {
    XrClass *cls = xr_stdlib_record_class_get(X, "cluster", name);
    return cls ? xr_object_instance_new_with_class(NULL, cls) : NULL;
}

static XrValue cluster_handshake_config_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *cluster = (XrCluster *) X->cluster;
    if (!cluster || !atomic_load(&cluster->running))
        return xr_null();
    XrObjectInstance *config = cluster_object_new(X, "__ClusterHandshakeConfig");
    if (!config)
        return xr_null();
    XrString *secret = xr_string_intern(X, cluster->secret, (uint32_t) strlen(cluster->secret), 0);
    xr_object_instance_set_by_key(X, config, "secret", xr_string_value(secret));
    xr_object_instance_set_by_key(X, config, "tlsEnabled", xr_bool(cluster->tls_enabled));
    xr_object_instance_set_by_key(X, config, "tlsServerReady",
                                  xr_bool(cluster->tls_server_ctx != NULL));
    return xr_object_instance_value(config);
}

static XrValue cluster_info_fn(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrCluster *c = (XrCluster *) X->cluster;
    if (!c)
        return xr_null();

    XrObjectInstance *info = cluster_object_new(X, "__ClusterSnapshot");
    if (!info)
        return xr_null();

    // Self name
    XrString *self = xr_string_intern(X, c->self_name, (uint32_t) strlen(c->self_name), 0);
    xr_object_instance_set_by_key(X, info, "self", xr_string_value(self));
    xr_object_instance_set_by_key(X, info, "port", xr_int(c->listen_port));
    xr_object_instance_set_by_key(X, info, "running", xr_bool(atomic_load(&c->running)));

    // Node list with metrics
    XrArray *node_arr = xr_array_new(NULL);
    if (node_arr) {
        xr_amutex_lock(&c->nodes_lock);
        XrClusterNode *node = c->nodes;
        while (node) {
            XrObjectInstance *nj = cluster_object_new(X, "__ClusterNodeSnapshot");
            if (nj) {
                XrString *nname = xr_string_intern(X, node->name, (uint32_t) strlen(node->name), 0);
                xr_object_instance_set_by_key(X, nj, "name", xr_string_value(nname));

                XrString *nhost = xr_string_intern(X, node->host, (uint32_t) strlen(node->host), 0);
                xr_object_instance_set_by_key(X, nj, "host", xr_string_value(nhost));
                xr_object_instance_set_by_key(X, nj, "port", xr_int(node->port));
                xr_object_instance_set_by_key(X, nj, "state", xr_int((int64_t) node->state));

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
                // high watermark (4 MiB by default) since start. Each
                // event corresponds to one outq_bytes >= high_watermark
                // transition in cluster_node.
                xr_object_instance_set_by_key(
                    X, nj, "slowConsumerEvents",
                    xr_int((int64_t) atomic_load(&node->metrics.slow_consumer_events)));
                xr_object_instance_set_by_key(X, nj, "rttMs", xr_int(node->metrics.last_rtt_ms));
                xr_object_instance_set_by_key(X, nj, "outQueueBytes",
                                              xr_int(node->outq.total_bytes));
                xr_object_instance_set_by_key(X, nj, "outQueueFrames",
                                              xr_int(node->outq.frame_count));
                xr_object_instance_set_by_key(X, nj, "slow", xr_bool(cluster_node_is_slow(node)));

                // Phi accrual failure-detector score. Higher = more
                // likely dead. Threshold for "kill" is set by
                // cluster policy in cluster_health.c.
                int64_t now = cluster_now_ms();
                double phi = xr_phi_detector_value(&node->phi, now);
                xr_object_instance_set_by_key(X, nj, "phi", xr_float(phi));
                xr_object_instance_set_by_key(X, nj, "missedHeartbeats",
                                              xr_int((int64_t) node->missed_heartbeats));

                xr_array_push(node_arr, xr_object_instance_value(nj));
            } else {
                xr_amutex_unlock(&c->nodes_lock);
                return xr_null();
            }
            node = node->next;
        }
        xr_amutex_unlock(&c->nodes_lock);
        xr_object_instance_set_by_key(X, info, "nodes", xr_value_from_array(node_arr));
    } else {
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
        X, info, "deadNodes", xr_int(xr_tombstone_registry_count(c->tombstones, cluster_now_ms())));

    /*
     * Expose the operator-configurable heartbeat knobs so ops can
     * sanity-check the live cluster against their YAML without
     * shelling into the node. These fields are rarely changed at
     * runtime but live at the XrCluster level so a snapshot is
     * trivially consistent.
     */
    xr_object_instance_set_by_key(X, info, "heartbeatIntervalMs", xr_int(c->heartbeat_interval_ms));
    xr_object_instance_set_by_key(X, info, "heartbeatTimeoutMs", xr_int(c->heartbeat_timeout_ms));
    xr_object_instance_set_by_key(X, info, "maxMissedHeartbeats", xr_int(c->max_missed_heartbeats));

    /*
     * TLS posture is a typed nested object. A mis-configured cluster
     * (enabled with neither context ready) remains directly visible to
     * operators without exposing a bitmap convention in the public API.
     */
    XrObjectInstance *tls = cluster_object_new(X, "__ClusterTlsSnapshot");
    if (!tls)
        return xr_null();
    xr_object_instance_set_by_key(X, tls, "enabled", xr_bool(c->tls_enabled));
    xr_object_instance_set_by_key(X, tls, "clientReady", xr_bool(c->tls_client_ctx != NULL));
    xr_object_instance_set_by_key(X, tls, "serverReady", xr_bool(c->tls_server_ctx != NULL));
    xr_object_instance_set_by_key(X, info, "tls", xr_object_instance_value(tls));

    return xr_object_instance_value(info);
}

static XrValue cluster_register_node_monitor_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]) || !xr_value_is_channel(args[1]))
        return xr_bool(false);

    XrString *name = XR_TO_STRING(args[0]);
    if (!cluster_binding_text_fits(name, XR_NODE_NAME_MAX, false))
        return xr_bool(false);
    XrCluster *cluster = (XrCluster *) X->cluster;
    return xr_bool(cluster && xr_monitor_registry_add_node(cluster->monitors, name->data,
                                                           xr_value_to_channel(args[1])));
}

static XrValue cluster_register_coro_monitor_fn(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 3 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]) ||
        !xr_value_is_channel(args[2]))
        return xr_bool(false);

    XrString *node = XR_TO_STRING(args[0]);
    XrString *coro = XR_TO_STRING(args[1]);
    if (!cluster_binding_text_fits(node, XR_NODE_NAME_MAX, false) ||
        !cluster_binding_text_fits(coro, XR_CORO_NAME_MAX, false))
        return xr_bool(false);
    return xr_bool(cluster_monitor_register_remote((XrCluster *) X->cluster, node->data, coro->data,
                                                   xr_value_to_channel(args[2])));
}

#define XR_STDLIB_VM_BIND_MODULE_CLUSTER 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_CLUSTER
