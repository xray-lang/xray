/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_handshake.c - Backend-neutral cluster handshake projection
 */

#include "xcluster_handshake.h"

#include "xcluster_auth.h"
#include "../shared/xr_crypto_core.h"

#include <string.h>

int xr_cluster_handshake_client_start(const char *self_name, uint32_t flags,
                                      const uint8_t nonce[XR_NONCE_SIZE],
                                      XrFrameHandshakeReq *request, uint8_t *frame,
                                      size_t frame_capacity) {
    if (!self_name || !nonce || !request || !frame)
        return -1;
    memset(request, 0, sizeof(*request));
    request->version = XR_CLUSTER_HANDSHAKE_VERSION;
    strncpy(request->name, self_name, XR_NODE_NAME_MAX);
    request->name[XR_NODE_NAME_MAX] = '\0';
    memcpy(request->nonce, nonce, XR_NONCE_SIZE);
    request->flags = flags;
    return cluster_frame_encode_handshake_req(frame, frame_capacity, request);
}

int xr_cluster_handshake_server_accept_request(const char *self_name, const char *secret,
                                               uint32_t flags, const uint8_t nonce[XR_NONCE_SIZE],
                                               const uint8_t *payload, uint32_t payload_length,
                                               XrFrameHandshakeReq *request,
                                               XrFrameHandshakeAck *ack, uint8_t *frame,
                                               size_t frame_capacity) {
    if (!self_name || !secret || !nonce || !payload || !request || !ack || !frame ||
        cluster_frame_decode_handshake_req(payload, payload_length, request) != 0)
        return -1;
    memset(ack, 0, sizeof(*ack));
    ack->version = XR_CLUSTER_HANDSHAKE_VERSION;
    strncpy(ack->name, self_name, XR_NODE_NAME_MAX);
    ack->name[XR_NODE_NAME_MAX] = '\0';
    memcpy(ack->nonce, nonce, XR_NONCE_SIZE);
    xr_cluster_auth_compute_proof(secret, request->nonce, ack->proof);
    ack->flags = flags;
    return cluster_frame_encode_handshake_ack(frame, frame_capacity, ack);
}

bool xr_cluster_handshake_server_accept_done(const char *secret, const XrFrameHandshakeAck *ack,
                                             const uint8_t *payload, uint32_t payload_length) {
    if (!secret || !ack || !payload)
        return false;
    XrFrameHandshakeDone done;
    if (cluster_frame_decode_handshake_done(payload, payload_length, &done) != 0)
        return false;
    uint8_t expected[XR_PROOF_SIZE];
    xr_cluster_auth_compute_proof(secret, ack->nonce, expected);
    bool accepted = xr_cluster_auth_proof_equal(done.proof, expected);
    xr_secure_wipe(expected, sizeof(expected));
    xr_secure_wipe(&done, sizeof(done));
    return accepted;
}

int xr_cluster_handshake_client_accept_ack(const char *secret, const XrFrameHandshakeReq *request,
                                           const uint8_t *payload, uint32_t payload_length,
                                           XrFrameHandshakeAck *ack, XrFrameHandshakeDone *done,
                                           uint8_t *frame, size_t frame_capacity) {
    if (!secret || !request || !payload || !ack || !done || !frame ||
        cluster_frame_decode_handshake_ack(payload, payload_length, ack) != 0)
        return -1;
    uint8_t expected[XR_PROOF_SIZE];
    xr_cluster_auth_compute_proof(secret, request->nonce, expected);
    bool accepted = xr_cluster_auth_proof_equal(ack->proof, expected);
    xr_secure_wipe(expected, sizeof(expected));
    if (!accepted)
        return -1;
    memset(done, 0, sizeof(*done));
    xr_cluster_auth_compute_proof(secret, ack->nonce, done->proof);
    return cluster_frame_encode_handshake_done(frame, frame_capacity, done);
}
