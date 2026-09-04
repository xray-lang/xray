/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_aot.c - Standalone AOT adapter for the cluster transport
 *
 * KEY CONCEPT:
 *   The adapter owns sockets and blocking I/O threads, while protocol frames,
 *   authentication, topic rules, Channel transport and Buffer ownership use
 *   the same backend-neutral kernels as the VM runtime. No VM is embedded.
 */

#include "cluster_internal.h"
#include "../../src/aot/xrt_cluster.h"
#include "../../src/base/xchecks.h"
#include "../../src/coro/xaot_coro.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xtopic_registry.h"
#include "../../src/io/xcluster_blocking.h"
#include "../../src/os/os_net.h"
#include "../../src/runtime/core/xr_runtime_core.h"

#include <stdio.h>
#include <string.h>

#define XR_AOT_CLUSTER_QUEUE_HIGH_WATERMARK (4u * 1024u * 1024u)
#define XR_AOT_CLUSTER_ACCEPT_POLL_MS 100

typedef struct XrAotClusterFrame {
    uint8_t *data;
    uint32_t length;
    struct XrAotClusterFrame *next;
} XrAotClusterFrame;

typedef struct XrAotClusterState XrAotClusterState;

typedef struct XrAotClusterNode {
    XrAotClusterState *cluster;
    char name[XR_NODE_NAME_MAX + 1];
    xr_socket_t socket;
    _Atomic(bool) running;
    xr_mutex_t queue_lock;
    xr_cond_t queue_ready;
    XrAotClusterFrame *queue_head;
    XrAotClusterFrame *queue_tail;
    size_t queue_bytes;
    xr_thread_t reader_thread;
    xr_thread_t writer_thread;
    bool reader_started;
    bool writer_started;
    struct XrAotClusterNode *next;
} XrAotClusterNode;

struct XrAotClusterState {
    XrAotRuntime *runtime;
    const XrAotValueOps *values;
    char self_name[XR_NODE_NAME_MAX + 1];
    char secret[XR_CLUSTER_SECRET_MAX + 1];
    uint16_t listen_port;
    /* The heartbeat schedule cluster.xr decided for this node. The AOT adapter
     * accepts the schedule but has no heartbeat coroutine to run it yet; only
     * the VM transport in stdlib/cluster/cluster.c drives heartbeats today.
     * These fields exist so the schedule keeps a single owner in cluster.xr:
     * when AOT grows its own heartbeat it reads the numbers from here, with no
     * further change to the leaf signature. */
    int64_t heartbeat_interval_ms;
    int64_t heartbeat_timeout_ms;
    int64_t max_missed_heartbeats;
    double phi_threshold;
    xr_socket_t listen_socket;
    _Atomic(bool) running;
    xr_thread_t accept_thread;
    bool accept_started;
    xr_mutex_t nodes_lock;
    XrAotClusterNode *nodes;
    XrTopicRegistry *topics;
};

static bool aot_cluster_copy_text(char *target, size_t capacity, const char *source, int64_t length,
                                  bool allow_empty) {
    if (!target || capacity == 0 || !source || length < 0 || (size_t) length >= capacity ||
        (!allow_empty && length == 0) || memchr(source, '\0', (size_t) length))
        return false;
    memcpy(target, source, (size_t) length);
    target[length] = '\0';
    return true;
}

static void aot_cluster_node_stop(XrAotClusterNode *node) {
    if (!node || !atomic_exchange_explicit(&node->running, false, memory_order_acq_rel))
        return;
    shutdown(node->socket, XR_SHUT_RDWR);
    xr_mutex_lock(&node->queue_lock);
    xr_cond_broadcast(&node->queue_ready);
    xr_mutex_unlock(&node->queue_lock);
}

static XrAotClusterNode *aot_cluster_node_new(XrAotClusterState *cluster, const char *name,
                                              xr_socket_t socket) {
    XrAotClusterNode *node = (XrAotClusterNode *) xr_calloc(1, sizeof(*node));
    if (!node)
        return NULL;
    node->cluster = cluster;
    node->socket = socket;
    strncpy(node->name, name, XR_NODE_NAME_MAX);
    node->name[XR_NODE_NAME_MAX] = '\0';
    atomic_store_explicit(&node->running, true, memory_order_relaxed);
    xr_mutex_init(&node->queue_lock);
    xr_cond_init(&node->queue_ready);
    (void) xr_socket_set_nodelay(socket, true);
    return node;
}

