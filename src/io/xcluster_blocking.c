/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_blocking.c - Blocking socket provider for standalone AOT cluster
 */

#include "xcluster_blocking.h"

#include "xcluster_handshake.h"
#include "xcluster_wire.h"
#include "../base/xmalloc.h"
#include "../os/os_random.h"
#include "../os/os_thread.h"
#include "../shared/xr_crypto_core.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct XrClusterBlockingFrame {
    uint8_t *data;
    uint32_t length;
    struct XrClusterBlockingFrame *next;
} XrClusterBlockingFrame;

struct XrClusterBlockingPeer {
    xr_socket_t socket;
    void *context;
    XrClusterBlockingFrameHandler frame_handler;
    _Atomic(bool) running;
    xr_mutex_t queue_lock;
    xr_cond_t queue_ready;
    XrClusterBlockingFrame *queue_head;
    XrClusterBlockingFrame *queue_tail;
    size_t queue_bytes;
    size_t queue_high_watermark;
    xr_thread_t reader_thread;
    xr_thread_t writer_thread;
    bool reader_started;
    bool writer_started;
};

int xr_cluster_blocking_wait(xr_socket_t socket, bool read_ready, int timeout_ms) {
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

xr_socket_t xr_cluster_blocking_listener_open(uint16_t port, uint16_t *actual_port) {
    if (!actual_port)
        return XR_INVALID_SOCKET;
    xr_socket_t socket_fd = (xr_socket_t) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd == XR_INVALID_SOCKET)
        return XR_INVALID_SOCKET;
    (void) xr_socket_set_reuseaddr(socket_fd, true);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socket_fd, (struct sockaddr *) &address, sizeof(address)) != 0 ||
        listen(socket_fd, 128) != 0) {
        xr_closesocket(socket_fd);
        return XR_INVALID_SOCKET;
    }
    socklen_t length = (socklen_t) sizeof(address);
    if (getsockname(socket_fd, (struct sockaddr *) &address, &length) != 0) {
        xr_closesocket(socket_fd);
        return XR_INVALID_SOCKET;
    }
    *actual_port = ntohs(address.sin_port);
    return socket_fd;
}

xr_socket_t xr_cluster_blocking_listener_accept(xr_socket_t listener) {
    if (listener == XR_INVALID_SOCKET)
        return XR_INVALID_SOCKET;
    return accept(listener, NULL, NULL);
}

xr_socket_t xr_cluster_blocking_connect(const char *host, size_t host_length, uint16_t port) {
    if (!host || host_length == 0 || host_length > XR_CLUSTER_ADDRESS_HOST_MAX ||
        memchr(host, '\0', host_length))
        return XR_INVALID_SOCKET;
    char host_text[XR_CLUSTER_ADDRESS_HOST_MAX + 1];
    memcpy(host_text, host, host_length);
    host_text[host_length] = '\0';
    char service[8];
    if (snprintf(service, sizeof(service), "%u", (unsigned) port) < 0)
        return XR_INVALID_SOCKET;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *resolved = NULL;
    if (getaddrinfo(host_text, service, &hints, &resolved) != 0)
        return XR_INVALID_SOCKET;
    xr_socket_t connected = XR_INVALID_SOCKET;
    for (struct addrinfo *candidate = resolved; candidate; candidate = candidate->ai_next) {
        xr_socket_t socket_fd = (xr_socket_t) socket(candidate->ai_family, candidate->ai_socktype,
                                                     candidate->ai_protocol);
        if (socket_fd == XR_INVALID_SOCKET)
            continue;
        if (connect(socket_fd, candidate->ai_addr, (socklen_t) candidate->ai_addrlen) == 0) {
            connected = socket_fd;
            break;
        }
        xr_closesocket(socket_fd);
    }
    freeaddrinfo(resolved);
    return connected;
}

void xr_cluster_blocking_socket_close(xr_socket_t socket_fd) {
    if (socket_fd == XR_INVALID_SOCKET)
        return;
    shutdown(socket_fd, XR_SHUT_RDWR);
    xr_closesocket(socket_fd);
}

static bool blocking_read_all(xr_socket_t socket, uint8_t *data, size_t length, int timeout_ms) {
    size_t offset = 0;
    while (offset < length) {
        if (xr_cluster_blocking_wait(socket, true, timeout_ms) != 0)
            return false;
        ssize_t count = xr_socket_recv(socket, data + offset, length - offset);
        if (count <= 0)
            return false;
        offset += (size_t) count;
    }
    return true;
}

