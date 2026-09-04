/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_blocking_runtime.c - Blocking cluster service provider
 *
 * KEY CONCEPT:
 *   The provider owns blocking listener and peer lifetimes for standalone
 *   runtimes. Its public operations are one-to-one projections of the native
 *   cluster leaves, without an AOT value or service-slot dependency.
 */

#include "xcluster_blocking_runtime.h"

#include "xchannel.h"
#include "xtopic_registry.h"
#include "../base/xmalloc.h"
#include "../io/xcluster_blocking.h"
#include "../os/os_thread.h"

#include <stdatomic.h>
#include <string.h>

#define XR_CLUSTER_BLOCKING_ACCEPT_POLL_MS 100
#define XR_CLUSTER_BLOCKING_SECRET_MAX 63

typedef struct XrClusterBlockingNode {
    XrClusterBlockingRuntime *runtime;
    XrClusterBlockingPeer *peer;
    struct XrClusterBlockingNode *next;
} XrClusterBlockingNode;

struct XrClusterBlockingRuntime {
    char self_name[XR_NODE_NAME_MAX + 1];
    char secret[XR_CLUSTER_BLOCKING_SECRET_MAX + 1];
    xr_socket_t listener;
    _Atomic(bool) running;
    xr_thread_t accept_thread;
    bool accept_started;
    size_t output_queue_high_watermark;
    xr_mutex_t nodes_lock;
    XrClusterBlockingNode *nodes;
    XrTopicRegistry *topics;
};

static XrClusterBlockingDelivery blocking_delivery_from_topic(XrTopicDelivery delivery) {
    switch (delivery) {
        case XR_TOPIC_DELIVERY_ACCEPTED:
            return XR_CLUSTER_BLOCKING_DELIVERY_ACCEPTED;
        case XR_TOPIC_DELIVERY_OVERLOADED:
            return XR_CLUSTER_BLOCKING_DELIVERY_OVERLOADED;
        case XR_TOPIC_DELIVERY_DISCONNECTED:
            return XR_CLUSTER_BLOCKING_DELIVERY_DISCONNECTED;
        case XR_TOPIC_DELIVERY_UNAVAILABLE:
        default:
            return XR_CLUSTER_BLOCKING_DELIVERY_UNAVAILABLE;
    }
}

static bool blocking_copy_text(char *target, size_t capacity, const char *source,
                               size_t source_length, bool allow_empty) {
    if (!target || capacity == 0 || !source || source_length >= capacity ||
        (!allow_empty && source_length == 0) || memchr(source, '\0', source_length))
        return false;
    memcpy(target, source, source_length);
    target[source_length] = '\0';
    return true;
}

static XrClusterBlockingDelivery blocking_broadcast(XrClusterBlockingRuntime *runtime,
                                                    XrClusterBlockingNode *excluded,
                                                    uint8_t hop_limit, const char *topic,
                                                    uint8_t topic_length, const uint8_t *envelope,
                                                    uint32_t envelope_length) {
    int connected = 0;
    int accepted = 0;
    xr_mutex_lock(&runtime->nodes_lock);
    for (XrClusterBlockingNode *node = runtime->nodes; node; node = node->next) {
        if (node == excluded || !xr_cluster_blocking_peer_is_running(node->peer))
            continue;
        connected++;
        if (xr_cluster_blocking_peer_enqueue_transport(node->peer, hop_limit, topic, topic_length,
                                                       envelope, envelope_length) == 0)
            accepted++;
    }
    xr_mutex_unlock(&runtime->nodes_lock);
    if (accepted > 0)
        return XR_CLUSTER_BLOCKING_DELIVERY_ACCEPTED;
    return connected > 0 ? XR_CLUSTER_BLOCKING_DELIVERY_OVERLOADED
                         : XR_CLUSTER_BLOCKING_DELIVERY_DISCONNECTED;
}

static void blocking_receive_frame(void *context, const XrClusterFrameProjection *projection) {
    XrClusterBlockingNode *node = (XrClusterBlockingNode *) context;
    if (!node || !projection || projection->kind != XR_CLUSTER_FRAME_TRANSPORT ||
        !xr_topic_name_valid(projection->transport.topic))
        return;
    (void) xr_topic_registry_deliver(node->runtime->topics, projection->transport.topic,
                                     projection->transport.envelope,
                                     projection->transport.envelope_length);
    if (projection->transport.hop_limit > 0)
        (void) blocking_broadcast(
            node->runtime, node, (uint8_t) (projection->transport.hop_limit - 1),
            projection->transport.topic, projection->transport.topic_length,
            projection->transport.envelope, projection->transport.envelope_length);
}