static void aot_cluster_frame_free(XrAotClusterFrame *frame) {
    if (!frame)
        return;
    xr_free(frame->data);
    xr_free(frame);
}

static void aot_cluster_node_destroy(XrAotClusterNode *node) {
    if (!node)
        return;
    aot_cluster_node_stop(node);
    if (node->writer_started)
        (void) xr_thread_join(node->writer_thread, NULL);
    if (node->reader_started)
        (void) xr_thread_join(node->reader_thread, NULL);
    xr_closesocket(node->socket);
    XrAotClusterFrame *frame = node->queue_head;
    while (frame) {
        XrAotClusterFrame *next = frame->next;
        aot_cluster_frame_free(frame);
        frame = next;
    }
    xr_cond_destroy(&node->queue_ready);
    xr_mutex_destroy(&node->queue_lock);
    xr_free(node);
}

static int aot_cluster_node_enqueue_ready_frame(XrAotClusterNode *node, XrAotClusterFrame *frame) {
    xr_mutex_lock(&node->queue_lock);
    if (!atomic_load_explicit(&node->running, memory_order_relaxed) ||
        node->queue_bytes + frame->length > XR_AOT_CLUSTER_QUEUE_HIGH_WATERMARK) {
        xr_mutex_unlock(&node->queue_lock);
        aot_cluster_frame_free(frame);
        return -1;
    }
    if (node->queue_tail)
        node->queue_tail->next = frame;
    else
        node->queue_head = frame;
    node->queue_tail = frame;
    node->queue_bytes += frame->length;
    xr_cond_signal(&node->queue_ready);
    xr_mutex_unlock(&node->queue_lock);
    return 0;
}

static int aot_cluster_node_enqueue(XrAotClusterNode *node, uint8_t frame_type,
                                    const uint8_t *payload, uint32_t payload_length) {
    if (!node || !atomic_load_explicit(&node->running, memory_order_acquire))
        return -1;
    size_t frame_length = (size_t) payload_length + XR_FRAME_HEADER_SIZE + 1;
    XrAotClusterFrame *frame = (XrAotClusterFrame *) xr_calloc(1, sizeof(*frame));
    if (!frame)
        return -1;
    frame->data = (uint8_t *) xr_malloc(frame_length);
    if (!frame->data) {
        xr_free(frame);
        return -1;
    }
    frame->length =
        (uint32_t) cluster_frame_write(frame->data, frame_type, payload, payload_length);
    return aot_cluster_node_enqueue_ready_frame(node, frame);
}

static int aot_cluster_node_enqueue_transport(XrAotClusterNode *node, uint8_t hop_limit,
                                              const char *topic, uint8_t topic_len,
                                              const uint8_t *envelope, uint32_t envelope_length) {
    if (!node || !atomic_load_explicit(&node->running, memory_order_acquire))
        return -1;
    size_t frame_length = (size_t) XR_FRAME_HEADER_SIZE + 3u + topic_len + envelope_length;
    if (frame_length > UINT32_MAX)
        return -1;
    XrAotClusterFrame *frame = (XrAotClusterFrame *) xr_calloc(1, sizeof(*frame));
    if (!frame)
        return -1;
    frame->data = (uint8_t *) xr_malloc(frame_length);
    if (!frame->data) {
        xr_free(frame);
        return -1;
    }
    int wrote = cluster_frame_write_transport(frame->data, frame_length, hop_limit, topic,
                                              topic_len, envelope, envelope_length);
    if (wrote < 0) {
        aot_cluster_frame_free(frame);
        return -1;
    }
    frame->length = (uint32_t) wrote;
    return aot_cluster_node_enqueue_ready_frame(node, frame);
}

