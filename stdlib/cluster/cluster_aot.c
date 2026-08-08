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
#include "cluster_topic_core.h"
#include "../../src/aot/xrt_cluster.h"
#include "../../src/base/xchecks.h"
#include "../../src/coro/xaot_coro.h"
#include "../../src/coro/xchannel.h"
#include "../../src/coro/xchannel_ops.h"
#include "../../src/os/os_net.h"
#include "../../src/os/os_random.h"
#include "../../src/runtime/core/xr_runtime_core.h"
#include "../../src/runtime/value/xtransfer_mode.h"

#include <stdio.h>
#include <string.h>

#define XR_AOT_CLUSTER_QUEUE_HIGH_WATERMARK (4u * 1024u * 1024u)
#define XR_AOT_CLUSTER_IO_TIMEOUT_MS 5000

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

typedef struct XrAotClusterSubscription {
    char pattern[XR_TOPIC_PATTERN_MAX + 1];
    XrChannel *channel;
    struct XrAotClusterSubscription *next;
} XrAotClusterSubscription;

struct XrAotClusterState {
    XrAotRuntime *runtime;
    XrRuntimeCore *core;
    const XrAotValueOps *values;
    char self_name[XR_NODE_NAME_MAX + 1];
    char secret[64];
    uint16_t listen_port;
    xr_socket_t listen_socket;
    _Atomic(bool) running;
    xr_thread_t accept_thread;
    bool accept_started;
    xr_mutex_t nodes_lock;
    XrAotClusterNode *nodes;
    xr_mutex_t subscriptions_lock;
    XrAotClusterSubscription *subscriptions;
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

static int aot_cluster_wait_socket(xr_socket_t socket, bool read_ready, int timeout_ms) {
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (read_ready)
        FD_SET(socket, &read_set);
    else
        FD_SET(socket, &write_set);
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int result = select((int) socket + 1, read_ready ? &read_set : NULL,
                        read_ready ? NULL : &write_set, NULL, &timeout);
    return result > 0 ? 0 : -1;
}

static bool aot_cluster_read_all(xr_socket_t socket, uint8_t *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        if (aot_cluster_wait_socket(socket, true, XR_AOT_CLUSTER_IO_TIMEOUT_MS) != 0)
            return false;
        ssize_t count = xr_socket_recv(socket, data + offset, length - offset);
        if (count <= 0)
            return false;
        offset += (size_t) count;
    }
    return true;
}

static bool aot_cluster_write_all(xr_socket_t socket, const uint8_t *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        if (aot_cluster_wait_socket(socket, false, XR_AOT_CLUSTER_IO_TIMEOUT_MS) != 0)
            return false;
        ssize_t count = xr_socket_send(socket, data + offset, length - offset);
        if (count <= 0)
            return false;
        offset += (size_t) count;
    }
    return true;
}

static bool aot_cluster_read_frame(xr_socket_t socket, uint8_t *type, uint8_t **payload,
                                   uint32_t *payload_length) {
    uint8_t header[XR_FRAME_HEADER_SIZE + 1];
    if (!type || !payload || !payload_length ||
        !aot_cluster_read_all(socket, header, sizeof(header)) ||
        cluster_frame_read_header(header, sizeof(header), type, payload_length) != 0)
        return false;
    *payload = NULL;
    if (*payload_length == 0)
        return true;
    uint8_t *owned = (uint8_t *) xr_malloc(*payload_length);
    if (!owned)
        return false;
    if (!aot_cluster_read_all(socket, owned, *payload_length)) {
        xr_free(owned);
        return false;
    }
    *payload = owned;
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
        bool written = aot_cluster_write_all(node->socket, frame->data, frame->length);
        aot_cluster_frame_free(frame);
        if (!written)
            break;
    }
    aot_cluster_node_stop(node);
    return NULL;
}

