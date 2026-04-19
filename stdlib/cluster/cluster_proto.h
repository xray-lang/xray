/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_proto.h - Cluster protocol frame encoding/decoding
 *
 * KEY CONCEPT:
 *   Length-prefixed TCP frame protocol for inter-node communication.
 *   Each frame has a 4-byte big-endian length header followed by
 *   a 1-byte type tag and variable-length payload.
 *
 * FRAME FORMAT:
 *   [Length 4B big-endian] [Type 1B] [Payload ...]
 *   Length includes Type + Payload (excludes the 4-byte length itself).
 */

#ifndef XR_CLUSTER_PROTO_H
#define XR_CLUSTER_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../src/base/xdefs.h"
#include "../../src/base/xmalloc.h"

/* ========== Frame Types ========== */

typedef enum {
    XR_FRAME_HANDSHAKE_REQ  = 0x01,
    XR_FRAME_HANDSHAKE_ACK  = 0x02,
    XR_FRAME_HANDSHAKE_DONE = 0x03,
    XR_FRAME_HANDSHAKE_ERR  = 0x04,
    XR_FRAME_HEARTBEAT_PING = 0x05,
    XR_FRAME_HEARTBEAT_PONG = 0x06,
    XR_FRAME_CHANNEL_SEND   = 0x07,
    XR_FRAME_CHANNEL_RECV_REQ = 0x08,
    XR_FRAME_CHANNEL_RECV_RSP = 0x09,
    XR_FRAME_CHANNEL_CLOSE  = 0x0A,
    XR_FRAME_SERVICE_CALL   = 0x0B,
    XR_FRAME_SERVICE_REPLY  = 0x0C,
    XR_FRAME_NODE_INFO      = 0x0D,
    XR_FRAME_CHANNEL_SYNC        = 0x0E,
    XR_FRAME_CHANNEL_SUBSCRIBE   = 0x0F,
    XR_FRAME_CHANNEL_UNSUBSCRIBE = 0x10,
    XR_FRAME_CHANNEL_PUSH        = 0x11,
    XR_FRAME_TOPIC_SUBSCRIBE     = 0x12,
    XR_FRAME_TOPIC_UNSUBSCRIBE   = 0x13,
    XR_FRAME_TOPIC_PUBLISH       = 0x14,
    XR_FRAME_CORO_MONITOR        = 0x15,
    XR_FRAME_CORO_DEMONITOR      = 0x16,
    XR_FRAME_CORO_EXIT           = 0x17,
} XrFrameType;

// Frame header size (length field only, type is part of payload)
#define XR_FRAME_HEADER_SIZE  4

// Max frame payload (16MB, safety limit)
#define XR_FRAME_MAX_PAYLOAD  (16 * 1024 * 1024)

// Handshake nonce size
#define XR_NONCE_SIZE  16

// SHA-256 digest size
#define XR_PROOF_SIZE  32

// Max node name length
#define XR_NODE_NAME_MAX  63

// Max channel name length
#define XR_CHANNEL_NAME_MAX  127

// Max service name length
#define XR_SERVICE_NAME_MAX  127

/* ========== Frame Structures ========== */

typedef struct {
    uint8_t  version;
    char     name[XR_NODE_NAME_MAX + 1];
    uint8_t  nonce[XR_NONCE_SIZE];
    uint32_t flags;
} XrFrameHandshakeReq;

typedef struct {
    uint8_t  version;
    char     name[XR_NODE_NAME_MAX + 1];
    uint8_t  nonce[XR_NONCE_SIZE];
    uint8_t  proof[XR_PROOF_SIZE];
    uint32_t flags;
} XrFrameHandshakeAck;

typedef struct {
    uint8_t  proof[XR_PROOF_SIZE];
} XrFrameHandshakeDone;

typedef struct {
    int64_t  timestamp;
} XrFrameHeartbeat;