static XrClusterDelivery aot_cluster_broadcast(XrAotClusterState *cluster,
                                               XrAotClusterNode *excluded_node, uint8_t hop_limit,
                                               const char *topic, uint8_t topic_length,
                                               const uint8_t *envelope, uint32_t envelope_length) {
    int connected = 0;
    int accepted = 0;
    xr_mutex_lock(&cluster->nodes_lock);
    for (XrAotClusterNode *node = cluster->nodes; node; node = node->next) {
        if (node == excluded_node || !atomic_load_explicit(&node->running, memory_order_acquire))
            continue;
        connected++;
        if (aot_cluster_node_enqueue_transport(node, hop_limit, topic, topic_length, envelope,
                                               envelope_length) == 0)
            accepted++;
    }
    xr_mutex_unlock(&cluster->nodes_lock);
    if (accepted > 0)
        return XR_CLUSTER_DELIVERY_ACCEPTED;
    if (connected > 0)
        return XR_CLUSTER_DELIVERY_OVERLOADED;
    return XR_CLUSTER_DELIVERY_DISCONNECTED;
}

static XrAotClusterFrame *aot_cluster_node_take_frame(XrAotClusterNode *node) {
    xr_mutex_lock(&node->queue_lock);
    while (atomic_load_explicit(&node->running, memory_order_relaxed) && !node->queue_head)
        xr_cond_wait(&node->queue_ready, &node->queue_lock);
    XrAotClusterFrame *frame = node->queue_head;
    if (frame) {
        node->queue_head = frame->next;
        if (!node->queue_head)
            node->queue_tail = NULL;
        node->queue_bytes -= frame->length;
        frame->next = NULL;
    }
    xr_mutex_unlock(&node->queue_lock);
    return frame;
}

static void *aot_cluster_writer_main(void *argument) {
    XrAotClusterNode *node = (XrAotClusterNode *) argument;
    while (atomic_load_explicit(&node->running, memory_order_acquire)) {
        XrAotClusterFrame *frame = aot_cluster_node_take_frame(node);
        if (!frame)
            break;
        bool written = xr_cluster_blocking_write_all(node->socket, frame->data, frame->length,
                                                     XR_CLUSTER_HANDSHAKE_TIMEOUT_MS);
        aot_cluster_frame_free(frame);
        if (!written)
            break;
    }
    aot_cluster_node_stop(node);
    return NULL;
}

static void aot_cluster_process_frame(XrAotClusterNode *node, uint8_t frame_type,
                                      const uint8_t *payload, uint32_t payload_length) {
    if (frame_type == XR_FRAME_TRANSPORT_ENVELOPE) {
        XrFrameTransport transport;
        if (cluster_frame_decode_transport(payload, payload_length, &transport) != 0 ||
            !xr_topic_name_valid(transport.topic))
            return;
        (void) xr_topic_registry_deliver(node->cluster->topics, transport.topic, transport.envelope,
                                         transport.envelope_length);
        if (transport.hop_limit > 0)
            (void) aot_cluster_broadcast(node->cluster, node, (uint8_t) (transport.hop_limit - 1),
                                         transport.topic, transport.topic_length,
                                         transport.envelope, transport.envelope_length);
        return;
    }
    if (frame_type == XR_FRAME_HEARTBEAT_PING) {
        int64_t timestamp = 0;
        uint8_t pong[32];
        if (cluster_frame_decode_heartbeat(payload, payload_length, &timestamp) == 0) {
            int length = cluster_frame_encode_heartbeat(pong, sizeof(pong), XR_FRAME_HEARTBEAT_PONG,
                                                        timestamp);
            if (length > (XR_FRAME_HEADER_SIZE + 1))
                (void) aot_cluster_node_enqueue(node, XR_FRAME_HEARTBEAT_PONG,
                                                pong + XR_FRAME_HEADER_SIZE + 1,
                                                (uint32_t) length - XR_FRAME_HEADER_SIZE - 1);
        }
    }
}

static void *aot_cluster_reader_main(void *argument) {
    XrAotClusterNode *node = (XrAotClusterNode *) argument;
    while (atomic_load_explicit(&node->running, memory_order_acquire)) {
        uint8_t frame_type = 0;
        uint8_t *payload = NULL;
        uint32_t payload_length = 0;
        if (!xr_cluster_blocking_read_frame(node->socket, &frame_type, &payload, &payload_length,
                                            XR_CLUSTER_HANDSHAKE_TIMEOUT_MS))
            break;
        aot_cluster_process_frame(node, frame_type, payload, payload_length);
        xr_free(payload);
    }
    aot_cluster_node_stop(node);
    return NULL;
}

