/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_handshake.h - Backend-neutral cluster handshake projection
 *
 * cluster.xr owns the wire format and proof construction. Native transports
 * cannot re-enter Xray while an I/O continuation is active, so both VM and AOT
 * use this single allocation-free projection of those source rules.
 */

#ifndef XR_IO_CLUSTER_HANDSHAKE_H
#define XR_IO_CLUSTER_HANDSHAKE_H

#include "xcluster_wire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

XR_FUNC int xr_cluster_handshake_client_start(const char *self_name, uint32_t flags,
                                              const uint8_t nonce[XR_NONCE_SIZE],
                                              XrFrameHandshakeReq *request, uint8_t *frame,
                                              size_t frame_capacity);
XR_FUNC int xr_cluster_handshake_server_accept_request(
    const char *self_name, const char *secret, uint32_t flags, const uint8_t nonce[XR_NONCE_SIZE],
    const uint8_t *payload, uint32_t payload_length, XrFrameHandshakeReq *request,
    XrFrameHandshakeAck *ack, uint8_t *frame, size_t frame_capacity);
XR_FUNC bool xr_cluster_handshake_server_accept_done(const char *secret,
                                                     const XrFrameHandshakeAck *ack,
                                                     const uint8_t *payload,
                                                     uint32_t payload_length);
XR_FUNC int xr_cluster_handshake_client_accept_ack(const char *secret,
                                                   const XrFrameHandshakeReq *request,
                                                   const uint8_t *payload, uint32_t payload_length,
                                                   XrFrameHandshakeAck *ack,
                                                   XrFrameHandshakeDone *done, uint8_t *frame,
                                                   size_t frame_capacity);

#endif /* XR_IO_CLUSTER_HANDSHAKE_H */