static XrClusterDelivery aot_cluster_deliver_local(XrAotClusterState *cluster, const char *topic,
                                                   const uint8_t *envelope,
                                                   uint32_t envelope_length) {
    XrChannel *targets[256];
    int target_count = 0;
    xr_mutex_lock(&cluster->subscriptions_lock);
    for (XrAotClusterSubscription *sub = cluster->subscriptions;
         sub && target_count < (int) (sizeof(targets) / sizeof(targets[0])); sub = sub->next) {
        if (!xr_channel_is_closed(sub->channel) && xr_cluster_topic_matches(sub->pattern, topic))
            targets[target_count++] = sub->channel;
    }
    xr_mutex_unlock(&cluster->subscriptions_lock);

    int delivered = 0;
    int rejected = 0;
    for (int i = 0; i < target_count; i++) {
        XrValue buffer = cluster->values->buffer_copy_transfer(envelope, envelope_length);
        if (XR_IS_NULL(buffer)) {
            rejected++;
            continue;
        }
        if (xr_chan_try_send_transfer_core(cluster->core, targets[i], buffer, XR_TRANSFER_MOVE))
            delivered++;
        else
            rejected++;
    }
    if (delivered > 0)
        return XR_CLUSTER_DELIVERY_ACCEPTED;
    if (rejected > 0)
        return XR_CLUSTER_DELIVERY_OVERLOADED;
    return XR_CLUSTER_DELIVERY_DISCONNECTED;
}

static void aot_cluster_process_transport(XrAotClusterNode *node, const uint8_t *payload,
                                          uint32_t payload_length) {
    if (!node || !payload || payload_length < 2)
        return;
    uint8_t topic_length = payload[1];
    if (topic_length == 0 || topic_length > XR_TOPIC_PATTERN_MAX ||
        payload_length <= (uint32_t) topic_length + 2)
        return;
    char topic[XR_TOPIC_PATTERN_MAX + 1];
    memcpy(topic, payload + 2, topic_length);
    topic[topic_length] = '\0';
    const uint8_t *envelope = payload + 2 + topic_length;
    uint32_t envelope_length = payload_length - 2 - topic_length;
    if (envelope_length < XR_CLUSTER_ENVELOPE_HEADER_SIZE)
        return;
    (void) aot_cluster_deliver_local(node->cluster, topic, envelope, envelope_length);
}