static bool aot_cluster_node_start(XrAotClusterNode *node) {
    if (!node || !xr_thread_create(&node->writer_thread, aot_cluster_writer_main, node))
        return false;
    node->writer_started = true;
    if (!xr_thread_create(&node->reader_thread, aot_cluster_reader_main, node)) {
        aot_cluster_node_stop(node);
        (void) xr_thread_join(node->writer_thread, NULL);
        node->writer_started = false;
        return false;
    }
    node->reader_started = true;
    return true;
}

static void aot_cluster_add_node(XrAotClusterState *cluster, XrAotClusterNode *node) {
    xr_mutex_lock(&cluster->nodes_lock);
    node->next = cluster->nodes;
    cluster->nodes = node;
    xr_mutex_unlock(&cluster->nodes_lock);
}

static void *aot_cluster_accept_main(void *argument) {
    XrAotClusterState *cluster = (XrAotClusterState *) argument;
    while (atomic_load_explicit(&cluster->running, memory_order_acquire)) {
        /* Closing a listening socket from another thread does not reliably
         * interrupt a blocking accept() on every supported OS. Poll with a
         * bounded timeout so stop can flip running and join this thread before
         * it closes the descriptor, avoiding both shutdown hangs and fd reuse. */
        if (xr_cluster_blocking_wait(cluster->listen_socket, true, XR_AOT_CLUSTER_ACCEPT_POLL_MS) !=
            0)
            continue;
        if (!atomic_load_explicit(&cluster->running, memory_order_acquire))
            break;
        xr_socket_t socket = accept(cluster->listen_socket, NULL, NULL);
        if (socket == XR_INVALID_SOCKET)
            break;
        char peer_name[XR_NODE_NAME_MAX + 1] = {0};
        if (!xr_cluster_blocking_server_handshake(socket, cluster->self_name, cluster->secret, 0x01,
                                                  peer_name) ||
            !atomic_load_explicit(&cluster->running, memory_order_acquire)) {
            xr_closesocket(socket);
            continue;
        }
        XrAotClusterNode *node = aot_cluster_node_new(cluster, peer_name, socket);
        if (!node || !aot_cluster_node_start(node)) {
            if (node)
                aot_cluster_node_destroy(node);
            else
                xr_closesocket(socket);
            continue;
        }
        aot_cluster_add_node(cluster, node);
    }
    return NULL;
}

static xr_socket_t aot_cluster_listen_socket(uint16_t port, uint16_t *actual_port) {
    xr_socket_t fd = (xr_socket_t) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == XR_INVALID_SOCKET)
        return XR_INVALID_SOCKET;
    (void) xr_socket_set_reuseaddr(fd, true);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) != 0 || listen(fd, 128) != 0) {
        xr_closesocket(fd);
        return XR_INVALID_SOCKET;
    }
    socklen_t length = (socklen_t) sizeof(address);
    if (getsockname(fd, (struct sockaddr *) &address, &length) != 0) {
        xr_closesocket(fd);
        return XR_INVALID_SOCKET;
    }
    *actual_port = ntohs(address.sin_port);
    return fd;
}

/* Address syntax has a single owner in cluster.xr's parseAddress, so this
 * adapter receives a host and an already parsed port number. Rendering that
 * number back to decimal here is deliberate rather than incidental: the earlier
 * code handed getaddrinfo(3) the raw text that followed the colon, and
 * getaddrinfo also resolves service names, so cluster.join("db:http") silently
 * connected to port 80 under AOT while the VM answered false for the same
 * address. A decimal string built from an integer cannot name a service. */
static xr_socket_t aot_cluster_connect(const char *host, uint16_t port) {
    char service[8];
    if (snprintf(service, sizeof(service), "%u", (unsigned) port) < 0)
        return XR_INVALID_SOCKET;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *resolved = NULL;
    if (getaddrinfo(host, service, &hints, &resolved) != 0)
        return XR_INVALID_SOCKET;
    xr_socket_t connected = XR_INVALID_SOCKET;
    for (struct addrinfo *candidate = resolved; candidate; candidate = candidate->ai_next) {
        xr_socket_t fd = (xr_socket_t) socket(candidate->ai_family, candidate->ai_socktype,
                                              candidate->ai_protocol);
        if (fd == XR_INVALID_SOCKET)
            continue;
        if (connect(fd, candidate->ai_addr, (socklen_t) candidate->ai_addrlen) == 0) {
            connected = fd;
            break;
        }
        xr_closesocket(fd);
    }
    freeaddrinfo(resolved);
    return connected;
}

