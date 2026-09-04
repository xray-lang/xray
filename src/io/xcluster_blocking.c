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
#include "../shared/xr_crypto_core.h"

#include <string.h>

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