typedef struct {
    char     channel_name[XR_CHANNEL_NAME_MAX + 1];
    uint8_t  channel_name_len;
    uint8_t *value_data;        // Serialized XrValue (borrowed pointer)
    uint32_t value_len;
} XrFrameChannelSend;

typedef struct {
    char     channel_name[XR_CHANNEL_NAME_MAX + 1];
    uint8_t  channel_name_len;
} XrFrameChannelRecvReq;

typedef struct {
    bool     has_value;
    uint8_t *value_data;        // Serialized XrValue (borrowed pointer)
    uint32_t value_len;
} XrFrameChannelRecvRsp;

typedef struct {
    uint64_t request_id;
    char     service_name[XR_SERVICE_NAME_MAX + 1];
    uint8_t  service_name_len;
    uint8_t *args_data;         // Serialized args (borrowed pointer)
    uint32_t args_len;
} XrFrameServiceCall;

typedef struct {
    uint64_t request_id;
    bool     is_error;
    uint8_t *result_data;       // Serialized result (borrowed pointer)
    uint32_t result_len;
} XrFrameServiceReply;

/* ========== Channel Subscribe/Push Structures ========== */

typedef struct {
    char     channel_name[XR_CHANNEL_NAME_MAX + 1];
    uint8_t  channel_name_len;
} XrFrameChannelSubscribe;

typedef struct {
    char     channel_name[XR_CHANNEL_NAME_MAX + 1];
    uint8_t  channel_name_len;
    uint8_t *value_data;        // Serialized XrValue (borrowed pointer)
    uint32_t value_len;
} XrFrameChannelPush;

/* ========== Encode API ========== */

/*
 * Write a raw frame to an output buffer.
 * Prepends 4-byte length header + 1-byte type.
 *
 * buf:         Output buffer (must have enough space: 5 + payload_len)
 * frame_type:  XrFrameType
 * payload:     Frame payload bytes
 * payload_len: Length of payload
 *
 * Returns: Total bytes written (5 + payload_len)
 */
XR_FUNC int xr_frame_write(uint8_t *buf, uint8_t frame_type,
                   const uint8_t *payload, uint32_t payload_len);

/*
 * Encode handshake request into output buffer.
 * Returns total frame size, or -1 on error.
 */
XR_FUNC int xr_frame_encode_handshake_req(uint8_t *buf, size_t buf_size,
                                   const XrFrameHandshakeReq *req);

XR_FUNC int xr_frame_encode_handshake_ack(uint8_t *buf, size_t buf_size,
                                   const XrFrameHandshakeAck *ack);

XR_FUNC int xr_frame_encode_handshake_done(uint8_t *buf, size_t buf_size,
                                    const XrFrameHandshakeDone *done);

XR_FUNC int xr_frame_encode_heartbeat(uint8_t *buf, size_t buf_size,
                               uint8_t type, int64_t timestamp);

XR_FUNC int xr_frame_encode_channel_send(uint8_t *buf, size_t buf_size,
                                  const char *channel_name,
                                  const uint8_t *value_data, uint32_t value_len);

XR_FUNC int xr_frame_encode_channel_close(uint8_t *buf, size_t buf_size,
                                   const char *channel_name);

XR_FUNC int xr_frame_encode_service_call(uint8_t *buf, size_t buf_size,
                                  uint64_t request_id,
                                  const char *service_name,
                                  const uint8_t *args_data, uint32_t args_len);

XR_FUNC int xr_frame_encode_service_reply(uint8_t *buf, size_t buf_size,
                                   uint64_t request_id, bool is_error,
                                   const uint8_t *result_data, uint32_t result_len);

XR_FUNC int xr_frame_encode_channel_subscribe(uint8_t *buf, size_t buf_size,
                                       const char *channel_name);

XR_FUNC int xr_frame_encode_channel_unsubscribe(uint8_t *buf, size_t buf_size,
                                         const char *channel_name);

XR_FUNC int xr_frame_encode_channel_push(uint8_t *buf, size_t buf_size,
                                  const char *channel_name,
                                  const uint8_t *value_data, uint32_t value_len);

