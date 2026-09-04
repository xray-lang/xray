/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_wire.h - Allocation-free cluster wire projection for native I/O loops
 *
 * cluster.xr is the semantic owner. This API only projects its byte layout into
 * native reader loops that cannot suspend through an Xray stack yet.
 */

#ifndef XR_IO_CLUSTER_WIRE_H
#define XR_IO_CLUSTER_WIRE_H

#include "../base/xdefs.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    XR_FRAME_HANDSHAKE_REQ = 0x01,
    XR_FRAME_HANDSHAKE_ACK = 0x02,
    XR_FRAME_HANDSHAKE_DONE = 0x03,
    XR_FRAME_HANDSHAKE_ERR = 0x04,
    XR_FRAME_HEARTBEAT_PING = 0x05,
    XR_FRAME_HEARTBEAT_PONG = 0x06,
    XR_FRAME_TRANSPORT_ENVELOPE = 0x07,
    XR_FRAME_CORO_MONITOR = 0x08,
    XR_FRAME_CORO_DEMONITOR = 0x09,
    XR_FRAME_CORO_EXIT = 0x0A,
} XrFrameType;

#define XR_FRAME_HEADER_SIZE 4
#define XR_FRAME_MAX_PAYLOAD (16 * 1024 * 1024)
#define XR_CLUSTER_ENVELOPE_HEADER_SIZE 64
#define XR_NONCE_SIZE 16
#define XR_PROOF_SIZE 32
#define XR_CLUSTER_HANDSHAKE_VERSION 6
#define XR_CLUSTER_HANDSHAKE_TIMEOUT_MS 5000
#define XR_TOPIC_DEFAULT_HOP_LIMIT 3
#define XR_NODE_NAME_MAX 63
#define XR_CORO_NAME_MAX 127

typedef struct {
    uint8_t version;
    char name[XR_NODE_NAME_MAX + 1];
    uint8_t nonce[XR_NONCE_SIZE];
    uint32_t flags;
} XrFrameHandshakeReq;

typedef struct {
    uint8_t version;
    char name[XR_NODE_NAME_MAX + 1];
    uint8_t nonce[XR_NONCE_SIZE];
    uint8_t proof[XR_PROOF_SIZE];
    uint32_t flags;
} XrFrameHandshakeAck;

typedef struct {
    uint8_t proof[XR_PROOF_SIZE];
} XrFrameHandshakeDone;

typedef struct {
    int64_t timestamp;
} XrFrameHeartbeat;

XR_FUNC int cluster_frame_write(uint8_t *buf, uint8_t frame_type, const uint8_t *payload,
                                uint32_t payload_len);
XR_FUNC int cluster_frame_write_transport(uint8_t *buf, size_t buf_size, uint8_t hop_limit,
                                          const char *topic, uint8_t topic_len,
                                          const uint8_t *envelope, uint32_t envelope_len);
XR_FUNC int cluster_frame_encode_handshake_req(uint8_t *buf, size_t buf_size,
                                               const XrFrameHandshakeReq *req);
XR_FUNC int cluster_frame_encode_handshake_ack(uint8_t *buf, size_t buf_size,
                                               const XrFrameHandshakeAck *ack);
XR_FUNC int cluster_frame_encode_handshake_done(uint8_t *buf, size_t buf_size,
                                                const XrFrameHandshakeDone *done);
XR_FUNC int cluster_frame_encode_heartbeat(uint8_t *buf, size_t buf_size, uint8_t type,
                                           int64_t timestamp);
XR_FUNC int cluster_frame_read_header(const uint8_t *data, size_t data_len, uint8_t *frame_type,
                                      uint32_t *payload_len);
XR_FUNC int cluster_frame_decode_handshake_req(const uint8_t *payload, uint32_t len,
                                               XrFrameHandshakeReq *req);
XR_FUNC int cluster_frame_decode_handshake_ack(const uint8_t *payload, uint32_t len,
                                               XrFrameHandshakeAck *ack);
XR_FUNC int cluster_frame_decode_handshake_done(const uint8_t *payload, uint32_t len,
                                                XrFrameHandshakeDone *done);
XR_FUNC int cluster_frame_decode_heartbeat(const uint8_t *payload, uint32_t len,
                                           int64_t *timestamp);
XR_FUNC int cluster_frame_encode_coro_monitor(uint8_t *buf, size_t buf_size, uint8_t frame_type,
                                              const char *coro_name);
XR_FUNC int cluster_frame_encode_coro_exit(uint8_t *buf, size_t buf_size, const char *coro_name,
                                           const char *reason);
XR_FUNC int cluster_frame_decode_coro_monitor(const uint8_t *payload, uint32_t len, char *coro_name,
                                              size_t name_size);
XR_FUNC int cluster_frame_decode_coro_exit(const uint8_t *payload, uint32_t len, char *coro_name,
                                           size_t name_size, char *reason, size_t reason_size);

#endif /* XR_IO_CLUSTER_WIRE_H */
