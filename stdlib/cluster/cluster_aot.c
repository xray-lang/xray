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
    /* The heartbeat schedule cluster.xr decided for this node. The AOT adapter
     * accepts the schedule but has no heartbeat coroutine to run it yet; only
     * the VM transport in stdlib/cluster/cluster.c drives heartbeats today.
     * These fields exist so the schedule keeps a single owner in cluster.xr:
     * when AOT grows its own heartbeat it reads the numbers from here, with no
     * further change to the leaf signature. */
    int64_t heartbeat_interval_ms;
    int64_t heartbeat_timeout_ms;
    int64_t max_missed_heartbeats;
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
        /* Closing a listening socket from another thread does not reliably
         * interrupt a blocking accept() on every supported OS. Poll with a
         * bounded timeout so stop can flip running and join this thread before
         * it closes the descriptor, avoiding both shutdown hangs and fd reuse. */
        if (aot_cluster_wait_socket(cluster->listen_socket, true, XR_AOT_CLUSTER_ACCEPT_POLL_MS) !=
            0)
            continue;
        if (!atomic_load_explicit(&cluster->running, memory_order_acquire))
            break;
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
                          XrValue insecure, XrValue heartbeat_interval_ms,
                          XrValue heartbeat_timeout_ms, XrValue max_missed_heartbeats) {
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
    if (!runtime || !XR_IS_INT(port_value) || !XR_IS_BOOL(tls_enabled) || XR_TO_BOOL(tls_enabled))
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

XrValue xrt_cluster_join(const char *host_text, int64_t host_len, XrValue port_value) {
    XrAotRuntime *runtime = NULL;
    XrAotClusterState *cluster = aot_cluster_acquire(&runtime);
    int64_t port = XR_IS_INT(port_value) ? XR_TO_INT(port_value) : -1;
    char host[256];
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

int64_t xrt_cluster_send(const char *topic_text, int64_t topic_len, XrValue envelope,
                         XrValue hop_limit) {
    XrAotRuntime *runtime = NULL;
    XrAotClusterState *cluster = aot_cluster_acquire(&runtime);
    int64_t hop_value = XR_IS_INT(hop_limit) ? XR_TO_INT(hop_limit) : -1;
    char topic[XR_TOPIC_PATTERN_MAX + 1];
    if (!cluster)
        return XR_CLUSTER_DELIVERY_UNAVAILABLE;
    /* Topic legality is decided in cluster.xr's send(). What is left here is
     * the local buffer capacity and the width of the one-byte hop field the
     * transport frame carries. */
    if (!aot_cluster_copy_text(topic, sizeof(topic), topic_text, topic_len, false) ||
        hop_value < 0 || hop_value > UINT8_MAX) {
        aot_cluster_release(runtime);
        return XR_CLUSTER_DELIVERY_INVALID_TOPIC;
    }
    const uint8_t *bytes = NULL;
    size_t length = 0;
    /* The upper bound is the wire frame limit, not a policy: cluster.xr cannot
     * see how many bytes the topic costs inside the transport frame. */
    if (!cluster->values->buffer_bytes(envelope, &bytes, &length) ||
        length > XR_FRAME_MAX_PAYLOAD - 2 - (size_t) topic_len) {
        aot_cluster_release(runtime);
        return XR_CLUSTER_DELIVERY_INVALID_ENVELOPE;
    }

    XrClusterDelivery local = aot_cluster_deliver_local(cluster, topic, bytes, (uint32_t) length);
    int connected = 0;
    int accepted = 0;
    xr_mutex_lock(&cluster->nodes_lock);
    for (XrAotClusterNode *node = cluster->nodes; node; node = node->next) {
        if (!atomic_load_explicit(&node->running, memory_order_acquire))
            continue;
        connected++;
        if (aot_cluster_node_enqueue_transport(node, (uint8_t) hop_value, topic,
                                               (uint8_t) topic_len, bytes, (uint32_t) length) == 0)
            accepted++;
    }
    xr_mutex_unlock(&cluster->nodes_lock);
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
    /* Pattern legality and the capacity a subscription may ask for are decided
     * in cluster.xr's listen(). What is left is the tagged-value shape, the
     * width of the channel capacity field and the local buffer capacity. */
    if (!subscription || capacity <= 0 || capacity > UINT32_MAX ||
        !aot_cluster_copy_text(subscription->pattern, sizeof(subscription->pattern), pattern_text,
                               pattern_len, false)) {
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