bool xr_cluster_blocking_write_all(xr_socket_t socket, const uint8_t *data, size_t length,
                                   int timeout_ms) {
    size_t offset = 0;
    while (offset < length) {
        if (xr_cluster_blocking_wait(socket, false, timeout_ms) != 0)
            return false;
        ssize_t count = xr_socket_send(socket, data + offset, length - offset);
        if (count <= 0)
            return false;
        offset += (size_t) count;
    }
    return true;
}

bool xr_cluster_blocking_read_frame(xr_socket_t socket, uint8_t *type, uint8_t **payload,
                                    uint32_t *payload_length, int timeout_ms) {
    uint8_t header[XR_FRAME_HEADER_SIZE + 1];
    if (!type || !payload || !payload_length)
        return false;
    *payload = NULL;
    *payload_length = 0;
    if (!blocking_read_all(socket, header, sizeof(header), timeout_ms) ||
        cluster_frame_read_header(header, sizeof(header), type, payload_length) != 0)
        return false;
    if (*payload_length == 0)
        return true;
    uint8_t *owned = (uint8_t *) xr_malloc(*payload_length);
    if (!owned)
        return false;
    if (!blocking_read_all(socket, owned, *payload_length, timeout_ms)) {
        xr_free(owned);
        return false;
    }
    *payload = owned;
    return true;
}

static void blocking_frame_free(XrClusterBlockingFrame *frame) {
    if (!frame)
        return;
    xr_free(frame->data);
    xr_free(frame);
}

void xr_cluster_blocking_peer_stop(XrClusterBlockingPeer *peer) {
    if (!peer || !atomic_exchange_explicit(&peer->running, false, memory_order_acq_rel))
        return;
    shutdown(peer->socket, XR_SHUT_RDWR);
    xr_mutex_lock(&peer->queue_lock);
    xr_cond_broadcast(&peer->queue_ready);
    xr_mutex_unlock(&peer->queue_lock);
}

static int blocking_peer_enqueue_owned(XrClusterBlockingPeer *peer, XrClusterBlockingFrame *frame) {
    if (!peer || !frame) {
        blocking_frame_free(frame);
        return -1;
    }
    xr_mutex_lock(&peer->queue_lock);
    if (!atomic_load_explicit(&peer->running, memory_order_relaxed) ||
        peer->queue_bytes > peer->queue_high_watermark ||
        frame->length > peer->queue_high_watermark - peer->queue_bytes) {
        xr_mutex_unlock(&peer->queue_lock);
        blocking_frame_free(frame);
        return -1;
    }
    if (peer->queue_tail)
        peer->queue_tail->next = frame;
    else
        peer->queue_head = frame;
    peer->queue_tail = frame;
    peer->queue_bytes += frame->length;
    xr_cond_signal(&peer->queue_ready);
    xr_mutex_unlock(&peer->queue_lock);
    return 0;
}

static int blocking_peer_enqueue_copy(XrClusterBlockingPeer *peer, const uint8_t *data,
                                      uint32_t length) {
    if (!peer || !data || length == 0)
        return -1;
    XrClusterBlockingFrame *frame = (XrClusterBlockingFrame *) xr_calloc(1, sizeof(*frame));
    if (!frame)
        return -1;
    frame->data = (uint8_t *) xr_malloc(length);
    if (!frame->data) {
        xr_free(frame);
        return -1;
    }
    memcpy(frame->data, data, length);
    frame->length = length;
    return blocking_peer_enqueue_owned(peer, frame);
}

int xr_cluster_blocking_peer_enqueue_transport(XrClusterBlockingPeer *peer, uint8_t hop_limit,
                                               const char *topic, uint8_t topic_length,
                                               const uint8_t *envelope, uint32_t envelope_length) {
    if (!peer || !topic || topic_length == 0 || !envelope)
        return -1;
    size_t frame_length = (size_t) XR_FRAME_HEADER_SIZE + 3u + topic_length + envelope_length;
    if (frame_length > UINT32_MAX)
        return -1;
    XrClusterBlockingFrame *frame = (XrClusterBlockingFrame *) xr_calloc(1, sizeof(*frame));
    if (!frame)
        return -1;
    frame->data = (uint8_t *) xr_malloc(frame_length);
    if (!frame->data) {
        xr_free(frame);
        return -1;
    }
    int wrote = cluster_frame_write_transport(frame->data, frame_length, hop_limit, topic,
                                              topic_length, envelope, envelope_length);
    if (wrote <= 0) {
        blocking_frame_free(frame);
        return -1;
    }
    frame->length = (uint32_t) wrote;
    return blocking_peer_enqueue_owned(peer, frame);
}

