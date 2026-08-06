/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_proto.c - Cluster protocol frame encoding/decoding
 *
 * KEY CONCEPT:
 *   All frames use big-endian length-prefixed format.
 *   Encode functions write to a caller-provided buffer.
 *   Decode functions parse from a payload buffer (after header).
 */

#include "cluster_internal.h"
#include <string.h>

/* ========== Byte Helpers ========== */

static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) (v);
}

static inline void put_u64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t) (v >> 56);
    p[1] = (uint8_t) (v >> 48);
    p[2] = (uint8_t) (v >> 40);
    p[3] = (uint8_t) (v >> 32);
    p[4] = (uint8_t) (v >> 24);
    p[5] = (uint8_t) (v >> 16);
    p[6] = (uint8_t) (v >> 8);
    p[7] = (uint8_t) (v);
}

static inline uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static inline uint64_t get_u64(const uint8_t *p) {
    return ((uint64_t) p[0] << 56) | ((uint64_t) p[1] << 48) | ((uint64_t) p[2] << 40) |
           ((uint64_t) p[3] << 32) | ((uint64_t) p[4] << 24) | ((uint64_t) p[5] << 16) |
           ((uint64_t) p[6] << 8) | p[7];
}

static inline int64_t get_i64(const uint8_t *p) {
    return (int64_t) get_u64(p);
}

/* ========== Raw Frame Write ========== */

int cluster_frame_write(uint8_t *buf, uint8_t frame_type, const uint8_t *payload,
                        uint32_t payload_len) {
    uint32_t total_payload = 1 + payload_len;  // type + payload
    put_u32(buf, total_payload);
    buf[4] = frame_type;
    if (payload && payload_len > 0) {
        memcpy(buf + 5, payload, payload_len);
    }
    return (int) (4 + total_payload);
}

/* ========== Frame Header Read ========== */

int cluster_frame_read_header(const uint8_t *data, size_t data_len, uint8_t *frame_type,
                              uint32_t *payload_len) {
    if (data_len < 5)
        return -1;
    uint32_t total = get_u32(data);
    if (total < 1 || total > XR_FRAME_MAX_PAYLOAD)
        return -1;
    *frame_type = data[4];
    *payload_len = total - 1;  // exclude type byte
    return 0;
}

/* ========== Handshake Encode/Decode ========== */

/*
 * HANDSHAKE_REQ payload:
 *   [version 1B] [name_len 1B] [name ...] [nonce 16B] [flags 4B]
 */
int cluster_frame_encode_handshake_req(uint8_t *buf, size_t buf_size,
                                       const XrFrameHandshakeReq *req) {
    uint8_t name_len = (uint8_t) strlen(req->name);
    uint32_t payload_len = 1 + 1 + name_len + XR_NONCE_SIZE + 4;
    uint32_t frame_size = 4 + 1 + payload_len;
    if (buf_size < frame_size)
        return -1;

    uint8_t payload[256];
    uint8_t *p = payload;
    *p++ = req->version;
    *p++ = name_len;
    memcpy(p, req->name, name_len);
    p += name_len;
    memcpy(p, req->nonce, XR_NONCE_SIZE);
    p += XR_NONCE_SIZE;
    put_u32(p, req->flags);

    return cluster_frame_write(buf, XR_FRAME_HANDSHAKE_REQ, payload, payload_len);
}

int cluster_frame_decode_handshake_req(const uint8_t *payload, uint32_t len,
                                       XrFrameHandshakeReq *req) {
    if (len < 1 + 1 + XR_NONCE_SIZE + 4)
        return -1;
    const uint8_t *p = payload;
    req->version = *p++;
    uint8_t name_len = *p++;
    if (name_len > XR_NODE_NAME_MAX)
        return -1;
    if ((uint32_t) (2 + name_len + XR_NONCE_SIZE + 4) > len)
        return -1;
    memcpy(req->name, p, name_len);
    req->name[name_len] = '\0';
    p += name_len;
    memcpy(req->nonce, p, XR_NONCE_SIZE);
    p += XR_NONCE_SIZE;
    req->flags = get_u32(p);
    return 0;
}

/*
 * HANDSHAKE_ACK payload:
 *   [version 1B] [name_len 1B] [name ...] [nonce 16B] [proof 32B] [flags 4B]
 */
int cluster_frame_encode_handshake_ack(uint8_t *buf, size_t buf_size,
                                       const XrFrameHandshakeAck *ack) {
    uint8_t name_len = (uint8_t) strlen(ack->name);
    uint32_t payload_len = 1 + 1 + name_len + XR_NONCE_SIZE + XR_PROOF_SIZE + 4;
    uint32_t frame_size = 4 + 1 + payload_len;
    if (buf_size < frame_size)
        return -1;

    uint8_t payload[384];
    uint8_t *p = payload;
    *p++ = ack->version;
    *p++ = name_len;
    memcpy(p, ack->name, name_len);
    p += name_len;
    memcpy(p, ack->nonce, XR_NONCE_SIZE);
    p += XR_NONCE_SIZE;
    memcpy(p, ack->proof, XR_PROOF_SIZE);
    p += XR_PROOF_SIZE;
    put_u32(p, ack->flags);

    return cluster_frame_write(buf, XR_FRAME_HANDSHAKE_ACK, payload, payload_len);
}