static XrClusterBlockingNode *blocking_node_new(XrClusterBlockingRuntime *runtime,
                                                xr_socket_t socket) {
    XrClusterBlockingNode *node = (XrClusterBlockingNode *) xr_calloc(1, sizeof(*node));
    if (!node)
        return NULL;
    node->runtime = runtime;
    XrClusterBlockingPeerConfig config = {
        .socket = socket,
        .context = node,
        .frame_handler = blocking_receive_frame,
        .queue_high_watermark = runtime->output_queue_high_watermark,
    };
    node->peer = xr_cluster_blocking_peer_new(&config);
    if (!node->peer) {
        xr_free(node);
        return NULL;
    }
    return node;
}

static void blocking_node_destroy(XrClusterBlockingNode *node) {
    if (!node)
        return;
    xr_cluster_blocking_peer_destroy(node->peer);
    xr_free(node);
}

static bool blocking_node_activate(XrClusterBlockingRuntime *runtime, xr_socket_t socket) {
    XrClusterBlockingNode *node = blocking_node_new(runtime, socket);
    if (!node) {
        xr_cluster_blocking_socket_close(socket);
        return false;
    }
    if (!xr_cluster_blocking_peer_start(node->peer)) {
        blocking_node_destroy(node);
        return false;
    }
    xr_mutex_lock(&runtime->nodes_lock);
    node->next = runtime->nodes;
    runtime->nodes = node;
    xr_mutex_unlock(&runtime->nodes_lock);
    return true;
}

static void *blocking_accept_main(void *argument) {
    XrClusterBlockingRuntime *runtime = (XrClusterBlockingRuntime *) argument;
    while (atomic_load_explicit(&runtime->running, memory_order_acquire)) {
        if (xr_cluster_blocking_wait(runtime->listener, true, XR_CLUSTER_BLOCKING_ACCEPT_POLL_MS) !=
            0)
            continue;
        if (!atomic_load_explicit(&runtime->running, memory_order_acquire))
            break;
        xr_socket_t socket = xr_cluster_blocking_listener_accept(runtime->listener);
        if (socket == XR_INVALID_SOCKET)
            break;
        char peer_name[XR_NODE_NAME_MAX + 1] = {0};
        if (!xr_cluster_blocking_server_handshake(socket, runtime->self_name, runtime->secret, 0x01,
                                                  peer_name) ||
            !atomic_load_explicit(&runtime->running, memory_order_acquire)) {
            xr_cluster_blocking_socket_close(socket);
            continue;
        }
        (void) blocking_node_activate(runtime, socket);
    }
    return NULL;
}

XrClusterBlockingRuntime *
xr_cluster_blocking_runtime_new(const XrClusterBlockingRuntimeConfig *config,
                                uint16_t *actual_port) {
    if (!config || !config->topics || !actual_port || config->output_queue_high_watermark == 0)
        return NULL;
    XrClusterBlockingRuntime *runtime = (XrClusterBlockingRuntime *) xr_calloc(1, sizeof(*runtime));
    if (!runtime)
        return NULL;
    runtime->listener = XR_INVALID_SOCKET;
    runtime->output_queue_high_watermark = config->output_queue_high_watermark;
    if (!blocking_copy_text(runtime->self_name, sizeof(runtime->self_name), config->name,
                            config->name_length, false) ||
        !blocking_copy_text(runtime->secret, sizeof(runtime->secret), config->secret,
                            config->secret_length, true)) {
        xr_free(runtime);
        return NULL;
    }
    xr_mutex_init(&runtime->nodes_lock);
    runtime->listener = xr_cluster_blocking_listener_open(config->port, actual_port);
    if (runtime->listener == XR_INVALID_SOCKET) {
        xr_mutex_destroy(&runtime->nodes_lock);
        xr_free(runtime);
        return NULL;
    }
    runtime->topics = config->topics;
    return runtime;
}

bool xr_cluster_blocking_runtime_start(XrClusterBlockingRuntime *runtime) {
    if (!runtime || runtime->accept_started || runtime->listener == XR_INVALID_SOCKET)
        return false;
    atomic_store_explicit(&runtime->running, true, memory_order_release);
    if (!xr_thread_create(&runtime->accept_thread, blocking_accept_main, runtime)) {
        atomic_store_explicit(&runtime->running, false, memory_order_release);
        return false;
    }
    runtime->accept_started = true;
    return true;
}