static XrClusterBlockingFrame *blocking_peer_take_frame(XrClusterBlockingPeer *peer) {
    xr_mutex_lock(&peer->queue_lock);
    while (atomic_load_explicit(&peer->running, memory_order_relaxed) && !peer->queue_head)
        xr_cond_wait(&peer->queue_ready, &peer->queue_lock);
    if (!atomic_load_explicit(&peer->running, memory_order_relaxed)) {
        xr_mutex_unlock(&peer->queue_lock);
        return NULL;
    }
    XrClusterBlockingFrame *frame = peer->queue_head;
    peer->queue_head = frame->next;
    if (!peer->queue_head)
        peer->queue_tail = NULL;
    peer->queue_bytes -= frame->length;
    frame->next = NULL;
    xr_mutex_unlock(&peer->queue_lock);
    return frame;
}

static void *blocking_peer_writer_main(void *argument) {
    XrClusterBlockingPeer *peer = (XrClusterBlockingPeer *) argument;
    while (atomic_load_explicit(&peer->running, memory_order_acquire)) {
        XrClusterBlockingFrame *frame = blocking_peer_take_frame(peer);
        if (!frame)
            break;
        bool written = xr_cluster_blocking_write_all(peer->socket, frame->data, frame->length,
                                                     XR_CLUSTER_HANDSHAKE_TIMEOUT_MS);
        blocking_frame_free(frame);
        if (!written)
            break;
    }
    xr_cluster_blocking_peer_stop(peer);
    return NULL;
}

static void *blocking_peer_reader_main(void *argument) {
    XrClusterBlockingPeer *peer = (XrClusterBlockingPeer *) argument;
    while (atomic_load_explicit(&peer->running, memory_order_acquire)) {
        uint8_t frame_type = 0;
        uint8_t *payload = NULL;
        uint32_t payload_length = 0;
        if (!xr_cluster_blocking_read_frame(peer->socket, &frame_type, &payload, &payload_length,
                                            XR_CLUSTER_HANDSHAKE_TIMEOUT_MS))
            break;
        XrClusterFrameProjection projection;
        bool valid = cluster_frame_project(frame_type, payload, payload_length, &projection);
        if (valid && projection.response_length > 0)
            (void) blocking_peer_enqueue_copy(peer, projection.response,
                                              projection.response_length);
        if (valid)
            peer->frame_handler(peer->context, &projection);
        xr_free(payload);
    }
    xr_cluster_blocking_peer_stop(peer);
    return NULL;
}

XrClusterBlockingPeer *xr_cluster_blocking_peer_new(const XrClusterBlockingPeerConfig *config) {
    if (!config || config->socket == XR_INVALID_SOCKET || !config->frame_handler ||
        config->queue_high_watermark == 0)
        return NULL;
    XrClusterBlockingPeer *peer = (XrClusterBlockingPeer *) xr_calloc(1, sizeof(*peer));
    if (!peer)
        return NULL;
    peer->socket = config->socket;
    peer->context = config->context;
    peer->frame_handler = config->frame_handler;
    peer->queue_high_watermark = config->queue_high_watermark;
    atomic_store_explicit(&peer->running, true, memory_order_relaxed);
    xr_mutex_init(&peer->queue_lock);
    xr_cond_init(&peer->queue_ready);
    (void) xr_socket_set_nodelay(peer->socket, true);
    return peer;
}

bool xr_cluster_blocking_peer_start(XrClusterBlockingPeer *peer) {
    if (!peer || peer->writer_started || peer->reader_started ||
        !atomic_load_explicit(&peer->running, memory_order_acquire) ||
        !xr_thread_create(&peer->writer_thread, blocking_peer_writer_main, peer))
        return false;
    peer->writer_started = true;
    if (!xr_thread_create(&peer->reader_thread, blocking_peer_reader_main, peer)) {
        xr_cluster_blocking_peer_stop(peer);
        (void) xr_thread_join(peer->writer_thread, NULL);
        peer->writer_started = false;
        return false;
    }
    peer->reader_started = true;
    return true;
}

bool xr_cluster_blocking_peer_is_running(const XrClusterBlockingPeer *peer) {
    return peer && atomic_load_explicit(&peer->running, memory_order_acquire);
}

void xr_cluster_blocking_peer_destroy(XrClusterBlockingPeer *peer) {
    if (!peer)
        return;
    xr_cluster_blocking_peer_stop(peer);
    if (peer->writer_started)
        (void) xr_thread_join(peer->writer_thread, NULL);
    if (peer->reader_started)
        (void) xr_thread_join(peer->reader_thread, NULL);
    xr_closesocket(peer->socket);
    XrClusterBlockingFrame *frame = peer->queue_head;
    while (frame) {
        XrClusterBlockingFrame *next = frame->next;
        blocking_frame_free(frame);
        frame = next;
    }
    xr_cond_destroy(&peer->queue_ready);
    xr_mutex_destroy(&peer->queue_lock);
    xr_free(peer);
}