/* ========== Decode API ========== */

/*
 * Read frame header from raw bytes.
 * Returns payload length (excluding 4-byte header), or -1 on error.
 * frame_type is written on success.
 */
XR_FUNC int xr_frame_read_header(const uint8_t *data, size_t data_len,
                          uint8_t *frame_type, uint32_t *payload_len);

XR_FUNC int xr_frame_decode_handshake_req(const uint8_t *payload, uint32_t len,
                                   XrFrameHandshakeReq *req);

XR_FUNC int xr_frame_decode_handshake_ack(const uint8_t *payload, uint32_t len,
                                   XrFrameHandshakeAck *ack);

XR_FUNC int xr_frame_decode_handshake_done(const uint8_t *payload, uint32_t len,
                                    XrFrameHandshakeDone *done);

XR_FUNC int xr_frame_decode_heartbeat(const uint8_t *payload, uint32_t len,
                               int64_t *timestamp);

XR_FUNC int xr_frame_decode_channel_send(const uint8_t *payload, uint32_t len,
                                  XrFrameChannelSend *out);

XR_FUNC int xr_frame_decode_channel_close(const uint8_t *payload, uint32_t len,
                                   char *channel_name, size_t name_size);

XR_FUNC int xr_frame_decode_service_call(const uint8_t *payload, uint32_t len,
                                  XrFrameServiceCall *out);

XR_FUNC int xr_frame_decode_service_reply(const uint8_t *payload, uint32_t len,
                                   XrFrameServiceReply *out);

XR_FUNC int xr_frame_decode_channel_subscribe(const uint8_t *payload, uint32_t len,
                                       XrFrameChannelSubscribe *out);

XR_FUNC int xr_frame_decode_channel_unsubscribe(const uint8_t *payload, uint32_t len,
                                         char *channel_name, size_t name_size);

XR_FUNC int xr_frame_decode_channel_push(const uint8_t *payload, uint32_t len,
                                  XrFrameChannelPush *out);

/* ========== Coroutine Monitor Frames ========== */

// CORO_MONITOR / CORO_DEMONITOR: [name_len 1B] [name ...]
// CORO_EXIT: [name_len 1B] [name ...] [reason_len 1B] [reason ...]

#define XR_CORO_NAME_MAX  127

XR_FUNC int xr_frame_encode_coro_monitor(uint8_t *buf, size_t buf_size,
                                  uint8_t frame_type, const char *coro_name);

XR_FUNC int xr_frame_encode_coro_exit(uint8_t *buf, size_t buf_size,
                               const char *coro_name, const char *reason);

XR_FUNC int xr_frame_decode_coro_monitor(const uint8_t *payload, uint32_t len,
                                  char *coro_name, size_t name_size);

XR_FUNC int xr_frame_decode_coro_exit(const uint8_t *payload, uint32_t len,
                               char *coro_name, size_t name_size,
                               char *reason, size_t reason_size);

/* ========== Frame Buffer Helper ========== */

/*
 * Stack-first frame buffer with heap fallback.
 * Eliminates the repeated stack_buf[4096] / xr_malloc pattern.
 *
 * NOTE: Always check fb->data for NULL after init when `needed` exceeds
 * the stack slot, since xr_malloc may fail under memory pressure.
 */

typedef struct {
    uint8_t  stack[4096];
    uint8_t *data;
    bool     heap;
} XrFrameBuf;

static inline void xr_frame_buf_init(XrFrameBuf *fb, size_t needed) {
    if (needed <= sizeof(fb->stack)) {
        fb->data = fb->stack;
        fb->heap = false;
    } else {
        fb->data = (uint8_t *)xr_malloc(needed);
        fb->heap = true;
    }
}

static inline void xr_frame_buf_free(XrFrameBuf *fb) {
    if (fb->heap && fb->data) {
        xr_free(fb->data);
        fb->data = NULL;
    }
}

#endif