static XrAotClusterState *aot_cluster_acquire(XrAotRuntime **runtime_out) {
    XrAotRuntime *runtime = xr_aot_runtime_current();
    if (runtime_out)
        *runtime_out = runtime;
    return runtime ? (XrAotClusterState *) xr_aot_runtime_service_acquire(
                         runtime, XR_AOT_SERVICE_SLOT_CLUSTER)
                   : NULL;
}

static void aot_cluster_release(XrAotRuntime *runtime) {
    if (runtime)
        xr_aot_runtime_service_release(runtime, XR_AOT_SERVICE_SLOT_CLUSTER);
}

static void aot_cluster_state_destroy(XrAotClusterState *cluster) {
    if (!cluster)
        return;
    atomic_store_explicit(&cluster->running, false, memory_order_release);
    if (cluster->accept_started)
        (void) xr_thread_join(cluster->accept_thread, NULL);
    if (cluster->listen_socket != XR_INVALID_SOCKET) {
        shutdown(cluster->listen_socket, XR_SHUT_RDWR);
        xr_closesocket(cluster->listen_socket);
        cluster->listen_socket = XR_INVALID_SOCKET;
    }

    xr_mutex_lock(&cluster->nodes_lock);
    for (XrAotClusterNode *node = cluster->nodes; node; node = node->next)
        aot_cluster_node_stop(node);
    XrAotClusterNode *nodes = cluster->nodes;
    cluster->nodes = NULL;
    xr_mutex_unlock(&cluster->nodes_lock);
    while (nodes) {
        XrAotClusterNode *next = nodes->next;
        aot_cluster_node_destroy(nodes);
        nodes = next;
    }

    xr_topic_registry_destroy(cluster->topics);
    xr_mutex_destroy(&cluster->nodes_lock);
    memset(cluster->secret, 0, sizeof(cluster->secret));
    xr_free(cluster);
}

static void aot_cluster_service_destroy(void *service) {
    aot_cluster_state_destroy((XrAotClusterState *) service);
}