static void aot_cluster_process_frame(XrAotClusterNode *node, uint8_t frame_type,
                                      const uint8_t *payload, uint32_t payload_length) {
    if (frame_type == XR_FRAME_TRANSPORT_ENVELOPE) {
        aot_cluster_process_transport(node, payload, payload_length);
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
        if (!aot_cluster_read_frame(node->socket, &frame_type, &payload, &payload_length))
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

static bool aot_cluster_server_handshake(XrAotClusterState *cluster, xr_socket_t socket,
                                         char *peer_name) {
    uint8_t type = 0;
    uint8_t *payload = NULL;
    uint32_t payload_length = 0;
    XrFrameHandshakeReq request = {0};
    if (!aot_cluster_read_frame(socket, &type, &payload, &payload_length) ||
        type != XR_FRAME_HANDSHAKE_REQ ||
        cluster_frame_decode_handshake_req(payload, payload_length, &request) != 0 ||
        request.version != XR_CLUSTER_HANDSHAKE_VERSION) {
        xr_free(payload);
        return false;
    }
    xr_free(payload);

    XrFrameHandshakeAck ack = {0};
    ack.version = XR_CLUSTER_HANDSHAKE_VERSION;
    strncpy(ack.name, cluster->self_name, XR_NODE_NAME_MAX);
    xr_random_bytes(ack.nonce, XR_NONCE_SIZE);
    cluster_compute_proof(cluster->secret, request.nonce, ack.proof);
    ack.flags = 0x01;
    uint8_t frame[512];
    int frame_length = cluster_frame_encode_handshake_ack(frame, sizeof(frame), &ack);
    if (frame_length <= 0 || !aot_cluster_write_all(socket, frame, (size_t) frame_length))
        return false;

    payload = NULL;
    payload_length = 0;
    XrFrameHandshakeDone done = {0};
    if (!aot_cluster_read_frame(socket, &type, &payload, &payload_length) ||
        type != XR_FRAME_HANDSHAKE_DONE ||
        cluster_frame_decode_handshake_done(payload, payload_length, &done) != 0) {
        xr_free(payload);
        return false;
    }
    xr_free(payload);
    uint8_t expected[XR_PROOF_SIZE];
    cluster_compute_proof(cluster->secret, ack.nonce, expected);
    bool accepted = cluster_proof_equal(done.proof, expected);
    memset(expected, 0, sizeof(expected));
    if (accepted) {
        strncpy(peer_name, request.name, XR_NODE_NAME_MAX);
        peer_name[XR_NODE_NAME_MAX] = '\0';
    }
    return accepted;
}

static void *aot_cluster_accept_main(void *argument) {
    XrAotClusterState *cluster = (XrAotClusterState *) argument;
    while (atomic_load_explicit(&cluster->running, memory_order_acquire)) {
        xr_socket_t socket = accept(cluster->listen_socket, NULL, NULL);
        if (socket == XR_INVALID_SOCKET)
            break;
        char peer_name[XR_NODE_NAME_MAX + 1] = {0};
        if (!aot_cluster_server_handshake(cluster, socket, peer_name) ||
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

static bool aot_cluster_parse_address(const char *address, char *host, size_t host_capacity,
                                      char *port, size_t port_capacity) {
    const char *colon = strrchr(address, ':');
    if (!colon || colon == address || colon[1] == '\0')
        return false;
    const char *host_start = address;
    size_t host_length = (size_t) (colon - address);
    if (host_length >= 2 && address[0] == '[' && colon[-1] == ']') {
        host_start++;
        host_length -= 2;
    } else if (memchr(address, ':', host_length)) {
        return false;
    }
    size_t port_length = strlen(colon + 1);
    if (host_length == 0 || host_length >= host_capacity || port_length == 0 ||
        port_length >= port_capacity)
        return false;
    memcpy(host, host_start, host_length);
    host[host_length] = '\0';
    memcpy(port, colon + 1, port_length + 1);
    return true;
}

static xr_socket_t aot_cluster_connect(const char *address) {
    char host[256];
    char port[16];
    if (!aot_cluster_parse_address(address, host, sizeof(host), port, sizeof(port)))
        return XR_INVALID_SOCKET;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *resolved = NULL;
    if (getaddrinfo(host, port, &hints, &resolved) != 0)
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

static bool aot_cluster_client_handshake(XrAotClusterState *cluster, xr_socket_t socket,
                                         char *peer_name) {
    XrFrameHandshakeReq request = {0};
    request.version = XR_CLUSTER_HANDSHAKE_VERSION;
    strncpy(request.name, cluster->self_name, XR_NODE_NAME_MAX);
    xr_random_bytes(request.nonce, XR_NONCE_SIZE);
    request.flags = 0x01;
    uint8_t frame[512];
    int frame_length = cluster_frame_encode_handshake_req(frame, sizeof(frame), &request);
    if (frame_length <= 0 || !aot_cluster_write_all(socket, frame, (size_t) frame_length))
        return false;

    uint8_t type = 0;
    uint8_t *payload = NULL;
    uint32_t payload_length = 0;
    XrFrameHandshakeAck ack = {0};
    if (!aot_cluster_read_frame(socket, &type, &payload, &payload_length) ||
        type != XR_FRAME_HANDSHAKE_ACK ||
        cluster_frame_decode_handshake_ack(payload, payload_length, &ack) != 0 ||
        ack.version != XR_CLUSTER_HANDSHAKE_VERSION) {
        xr_free(payload);
        return false;
    }
    xr_free(payload);
    uint8_t expected[XR_PROOF_SIZE];
    cluster_compute_proof(cluster->secret, request.nonce, expected);
    bool accepted = cluster_proof_equal(ack.proof, expected);
    memset(expected, 0, sizeof(expected));
    if (!accepted)
        return false;

    XrFrameHandshakeDone done = {0};
    cluster_compute_proof(cluster->secret, ack.nonce, done.proof);
    frame_length = cluster_frame_encode_handshake_done(frame, sizeof(frame), &done);
    if (frame_length <= 0 || !aot_cluster_write_all(socket, frame, (size_t) frame_length))
        return false;
    strncpy(peer_name, ack.name, XR_NODE_NAME_MAX);
    peer_name[XR_NODE_NAME_MAX] = '\0';
    return true;
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
    if (cluster->listen_socket != XR_INVALID_SOCKET) {
        shutdown(cluster->listen_socket, XR_SHUT_RDWR);
        xr_closesocket(cluster->listen_socket);
        cluster->listen_socket = XR_INVALID_SOCKET;
    }
    if (cluster->accept_started)
        (void) xr_thread_join(cluster->accept_thread, NULL);

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

    xr_mutex_lock(&cluster->subscriptions_lock);
    XrAotClusterSubscription *subscriptions = cluster->subscriptions;
    cluster->subscriptions = NULL;
    xr_mutex_unlock(&cluster->subscriptions_lock);
    while (subscriptions) {
        XrAotClusterSubscription *next = subscriptions->next;
        xr_channel_close(subscriptions->channel);
        xr_free(subscriptions);
        subscriptions = next;
    }
    xr_mutex_destroy(&cluster->subscriptions_lock);
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
                          XrValue insecure) {
    (void) ca_file;
    (void) ca_file_len;
    (void) cert_file;
    (void) cert_file_len;
    (void) key_file;
    (void) key_file_len;
    (void) insecure;
    XrAotRuntime *runtime = xr_aot_runtime_current();
    int64_t port = XR_IS_INT(port_value) ? XR_TO_INT(port_value) : -1;
    if (!runtime || !XR_IS_BOOL(tls_enabled) || XR_TO_BOOL(tls_enabled) || port < 0 ||
        port > UINT16_MAX)
        return XR_FALSE_VAL;
    XrAotClusterState *cluster = (XrAotClusterState *) xr_calloc(1, sizeof(*cluster));
    if (!cluster)
        return XR_FALSE_VAL;
    cluster->listen_socket = XR_INVALID_SOCKET;
    if (!aot_cluster_copy_text(cluster->self_name, sizeof(cluster->self_name), name, name_len,
                               false) ||
        !aot_cluster_copy_text(cluster->secret, sizeof(cluster->secret), secret, secret_len,
                               true)) {
        xr_free(cluster);
        return XR_FALSE_VAL;
    }
    cluster->runtime = runtime;
    cluster->core = xr_aot_runtime_core(runtime);
    cluster->values = xr_aot_runtime_value_ops(runtime);
    if (!cluster->core || !cluster->values || !cluster->values->buffer_copy_transfer ||
        !cluster->values->buffer_bytes) {
        xr_free(cluster);
        return XR_FALSE_VAL;
    }
    xr_mutex_init(&cluster->nodes_lock);
    xr_mutex_init(&cluster->subscriptions_lock);
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

XrValue xrt_cluster_join(const char *address, int64_t address_len) {
    XrAotRuntime *runtime = NULL;
    XrAotClusterState *cluster = aot_cluster_acquire(&runtime);
    char normalized[320];
    if (!cluster)
        return XR_FALSE_VAL;
    if (!aot_cluster_copy_text(normalized, sizeof(normalized), address, address_len, false)) {
        aot_cluster_release(runtime);
        return XR_FALSE_VAL;
    }
    xr_socket_t socket = aot_cluster_connect(normalized);
    if (socket == XR_INVALID_SOCKET) {
        aot_cluster_release(runtime);
        return XR_FALSE_VAL;
    }
    char peer_name[XR_NODE_NAME_MAX + 1] = {0};
    if (!aot_cluster_client_handshake(cluster, socket, peer_name)) {
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

int64_t xrt_cluster_send(const char *topic_text, int64_t topic_len, XrValue envelope) {
    XrAotRuntime *runtime = NULL;
    XrAotClusterState *cluster = aot_cluster_acquire(&runtime);
    char topic[XR_TOPIC_PATTERN_MAX + 1];
    if (!cluster)
        return XR_CLUSTER_DELIVERY_UNAVAILABLE;
    if (!aot_cluster_copy_text(topic, sizeof(topic), topic_text, topic_len, false) ||
        !xr_cluster_topic_valid(topic, false)) {
        aot_cluster_release(runtime);
        return XR_CLUSTER_DELIVERY_INVALID_TOPIC;
    }
    const uint8_t *bytes = NULL;
    size_t length = 0;
    if (!cluster->values->buffer_bytes(envelope, &bytes, &length) ||
        length < XR_CLUSTER_ENVELOPE_HEADER_SIZE ||
        length > XR_FRAME_MAX_PAYLOAD - 2 - (size_t) topic_len) {
        aot_cluster_release(runtime);
        return XR_CLUSTER_DELIVERY_INVALID_ENVELOPE;
    }

    XrClusterDelivery local = aot_cluster_deliver_local(cluster, topic, bytes, (uint32_t) length);
    size_t payload_length = 2 + (size_t) topic_len + length;
    uint8_t *payload = (uint8_t *) xr_malloc(payload_length);
    if (!payload) {
        aot_cluster_release(runtime);
        return XR_CLUSTER_DELIVERY_OVERLOADED;
    }
    payload[0] = XR_TOPIC_DEFAULT_HOP_LIMIT;
    payload[1] = (uint8_t) topic_len;
    memcpy(payload + 2, topic, (size_t) topic_len);
    memcpy(payload + 2 + topic_len, bytes, length);

    int connected = 0;
    int accepted = 0;
    xr_mutex_lock(&cluster->nodes_lock);
    for (XrAotClusterNode *node = cluster->nodes; node; node = node->next) {
        if (!atomic_load_explicit(&node->running, memory_order_acquire))
            continue;
        connected++;
        if (aot_cluster_node_enqueue(node, XR_FRAME_TRANSPORT_ENVELOPE, payload,
                                     (uint32_t) payload_length) == 0)
            accepted++;
    }
    xr_mutex_unlock(&cluster->nodes_lock);
    xr_free(payload);
    XrClusterDelivery result = XR_CLUSTER_DELIVERY_DISCONNECTED;
    if (local == XR_CLUSTER_DELIVERY_ACCEPTED || accepted > 0)
        result = XR_CLUSTER_DELIVERY_ACCEPTED;
    else if (local == XR_CLUSTER_DELIVERY_OVERLOADED || connected > 0)
        result = XR_CLUSTER_DELIVERY_OVERLOADED;
    aot_cluster_release(runtime);
    return result;
}

XrValue xrt_cluster_listen(const char *pattern_text, int64_t pattern_len, XrValue capacity_value) {
    XrAotRuntime *runtime = NULL;
    XrAotClusterState *cluster = aot_cluster_acquire(&runtime);
    int64_t capacity = XR_IS_INT(capacity_value) ? XR_TO_INT(capacity_value) : -1;
    if (!cluster)
        return XR_NULL_VAL;
    XrAotClusterSubscription *subscription =
        (XrAotClusterSubscription *) xr_calloc(1, sizeof(*subscription));
    if (!subscription || capacity <= 0 || capacity > XR_CLUSTER_SUBSCRIPTION_CAPACITY_MAX ||
        !aot_cluster_copy_text(subscription->pattern, sizeof(subscription->pattern), pattern_text,
                               pattern_len, false) ||
        !xr_cluster_topic_valid(subscription->pattern, true)) {
        xr_free(subscription);
        aot_cluster_release(runtime);
        return XR_NULL_VAL;
    }
    XrAotContext context = {
        .runtime = cluster->runtime,
        .coro = NULL,
        .vm_host_ops = NULL,
        .vm_host = NULL,
        .worker = NULL,
    };
    XrValue channel_value = xr_aot_channel_new(&context, (uint32_t) capacity);
    if (!xr_value_is_channel(channel_value)) {
        xr_free(subscription);
        aot_cluster_release(runtime);
        return XR_NULL_VAL;
    }
    subscription->channel = xr_value_to_channel(channel_value);
    xr_mutex_lock(&cluster->subscriptions_lock);
    subscription->next = cluster->subscriptions;
    cluster->subscriptions = subscription;
    xr_mutex_unlock(&cluster->subscriptions_lock);
    aot_cluster_release(runtime);
    return channel_value;
}