void xr_cluster_blocking_runtime_destroy(void *context) {
    XrClusterBlockingRuntime *runtime = (XrClusterBlockingRuntime *) context;
    if (!runtime)
        return;
    atomic_store_explicit(&runtime->running, false, memory_order_release);
    if (runtime->accept_started)
        (void) xr_thread_join(runtime->accept_thread, NULL);
    xr_cluster_blocking_socket_close(runtime->listener);
    runtime->listener = XR_INVALID_SOCKET;
    xr_mutex_lock(&runtime->nodes_lock);
    for (XrClusterBlockingNode *node = runtime->nodes; node; node = node->next)
        xr_cluster_blocking_peer_stop(node->peer);
    XrClusterBlockingNode *nodes = runtime->nodes;
    runtime->nodes = NULL;
    xr_mutex_unlock(&runtime->nodes_lock);
    while (nodes) {
        XrClusterBlockingNode *next = nodes->next;
        blocking_node_destroy(nodes);
        nodes = next;
    }
    xr_topic_registry_destroy(runtime->topics);
    xr_mutex_destroy(&runtime->nodes_lock);
    memset(runtime->secret, 0, sizeof(runtime->secret));
    xr_free(runtime);
}

bool xr_cluster_blocking_runtime_join(XrClusterBlockingRuntime *runtime, const char *host,
                                      size_t host_length, uint16_t port) {
    if (!runtime || !atomic_load_explicit(&runtime->running, memory_order_acquire))
        return false;
    xr_socket_t socket = xr_cluster_blocking_connect(host, host_length, port);
    if (socket == XR_INVALID_SOCKET)
        return false;
    char peer_name[XR_NODE_NAME_MAX + 1] = {0};
    if (!xr_cluster_blocking_client_handshake(socket, runtime->self_name, runtime->secret, 0x01,
                                              peer_name)) {
        xr_cluster_blocking_socket_close(socket);
        return false;
    }
    return blocking_node_activate(runtime, socket);
}

XrClusterBlockingDelivery
xr_cluster_blocking_runtime_publish_local(XrClusterBlockingRuntime *runtime, const char *topic,
                                          size_t topic_length, const uint8_t *envelope,
                                          uint32_t envelope_length) {
    char topic_text[XR_TOPIC_PATTERN_MAX + 1];
    if (!runtime)
        return XR_CLUSTER_BLOCKING_DELIVERY_UNAVAILABLE;
    if (!blocking_copy_text(topic_text, sizeof(topic_text), topic, topic_length, false))
        return XR_CLUSTER_BLOCKING_DELIVERY_INVALID_TOPIC;
    return blocking_delivery_from_topic(
        xr_topic_registry_deliver(runtime->topics, topic_text, envelope, envelope_length));
}

XrClusterBlockingDelivery
xr_cluster_blocking_runtime_publish_remote(XrClusterBlockingRuntime *runtime, uint8_t hop_limit,
                                           const char *topic, size_t topic_length,
                                           const uint8_t *envelope, uint32_t envelope_length) {
    char topic_text[XR_TOPIC_PATTERN_MAX + 1];
    if (!runtime)
        return XR_CLUSTER_BLOCKING_DELIVERY_UNAVAILABLE;
    if (!blocking_copy_text(topic_text, sizeof(topic_text), topic, topic_length, false))
        return XR_CLUSTER_BLOCKING_DELIVERY_INVALID_TOPIC;
    if (!envelope || envelope_length > XR_FRAME_MAX_PAYLOAD - 2u - topic_length)
        return XR_CLUSTER_BLOCKING_DELIVERY_INVALID_ENVELOPE;
    return blocking_broadcast(runtime, NULL, hop_limit, topic_text, (uint8_t) topic_length,
                              envelope, envelope_length);
}

XrChannel *xr_cluster_blocking_runtime_subscribe(XrClusterBlockingRuntime *runtime,
                                                 const char *pattern, size_t pattern_length,
                                                 uint32_t capacity) {
    char pattern_text[XR_TOPIC_PATTERN_MAX + 1];
    if (!runtime ||
        !blocking_copy_text(pattern_text, sizeof(pattern_text), pattern, pattern_length, false))
        return NULL;
    return xr_topic_registry_subscribe(runtime->topics, pattern_text, capacity);
}