int cluster_frame_decode_handshake_ack(const uint8_t *payload, uint32_t len,
                                       XrFrameHandshakeAck *ack) {
    if (len < 1 + 1 + XR_NONCE_SIZE + XR_PROOF_SIZE + 4)
        return -1;
    const uint8_t *p = payload;
    ack->version = *p++;
    uint8_t name_len = *p++;
    if (name_len > XR_NODE_NAME_MAX)
        return -1;
    if ((uint32_t) (2 + name_len + XR_NONCE_SIZE + XR_PROOF_SIZE + 4) > len)
        return -1;
    memcpy(ack->name, p, name_len);
    ack->name[name_len] = '\0';
    p += name_len;
    memcpy(ack->nonce, p, XR_NONCE_SIZE);
    p += XR_NONCE_SIZE;
    memcpy(ack->proof, p, XR_PROOF_SIZE);
    p += XR_PROOF_SIZE;
    ack->flags = get_u32(p);
    return 0;
}

/*
 * HANDSHAKE_DONE payload: [proof 32B]
 */
int cluster_frame_encode_handshake_done(uint8_t *buf, size_t buf_size,
                                        const XrFrameHandshakeDone *done) {
    uint32_t frame_size = 4 + 1 + XR_PROOF_SIZE;
    if (buf_size < frame_size)
        return -1;
    return cluster_frame_write(buf, XR_FRAME_HANDSHAKE_DONE, done->proof, XR_PROOF_SIZE);
}

int cluster_frame_decode_handshake_done(const uint8_t *payload, uint32_t len,
                                        XrFrameHandshakeDone *done) {
    if (len < XR_PROOF_SIZE)
        return -1;
    memcpy(done->proof, payload, XR_PROOF_SIZE);
    return 0;
}

/* ========== Heartbeat Encode/Decode ========== */

/*
 * HEARTBEAT payload: [timestamp 8B]
 */
int cluster_frame_encode_heartbeat(uint8_t *buf, size_t buf_size, uint8_t type, int64_t timestamp) {
    uint32_t frame_size = 4 + 1 + 8;
    if (buf_size < frame_size)
        return -1;
    uint8_t payload[8];
    put_u64(payload, (uint64_t) timestamp);
    return cluster_frame_write(buf, type, payload, 8);
}

int cluster_frame_decode_heartbeat(const uint8_t *payload, uint32_t len, int64_t *timestamp) {
    if (len < 8)
        return -1;
    *timestamp = get_i64(payload);
    return 0;
}

/* ========== Coroutine Monitor Encode/Decode ========== */

/*
 * CORO_MONITOR / CORO_DEMONITOR payload: [name_len 1B] [name ...]
 */
int cluster_frame_encode_coro_monitor(uint8_t *buf, size_t buf_size, uint8_t frame_type,
                                      const char *coro_name) {
    uint8_t name_len = (uint8_t) strlen(coro_name);
    if (name_len > XR_CORO_NAME_MAX)
        return -1;
    uint32_t payload_len = 1 + name_len;
    uint32_t frame_size = 4 + 1 + payload_len;
    if (buf_size < frame_size)
        return -1;

    uint8_t payload[256];
    payload[0] = name_len;
    memcpy(payload + 1, coro_name, name_len);
    return cluster_frame_write(buf, frame_type, payload, payload_len);
}

int cluster_frame_decode_coro_monitor(const uint8_t *payload, uint32_t len, char *coro_name,
                                      size_t name_size) {
    if (len < 1)
        return -1;
    uint8_t name_len = payload[0];
    if (name_len > XR_CORO_NAME_MAX || (size_t) (name_len + 1) > name_size)
        return -1;
    if (len < (uint32_t) (1 + name_len))
        return -1;
    memcpy(coro_name, payload + 1, name_len);
    coro_name[name_len] = '\0';
    return 0;
}

/*
 * CORO_EXIT payload: [name_len 1B] [name ...] [reason_len 1B] [reason ...]
 */
int cluster_frame_encode_coro_exit(uint8_t *buf, size_t buf_size, const char *coro_name,
                                   const char *reason) {
    uint8_t name_len = (uint8_t) strlen(coro_name);
    uint8_t reason_len = (uint8_t) strlen(reason);
    if (name_len > XR_CORO_NAME_MAX)
        return -1;
    uint32_t payload_len = 1 + name_len + 1 + reason_len;
    uint32_t frame_size = 4 + 1 + payload_len;
    if (buf_size < frame_size)
        return -1;

    uint8_t payload[512];
    payload[0] = name_len;
    memcpy(payload + 1, coro_name, name_len);
    payload[1 + name_len] = reason_len;
    memcpy(payload + 2 + name_len, reason, reason_len);
    return cluster_frame_write(buf, XR_FRAME_CORO_EXIT, payload, payload_len);
}

int cluster_frame_decode_coro_exit(const uint8_t *payload, uint32_t len, char *coro_name,
                                   size_t name_size, char *reason, size_t reason_size) {
    if (len < 2)
        return -1;
    uint8_t name_len = payload[0];
    if (name_len > XR_CORO_NAME_MAX || (size_t) (name_len + 1) > name_size)
        return -1;
    if (len < (uint32_t) (2 + name_len))
        return -1;
    memcpy(coro_name, payload + 1, name_len);
    coro_name[name_len] = '\0';

    uint8_t reason_len = payload[1 + name_len];
    if (len < (uint32_t) (2 + name_len + reason_len))
        return -1;
    if ((size_t) (reason_len + 1) > reason_size)
        return -1;
    memcpy(reason, payload + 2 + name_len, reason_len);
    reason[reason_len] = '\0';
    return 0;
}