bool xr_cluster_blocking_server_handshake(xr_socket_t socket, const char *self_name,
                                          const char *secret, uint32_t flags,
                                          char peer_name[XR_NODE_NAME_MAX + 1]) {
    if (!self_name || !secret || !peer_name)
        return false;
    uint8_t type = 0;
    uint8_t *payload = NULL;
    uint32_t payload_length = 0;
    XrFrameHandshakeReq request = {0};
    XrFrameHandshakeAck ack = {0};
    uint8_t nonce[XR_NONCE_SIZE] = {0};
    uint8_t frame[512] = {0};
    bool accepted = false;
    if (!xr_cluster_blocking_read_frame(socket, &type, &payload, &payload_length,
                                        XR_CLUSTER_HANDSHAKE_TIMEOUT_MS) ||
        type != XR_FRAME_HANDSHAKE_REQ)
        goto cleanup;
    xr_random_bytes(nonce, sizeof(nonce));
    int frame_length = xr_cluster_handshake_server_accept_request(self_name, secret, flags, nonce,
                                                                  payload, payload_length, &request,
                                                                  &ack, frame, sizeof(frame));
    xr_free(payload);
    payload = NULL;
    if (frame_length <= 0 || !xr_cluster_blocking_write_all(socket, frame, (size_t) frame_length,
                                                            XR_CLUSTER_HANDSHAKE_TIMEOUT_MS))
        goto cleanup;

    payload_length = 0;
    accepted = xr_cluster_blocking_read_frame(socket, &type, &payload, &payload_length,
                                              XR_CLUSTER_HANDSHAKE_TIMEOUT_MS) &&
               type == XR_FRAME_HANDSHAKE_DONE &&
               xr_cluster_handshake_server_accept_done(secret, &ack, payload, payload_length);
    if (accepted) {
        strncpy(peer_name, request.name, XR_NODE_NAME_MAX);
        peer_name[XR_NODE_NAME_MAX] = '\0';
    }

cleanup:
    xr_free(payload);
    xr_secure_wipe(nonce, sizeof(nonce));
    xr_secure_wipe(&request, sizeof(request));
    xr_secure_wipe(&ack, sizeof(ack));
    xr_secure_wipe(frame, sizeof(frame));
    return accepted;
}

bool xr_cluster_blocking_client_handshake(xr_socket_t socket, const char *self_name,
                                          const char *secret, uint32_t flags,
                                          char peer_name[XR_NODE_NAME_MAX + 1]) {
    if (!self_name || !secret || !peer_name)
        return false;
    XrFrameHandshakeReq request = {0};
    XrFrameHandshakeAck ack = {0};
    XrFrameHandshakeDone done = {0};
    uint8_t nonce[XR_NONCE_SIZE] = {0};
    uint8_t frame[512] = {0};
    uint8_t *payload = NULL;
    bool accepted = false;
    xr_random_bytes(nonce, sizeof(nonce));
    int frame_length =
        xr_cluster_handshake_client_start(self_name, flags, nonce, &request, frame, sizeof(frame));
    if (frame_length <= 0 || !xr_cluster_blocking_write_all(socket, frame, (size_t) frame_length,
                                                            XR_CLUSTER_HANDSHAKE_TIMEOUT_MS))
        goto cleanup;

    uint8_t type = 0;
    uint32_t payload_length = 0;
    if (!xr_cluster_blocking_read_frame(socket, &type, &payload, &payload_length,
                                        XR_CLUSTER_HANDSHAKE_TIMEOUT_MS) ||
        type != XR_FRAME_HANDSHAKE_ACK)
        goto cleanup;
    frame_length = xr_cluster_handshake_client_accept_ack(secret, &request, payload, payload_length,
                                                          &ack, &done, frame, sizeof(frame));
    xr_free(payload);
    payload = NULL;
    accepted =
        frame_length > 0 && xr_cluster_blocking_write_all(socket, frame, (size_t) frame_length,
                                                          XR_CLUSTER_HANDSHAKE_TIMEOUT_MS);
    if (accepted) {
        strncpy(peer_name, ack.name, XR_NODE_NAME_MAX);
        peer_name[XR_NODE_NAME_MAX] = '\0';
    }

cleanup:
    xr_free(payload);
    xr_secure_wipe(nonce, sizeof(nonce));
    xr_secure_wipe(&request, sizeof(request));
    xr_secure_wipe(&ack, sizeof(ack));
    xr_secure_wipe(&done, sizeof(done));
    xr_secure_wipe(frame, sizeof(frame));
    return accepted;
}