XrValue xrt_cluster_start(const char *name, int64_t name_len, XrValue port_value,
                          const char *secret, int64_t secret_len, XrValue tls_enabled,
                          const char *ca_file, int64_t ca_file_len, const char *cert_file,
                          int64_t cert_file_len, const char *key_file, int64_t key_file_len,
                          XrValue insecure, XrValue heartbeat_interval_ms,
                          XrValue heartbeat_timeout_ms, XrValue max_missed_heartbeats,
                          XrValue heartbeat_tick_ms, XrValue phi_min_samples, XrValue phi_threshold,
                          XrValue topic_delivery_fanout_max, XrValue tombstone_retention_ms) {
    (void) ca_file;
    (void) ca_file_len;
    (void) cert_file;
    (void) cert_file_len;
    (void) key_file;
    (void) key_file_len;
    (void) insecure;
    XrAotRuntime *runtime = xr_aot_runtime_current();
    /* Shape checks on the tagged arguments only. Which port numbers a node may
     * bind is decided in cluster.xr's start(). */
    if (!runtime || !XR_IS_INT(port_value) || !XR_IS_BOOL(tls_enabled) || XR_TO_BOOL(tls_enabled) ||
        !XR_IS_INT(heartbeat_tick_ms) || !XR_IS_INT(phi_min_samples) ||
        !XR_IS_FLOAT(phi_threshold) || !XR_IS_INT(topic_delivery_fanout_max) ||
        !XR_IS_INT(tombstone_retention_ms))
        return XR_FALSE_VAL;
    int64_t fanout_max = XR_TO_INT(topic_delivery_fanout_max);
    if (fanout_max <= 0 || fanout_max > UINT32_MAX)
        return XR_FALSE_VAL;
    int64_t port = XR_TO_INT(port_value);
    XrAotClusterState *cluster = (XrAotClusterState *) xr_calloc(1, sizeof(*cluster));
    if (!cluster)
        return XR_FALSE_VAL;
    cluster->listen_socket = XR_INVALID_SOCKET;
    cluster->heartbeat_interval_ms =
        XR_IS_INT(heartbeat_interval_ms) ? XR_TO_INT(heartbeat_interval_ms) : 0;
    cluster->heartbeat_timeout_ms =
        XR_IS_INT(heartbeat_timeout_ms) ? XR_TO_INT(heartbeat_timeout_ms) : 0;
    cluster->max_missed_heartbeats =
        XR_IS_INT(max_missed_heartbeats) ? XR_TO_INT(max_missed_heartbeats) : 0;
    cluster->phi_threshold = XR_TO_FLOAT(phi_threshold);
    if (!aot_cluster_copy_text(cluster->self_name, sizeof(cluster->self_name), name, name_len,
                               false) ||
        !aot_cluster_copy_text(cluster->secret, sizeof(cluster->secret), secret, secret_len,
                               true)) {
        xr_free(cluster);
        return XR_FALSE_VAL;
    }
    cluster->runtime = runtime;
    cluster->values = xr_aot_runtime_value_ops(runtime);
    if (!cluster->values || !cluster->values->buffer_bytes) {
        xr_free(cluster);
        return XR_FALSE_VAL;
    }
    xr_mutex_init(&cluster->nodes_lock);
    cluster->topics = xr_topic_registry_new_aot(runtime, (uint32_t) fanout_max);
    if (!cluster->topics) {
        aot_cluster_state_destroy(cluster);
        return XR_FALSE_VAL;
    }
    cluster->listen_socket = aot_cluster_listen_socket((uint16_t) port, &cluster->listen_port);
    if (cluster->listen_socket == XR_INVALID_SOCKET) {
        aot_cluster_state_destroy(cluster);
        return XR_FALSE_VAL;
    }
    atomic_store_explicit(&cluster->running, true, memory_order_release);
    if (!xr_aot_runtime_service_install(runtime, XR_AOT_SERVICE_SLOT_CLUSTER, cluster,
                                        aot_cluster_service_destroy)) {
        aot_cluster_state_destroy(cluster);
        return XR_FALSE_VAL;
    }
    if (!xr_thread_create(&cluster->accept_thread, aot_cluster_accept_main, cluster)) {
        (void) xr_aot_runtime_service_remove(runtime, XR_AOT_SERVICE_SLOT_CLUSTER);
        return XR_FALSE_VAL;
    }
    cluster->accept_started = true;
    return XR_TRUE_VAL;
}

XrValue xrt_cluster_join(const char *host_text, int64_t host_len, XrValue port_value) {
    XrAotRuntime *runtime = NULL;
    XrAotClusterState *cluster = aot_cluster_acquire(&runtime);
    int64_t port = XR_IS_INT(port_value) ? XR_TO_INT(port_value) : -1;
    char host[XR_ADDRESS_HOST_MAX + 1];
    if (!cluster)
        return XR_FALSE_VAL;
    if (port < 0 || port > UINT16_MAX ||
        !aot_cluster_copy_text(host, sizeof(host), host_text, host_len, false)) {
        aot_cluster_release(runtime);
        return XR_FALSE_VAL;
    }
    xr_socket_t socket = aot_cluster_connect(host, (uint16_t) port);
    if (socket == XR_INVALID_SOCKET) {
        aot_cluster_release(runtime);
        return XR_FALSE_VAL;
    }
    char peer_name[XR_NODE_NAME_MAX + 1] = {0};
    if (!xr_cluster_blocking_client_handshake(socket, cluster->self_name, cluster->secret, 0x01,
                                              peer_name)) {
        xr_closesocket(socket);
        aot_cluster_release(runtime);
        return XR_FALSE_VAL;
    }
    XrAotClusterNode *node = aot_cluster_node_new(cluster, peer_name, socket);
    if (!node || !aot_cluster_node_start(node)) {
        if (node)
            aot_cluster_node_destroy(node);
        else
            xr_closesocket(socket);
        aot_cluster_release(runtime);
        return XR_FALSE_VAL;
    }
    aot_cluster_add_node(cluster, node);
    aot_cluster_release(runtime);
    return XR_TRUE_VAL;
}

XrValue xrt_cluster_stop(void) {
    XrAotRuntime *runtime = xr_aot_runtime_current();
    if (runtime)
        (void) xr_aot_runtime_service_remove(runtime, XR_AOT_SERVICE_SLOT_CLUSTER);
    return XR_NULL_VAL;
}

int64_t xrt_cluster_publish_local(const char *topic_text, int64_t topic_len, XrValue envelope) {
    XrAotRuntime *runtime = NULL;
    XrAotClusterState *cluster = aot_cluster_acquire(&runtime);
    char topic[XR_TOPIC_PATTERN_MAX + 1];
    if (!cluster)
        return XR_CLUSTER_DELIVERY_UNAVAILABLE;
    if (!aot_cluster_copy_text(topic, sizeof(topic), topic_text, topic_len, false)) {
        aot_cluster_release(runtime);
        return XR_CLUSTER_DELIVERY_INVALID_TOPIC;
    }
    const uint8_t *bytes = NULL;
    size_t length = 0;
    if (!cluster->values->buffer_bytes(envelope, &bytes, &length) || length > UINT32_MAX) {
        aot_cluster_release(runtime);
        return XR_CLUSTER_DELIVERY_INVALID_ENVELOPE;
    }
    XrClusterDelivery result = (XrClusterDelivery) xr_topic_registry_deliver(
        cluster->topics, topic, bytes, (uint32_t) length);
    aot_cluster_release(runtime);
    return result;
}

int64_t xrt_cluster_publish_remote(const char *topic_text, int64_t topic_len, XrValue envelope,
                                   XrValue hop_limit) {
    XrAotRuntime *runtime = NULL;
    XrAotClusterState *cluster = aot_cluster_acquire(&runtime);
    int64_t hop_value = XR_IS_INT(hop_limit) ? XR_TO_INT(hop_limit) : -1;
    char topic[XR_TOPIC_PATTERN_MAX + 1];
    if (!cluster)
        return XR_CLUSTER_DELIVERY_UNAVAILABLE;
    if (!aot_cluster_copy_text(topic, sizeof(topic), topic_text, topic_len, false) ||
        hop_value < 0 || hop_value > UINT8_MAX) {
        aot_cluster_release(runtime);
        return XR_CLUSTER_DELIVERY_INVALID_TOPIC;
    }
    const uint8_t *bytes = NULL;
    size_t length = 0;
    if (!cluster->values->buffer_bytes(envelope, &bytes, &length) ||
        length > XR_FRAME_MAX_PAYLOAD - 2u - (size_t) topic_len) {
        aot_cluster_release(runtime);
        return XR_CLUSTER_DELIVERY_INVALID_ENVELOPE;
    }
    XrClusterDelivery result = aot_cluster_broadcast(cluster, NULL, (uint8_t) hop_value, topic,
                                                     (uint8_t) topic_len, bytes, (uint32_t) length);
    aot_cluster_release(runtime);
    return result;
}

XrValue xrt_cluster_listen(const char *pattern_text, int64_t pattern_len, XrValue capacity_value) {
    XrAotRuntime *runtime = NULL;
    XrAotClusterState *cluster = aot_cluster_acquire(&runtime);
    int64_t capacity = XR_IS_INT(capacity_value) ? XR_TO_INT(capacity_value) : -1;
    if (!cluster)
        return XR_NULL_VAL;
    /* Pattern legality and the capacity a subscription may ask for are decided
     * in cluster.xr's listen(). What is left is the tagged-value shape, the
     * width of the channel capacity field and the provider's C-string edge. */
    char pattern[XR_TOPIC_PATTERN_MAX + 1];
    if (capacity <= 0 || capacity > UINT32_MAX ||
        !aot_cluster_copy_text(pattern, sizeof(pattern), pattern_text, pattern_len, false)) {
        aot_cluster_release(runtime);
        return XR_NULL_VAL;
    }
    XrChannel *channel = xr_topic_registry_subscribe(cluster->topics, pattern, (uint32_t) capacity);
    if (!channel) {
        aot_cluster_release(runtime);
        return XR_NULL_VAL;
    }
    aot_cluster_release(runtime);
    return xr_value_from_channel(channel);
}
