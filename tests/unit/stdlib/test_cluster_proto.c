/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cluster_proto.c - Cluster wire format known-answer tests
 *
 * KEY CONCEPT:
 *   The WIRE FORMAT block at the top of stdlib/cluster/cluster.xr is the
 *   normative statement of the cluster line format. src/io/xcluster_wire.c is
 *   the native reader-loop projection both backends share. These cases pin the
 *   produced bytes directly rather than only round-tripping through the
 *   matching decoder, so an encoder bug and a mirrored decoder bug cannot
 *   cancel each other out.
 */

#include "../test_framework.h"
#include "../../../stdlib/cluster/cluster_internal.h"

#include <string.h>

TEST(cluster_transport_frame_is_byte_exact) {
    static const char topic[] = "bench.payload";
    uint8_t envelope[XR_CLUSTER_ENVELOPE_HEADER_SIZE];
    for (size_t i = 0; i < sizeof(envelope); i++)
        envelope[i] = (uint8_t) i;

    uint8_t frame[128] = {0};
    int wrote = cluster_frame_write_transport(frame, sizeof(frame), 3, topic,
                                              (uint8_t) strlen(topic), envelope, sizeof(envelope));
    ASSERT_EQ_INT(wrote, 84);

    uint8_t frame_type = 0;
    uint32_t payload_len = 0;
    ASSERT_EQ_INT(cluster_frame_read_header(frame, sizeof(frame), &frame_type, &payload_len), 0);
    ASSERT_EQ_INT(frame_type, XR_FRAME_TRANSPORT_ENVELOPE);
    ASSERT_EQ_INT(payload_len, 79);
    ASSERT_EQ_INT(frame[5], 3);
    ASSERT_EQ_INT(frame[6], strlen(topic));
    ASSERT_EQ_INT(memcmp(frame + 7, topic, strlen(topic)), 0);
    ASSERT_EQ_INT(memcmp(frame + 7 + strlen(topic), envelope, sizeof(envelope)), 0);

    XrFrameTransport transport;
    memset(&transport, 0, sizeof(transport));
    ASSERT_EQ_INT(
        cluster_frame_decode_transport(frame + XR_FRAME_HEADER_SIZE + 1, payload_len, &transport),
        0);
    ASSERT_EQ_INT(transport.hop_limit, 3);
    ASSERT_EQ_INT(transport.topic_length, strlen(topic));
    ASSERT_STR_EQ(transport.topic, topic);
    ASSERT_EQ_UINT(transport.envelope_length, sizeof(envelope));
    ASSERT_TRUE(transport.envelope == frame + 7 + strlen(topic));
    ASSERT_EQ_INT(memcmp(transport.envelope, envelope, sizeof(envelope)), 0);
}

TEST(cluster_transport_frame_rejects_invalid_shapes) {
    static const char topic[] = "bench.payload";
    uint8_t envelope[XR_CLUSTER_ENVELOPE_HEADER_SIZE] = {0};
    uint8_t frame[128] = {0};
    size_t exact_size = XR_FRAME_HEADER_SIZE + 3u + strlen(topic) + sizeof(envelope);

    ASSERT_EQ_INT(cluster_frame_write_transport(frame, exact_size - 1, 3, topic,
                                                (uint8_t) strlen(topic), envelope,
                                                sizeof(envelope)),
                  -1);
    ASSERT_EQ_INT(cluster_frame_write_transport(frame, sizeof(frame), 3, topic, 0, envelope,
                                                sizeof(envelope)),
                  -1);
    ASSERT_EQ_INT(cluster_frame_write_transport(frame, sizeof(frame), 3, topic,
                                                (uint8_t) strlen(topic), envelope,
                                                sizeof(envelope) - 1),
                  -1);
    ASSERT_EQ_INT(cluster_frame_write_transport(frame, sizeof(frame), 3, topic,
                                                (uint8_t) strlen(topic), envelope, UINT32_MAX),
                  -1);

    XrFrameTransport transport;
    uint8_t payload[2 + sizeof(topic) - 1 + XR_CLUSTER_ENVELOPE_HEADER_SIZE] = {0};
    payload[0] = 3;
    payload[1] = (uint8_t) strlen(topic);
    memcpy(payload + 2, topic, strlen(topic));
    ASSERT_EQ_INT(cluster_frame_decode_transport(payload, 1, &transport), -1);
    ASSERT_EQ_INT(cluster_frame_decode_transport(payload, 2, &transport), -1);
    ASSERT_EQ_INT(
        cluster_frame_decode_transport(payload, (uint32_t) sizeof(payload) - 1, &transport), -1);
    ASSERT_EQ_INT(cluster_frame_decode_transport(payload, (uint32_t) sizeof(payload), &transport),
                  0);
}

/*
 * frame := u32be(1 + len(payload)) u8(type) payload
 *
 * The prefix octets are asserted individually instead of through
 * cluster_frame_read_header: reading the length back with the same byte order
 * the writer used would pass even if both were little-endian.
 */
TEST(cluster_frame_header_is_big_endian_length_prefix) {
    uint8_t payload[3] = {0xAA, 0xBB, 0xCC};
    uint8_t frame[16] = {0};

    int wrote = cluster_frame_write(frame, XR_FRAME_HANDSHAKE_ERR, payload, sizeof(payload));
    ASSERT_EQ_INT(wrote, (int) (XR_FRAME_HEADER_SIZE + 1u + sizeof(payload)));
    ASSERT_EQ_INT(frame[0], 0x00);
    ASSERT_EQ_INT(frame[1], 0x00);
    ASSERT_EQ_INT(frame[2], 0x00);
    ASSERT_EQ_INT(frame[3], 0x04); /* 1 type octet + 3 payload octets */
    ASSERT_EQ_INT(frame[4], XR_FRAME_HANDSHAKE_ERR);
    ASSERT_EQ_INT(memcmp(frame + 5, payload, sizeof(payload)), 0);

    /* A length that spans two octets pins their order as well. */
    uint8_t wide_payload[300];
    uint8_t wide_frame[XR_FRAME_HEADER_SIZE + 1 + 300] = {0};
    memset(wide_payload, 0x5A, sizeof(wide_payload));
    wrote = cluster_frame_write(wide_frame, XR_FRAME_HEARTBEAT_PING, wide_payload,
                                (uint32_t) sizeof(wide_payload));
    ASSERT_EQ_INT(wrote, (int) sizeof(wide_frame));
    ASSERT_EQ_INT(wide_frame[0], 0x00);
    ASSERT_EQ_INT(wide_frame[1], 0x00);
    ASSERT_EQ_INT(wide_frame[2], 0x01);
    ASSERT_EQ_INT(wide_frame[3], 0x2D); /* 1 + 300 == 0x0000012D */

    uint8_t frame_type = 0;
    uint32_t payload_len = 0;
    ASSERT_EQ_INT(cluster_frame_read_header(frame, sizeof(frame), &frame_type, &payload_len), 0);
    ASSERT_EQ_INT(frame_type, XR_FRAME_HANDSHAKE_ERR);
    ASSERT_EQ_UINT(payload_len, sizeof(payload));

    /* Anything shorter than the prefix plus the type octet is not yet a frame. */
    ASSERT_EQ_INT(cluster_frame_read_header(frame, XR_FRAME_HEADER_SIZE, &frame_type, &payload_len),
                  -1);

    /* total == 0 leaves no room for the type octet the length is defined to include. */
    uint8_t zero_total[5] = {0x00, 0x00, 0x00, 0x00, XR_FRAME_HEARTBEAT_PING};
    ASSERT_EQ_INT(
        cluster_frame_read_header(zero_total, sizeof(zero_total), &frame_type, &payload_len), -1);

    /* One octet past FRAME_MAX_PAYLOAD is refused. */
    uint8_t over_max[5] = {0x01, 0x00, 0x00, 0x01, XR_FRAME_HEARTBEAT_PING};
    ASSERT_EQ_INT(cluster_frame_read_header(over_max, sizeof(over_max), &frame_type, &payload_len),
                  -1);

    /* Exactly FRAME_MAX_PAYLOAD is still a legal frame. */
    uint8_t at_max[5] = {0x01, 0x00, 0x00, 0x00, XR_FRAME_HEARTBEAT_PING};
    ASSERT_EQ_INT(cluster_frame_read_header(at_max, sizeof(at_max), &frame_type, &payload_len), 0);
    ASSERT_EQ_UINT(payload_len, (uint32_t) XR_FRAME_MAX_PAYLOAD - 1u);
}

/*
 * hsReq := u8(version) u8(len(name)) name nonce[16] u32be(flags)
 */
TEST(cluster_handshake_req_round_trips) {
    static const char name[] = "node-alpha";
    const size_t name_len = sizeof(name) - 1u;
    const size_t payload_len = 1u + 1u + name_len + XR_NONCE_SIZE + 4u;

    XrFrameHandshakeReq req;
    memset(&req, 0, sizeof(req));
    req.version = XR_CLUSTER_HANDSHAKE_VERSION;
    memcpy(req.name, name, sizeof(name));
    for (size_t i = 0; i < XR_NONCE_SIZE; i++)
        req.nonce[i] = (uint8_t) (0x10 + i);
    req.flags = 0x01020304u; /* four distinct octets, so byte order is observable */

    uint8_t frame[256] = {0};
    int wrote = cluster_frame_encode_handshake_req(frame, sizeof(frame), &req);
    ASSERT_EQ_INT(wrote, (int) (XR_FRAME_HEADER_SIZE + 1u + payload_len));
    ASSERT_EQ_INT(frame[0], 0x00);
    ASSERT_EQ_INT(frame[1], 0x00);
    ASSERT_EQ_INT(frame[2], 0x00);
    ASSERT_EQ_INT(frame[3], (int) (1u + payload_len));
    ASSERT_EQ_INT(frame[4], XR_FRAME_HANDSHAKE_REQ);

    const uint8_t *p = frame + XR_FRAME_HEADER_SIZE + 1;
    ASSERT_EQ_INT(p[0], XR_CLUSTER_HANDSHAKE_VERSION);
    ASSERT_EQ_INT(p[1], (int) name_len);
    ASSERT_EQ_INT(memcmp(p + 2, name, name_len), 0);
    ASSERT_EQ_INT(memcmp(p + 2 + name_len, req.nonce, XR_NONCE_SIZE), 0);
    const uint8_t *flags = p + 2 + name_len + XR_NONCE_SIZE;
    ASSERT_EQ_INT(flags[0], 0x01);
    ASSERT_EQ_INT(flags[1], 0x02);
    ASSERT_EQ_INT(flags[2], 0x03);
    ASSERT_EQ_INT(flags[3], 0x04);

    XrFrameHandshakeReq decoded;
    memset(&decoded, 0xFF, sizeof(decoded));
    ASSERT_EQ_INT(cluster_frame_decode_handshake_req(p, (uint32_t) payload_len, &decoded), 0);
    ASSERT_EQ_INT(decoded.version, XR_CLUSTER_HANDSHAKE_VERSION);
    ASSERT_STR_EQ(decoded.name, name);
    ASSERT_EQ_INT(memcmp(decoded.nonce, req.nonce, XR_NONCE_SIZE), 0);
    ASSERT_EQ_UINT(decoded.flags, 0x01020304u);

    /* One octet short of the whole frame is refused rather than truncated. */
    ASSERT_EQ_INT(
        cluster_frame_encode_handshake_req(frame, XR_FRAME_HEADER_SIZE + payload_len, &req), -1);
}

/*
 * hsAck := u8(version) u8(len(name)) name nonce[16] proof[32] u32be(flags)
 */
TEST(cluster_handshake_ack_round_trips) {
    static const char name[] = "node-beta";
    const size_t name_len = sizeof(name) - 1u;
    const size_t payload_len = 1u + 1u + name_len + XR_NONCE_SIZE + XR_PROOF_SIZE + 4u;

    XrFrameHandshakeAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.version = XR_CLUSTER_HANDSHAKE_VERSION;
    memcpy(ack.name, name, sizeof(name));
    for (size_t i = 0; i < XR_NONCE_SIZE; i++)
        ack.nonce[i] = (uint8_t) (0x20 + i);
    for (size_t i = 0; i < XR_PROOF_SIZE; i++)
        ack.proof[i] = (uint8_t) (0x80 + i);
    ack.flags = 0x0A0B0C0Du;

    uint8_t frame[256] = {0};
    int wrote = cluster_frame_encode_handshake_ack(frame, sizeof(frame), &ack);
    ASSERT_EQ_INT(wrote, (int) (XR_FRAME_HEADER_SIZE + 1u + payload_len));
    ASSERT_EQ_INT(frame[0], 0x00);
    ASSERT_EQ_INT(frame[1], 0x00);
    ASSERT_EQ_INT(frame[2], 0x00);
    ASSERT_EQ_INT(frame[3], (int) (1u + payload_len));
    ASSERT_EQ_INT(frame[4], XR_FRAME_HANDSHAKE_ACK);

    const uint8_t *p = frame + XR_FRAME_HEADER_SIZE + 1;
    ASSERT_EQ_INT(p[0], XR_CLUSTER_HANDSHAKE_VERSION);
    ASSERT_EQ_INT(p[1], (int) name_len);
    ASSERT_EQ_INT(memcmp(p + 2, name, name_len), 0);
    ASSERT_EQ_INT(memcmp(p + 2 + name_len, ack.nonce, XR_NONCE_SIZE), 0);
    ASSERT_EQ_INT(memcmp(p + 2 + name_len + XR_NONCE_SIZE, ack.proof, XR_PROOF_SIZE), 0);
    const uint8_t *flags = p + 2 + name_len + XR_NONCE_SIZE + XR_PROOF_SIZE;
    ASSERT_EQ_INT(flags[0], 0x0A);
    ASSERT_EQ_INT(flags[1], 0x0B);
    ASSERT_EQ_INT(flags[2], 0x0C);
    ASSERT_EQ_INT(flags[3], 0x0D);

    XrFrameHandshakeAck decoded;
    memset(&decoded, 0xFF, sizeof(decoded));
    ASSERT_EQ_INT(cluster_frame_decode_handshake_ack(p, (uint32_t) payload_len, &decoded), 0);
    ASSERT_EQ_INT(decoded.version, XR_CLUSTER_HANDSHAKE_VERSION);
    ASSERT_STR_EQ(decoded.name, name);
    ASSERT_EQ_INT(memcmp(decoded.nonce, ack.nonce, XR_NONCE_SIZE), 0);
    ASSERT_EQ_INT(memcmp(decoded.proof, ack.proof, XR_PROOF_SIZE), 0);
    ASSERT_EQ_UINT(decoded.flags, 0x0A0B0C0Du);

    ASSERT_EQ_INT(
        cluster_frame_encode_handshake_ack(frame, XR_FRAME_HEADER_SIZE + payload_len, &ack), -1);
}

/*
 * hsDone := proof[32] — no length octet, no version, nothing else.
 */
TEST(cluster_handshake_done_round_trips) {
    XrFrameHandshakeDone done;
    memset(&done, 0, sizeof(done));
    for (size_t i = 0; i < XR_PROOF_SIZE; i++)
        done.proof[i] = (uint8_t) (0xC0 + i);

    uint8_t frame[64] = {0};
    int wrote = cluster_frame_encode_handshake_done(frame, sizeof(frame), &done);
    ASSERT_EQ_INT(wrote, XR_FRAME_HEADER_SIZE + 1 + XR_PROOF_SIZE);
    ASSERT_EQ_INT(frame[0], 0x00);
    ASSERT_EQ_INT(frame[1], 0x00);
    ASSERT_EQ_INT(frame[2], 0x00);
    ASSERT_EQ_INT(frame[3], 1 + XR_PROOF_SIZE);
    ASSERT_EQ_INT(frame[4], XR_FRAME_HANDSHAKE_DONE);
    ASSERT_EQ_INT(memcmp(frame + XR_FRAME_HEADER_SIZE + 1, done.proof, XR_PROOF_SIZE), 0);

    XrFrameHandshakeDone decoded;
    memset(&decoded, 0, sizeof(decoded));
    ASSERT_EQ_INT(cluster_frame_decode_handshake_done(frame + XR_FRAME_HEADER_SIZE + 1,
                                                      XR_PROOF_SIZE, &decoded),
                  0);
    ASSERT_EQ_INT(memcmp(decoded.proof, done.proof, XR_PROOF_SIZE), 0);

    ASSERT_EQ_INT(
        cluster_frame_encode_handshake_done(frame, XR_FRAME_HEADER_SIZE + XR_PROOF_SIZE, &done),
        -1);
}

TEST(cluster_handshake_projection_shares_one_proof_flow) {
    static const char secret[] = "projection-secret";
    uint8_t client_nonce[XR_NONCE_SIZE];
    uint8_t server_nonce[XR_NONCE_SIZE];
    for (size_t i = 0; i < XR_NONCE_SIZE; i++) {
        client_nonce[i] = (uint8_t) (0x10 + i);
        server_nonce[i] = (uint8_t) (0x80 + i);
    }

    XrFrameHandshakeReq request;
    XrFrameHandshakeAck ack;
    XrFrameHandshakeDone done;
    uint8_t request_frame[256] = {0};
    uint8_t ack_frame[256] = {0};
    uint8_t done_frame[64] = {0};

    int request_length = xr_cluster_handshake_client_start(
        "client", 0x01020304u, client_nonce, &request, request_frame, sizeof(request_frame));
    ASSERT_TRUE(request_length > 0);
    ASSERT_STR_EQ(request.name, "client");
    ASSERT_EQ_UINT(request.flags, 0x01020304u);

    uint8_t type = 0;
    uint32_t payload_length = 0;
    ASSERT_EQ_INT(
        cluster_frame_read_header(request_frame, (size_t) request_length, &type, &payload_length),
        0);
    ASSERT_EQ_INT(type, XR_FRAME_HANDSHAKE_REQ);
    int ack_length = xr_cluster_handshake_server_accept_request(
        "server", secret, 0xA0B0C0D0u, server_nonce, request_frame + XR_FRAME_HEADER_SIZE + 1,
        payload_length, &request, &ack, ack_frame, sizeof(ack_frame));
    ASSERT_TRUE(ack_length > 0);
    ASSERT_STR_EQ(ack.name, "server");
    ASSERT_EQ_UINT(ack.flags, 0xA0B0C0D0u);

    ASSERT_EQ_INT(cluster_frame_read_header(ack_frame, (size_t) ack_length, &type, &payload_length),
                  0);
    ASSERT_EQ_INT(type, XR_FRAME_HANDSHAKE_ACK);
    int done_length = xr_cluster_handshake_client_accept_ack(
        secret, &request, ack_frame + XR_FRAME_HEADER_SIZE + 1, payload_length, &ack, &done,
        done_frame, sizeof(done_frame));
    ASSERT_TRUE(done_length > 0);

    ASSERT_EQ_INT(
        cluster_frame_read_header(done_frame, (size_t) done_length, &type, &payload_length), 0);
    ASSERT_EQ_INT(type, XR_FRAME_HANDSHAKE_DONE);
    ASSERT_TRUE(xr_cluster_handshake_server_accept_done(
        secret, &ack, done_frame + XR_FRAME_HEADER_SIZE + 1, payload_length));

    done_frame[XR_FRAME_HEADER_SIZE + 1] ^= 0x01;
    ASSERT_FALSE(xr_cluster_handshake_server_accept_done(
        secret, &ack, done_frame + XR_FRAME_HEADER_SIZE + 1, payload_length));

    ASSERT_EQ_INT(cluster_frame_read_header(ack_frame, (size_t) ack_length, &type, &payload_length),
                  0);
    ack_frame[XR_FRAME_HEADER_SIZE + 1 + 2 + strlen("server") + XR_NONCE_SIZE] ^= 0x01;
    ASSERT_EQ_INT(xr_cluster_handshake_client_accept_ack(
                      secret, &request, ack_frame + XR_FRAME_HEADER_SIZE + 1, payload_length, &ack,
                      &done, done_frame, sizeof(done_frame)),
                  -1);
}

/*
 * heartbeat := i64be(timestampMs) — shared by HEARTBEAT_PING and HEARTBEAT_PONG.
 */
TEST(cluster_heartbeat_round_trips) {
    const int64_t timestamp = INT64_C(0x0102030405060708);
    uint8_t frame[32] = {0};

    int wrote =
        cluster_frame_encode_heartbeat(frame, sizeof(frame), XR_FRAME_HEARTBEAT_PING, timestamp);
    ASSERT_EQ_INT(wrote, XR_FRAME_HEADER_SIZE + 1 + 8);
    ASSERT_EQ_INT(frame[0], 0x00);
    ASSERT_EQ_INT(frame[1], 0x00);
    ASSERT_EQ_INT(frame[2], 0x00);
    ASSERT_EQ_INT(frame[3], 9);
    ASSERT_EQ_INT(frame[4], XR_FRAME_HEARTBEAT_PING);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ_INT(frame[XR_FRAME_HEADER_SIZE + 1 + i], i + 1);

    int64_t decoded = 0;
    ASSERT_EQ_INT(cluster_frame_decode_heartbeat(frame + XR_FRAME_HEADER_SIZE + 1, 8, &decoded), 0);
    ASSERT_EQ_INT(decoded, timestamp);

    /* PONG carries the identical payload; only the type octet differs. */
    memset(frame, 0, sizeof(frame));
    ASSERT_EQ_INT(
        cluster_frame_encode_heartbeat(frame, sizeof(frame), XR_FRAME_HEARTBEAT_PONG, timestamp),
        XR_FRAME_HEADER_SIZE + 1 + 8);
    ASSERT_EQ_INT(frame[4], XR_FRAME_HEARTBEAT_PONG);
    for (int i = 0; i < 8; i++)
        ASSERT_EQ_INT(frame[XR_FRAME_HEADER_SIZE + 1 + i], i + 1);
    decoded = 0;
    ASSERT_EQ_INT(cluster_frame_decode_heartbeat(frame + XR_FRAME_HEADER_SIZE + 1, 8, &decoded), 0);
    ASSERT_EQ_INT(decoded, timestamp);

    /*
     * The field is signed. The codec routes it through uint64_t on the way out
     * and back through int64_t on the way in, so the sign bit is the part worth
     * pinning.
     */
    const int64_t negative = INT64_C(-1234567890123);
    memset(frame, 0, sizeof(frame));
    ASSERT_EQ_INT(
        cluster_frame_encode_heartbeat(frame, sizeof(frame), XR_FRAME_HEARTBEAT_PING, negative),
        XR_FRAME_HEADER_SIZE + 1 + 8);
    ASSERT_EQ_INT(frame[XR_FRAME_HEADER_SIZE + 1], 0xFF);
    decoded = 0;
    ASSERT_EQ_INT(cluster_frame_decode_heartbeat(frame + XR_FRAME_HEADER_SIZE + 1, 8, &decoded), 0);
    ASSERT_EQ_INT(decoded, negative);

    memset(frame, 0, sizeof(frame));
    ASSERT_EQ_INT(
        cluster_frame_encode_heartbeat(frame, sizeof(frame), XR_FRAME_HEARTBEAT_PING, INT64_MIN),
        XR_FRAME_HEADER_SIZE + 1 + 8);
    ASSERT_EQ_INT(frame[XR_FRAME_HEADER_SIZE + 1], 0x80);
    for (int i = 1; i < 8; i++)
        ASSERT_EQ_INT(frame[XR_FRAME_HEADER_SIZE + 1 + i], 0x00);
    decoded = 0;
    ASSERT_EQ_INT(cluster_frame_decode_heartbeat(frame + XR_FRAME_HEADER_SIZE + 1, 8, &decoded), 0);
    ASSERT_EQ_INT(decoded, INT64_MIN);

    ASSERT_EQ_INT(cluster_frame_encode_heartbeat(frame, XR_FRAME_HEADER_SIZE + 8,
                                                 XR_FRAME_HEARTBEAT_PING, timestamp),
                  -1);
}

TEST(cluster_frame_projection_is_backend_neutral) {
    const int64_t timestamp = INT64_C(0x0102030405060708);
    uint8_t frame[256] = {0};
    int frame_length =
        cluster_frame_encode_heartbeat(frame, sizeof(frame), XR_FRAME_HEARTBEAT_PING, timestamp);
    ASSERT_TRUE(frame_length > 0);

    uint8_t frame_type = 0;
    uint32_t payload_length = 0;
    ASSERT_EQ_INT(
        cluster_frame_read_header(frame, (size_t) frame_length, &frame_type, &payload_length), 0);
    XrClusterFrameProjection projection;
    ASSERT_TRUE(cluster_frame_project(frame_type, frame + XR_FRAME_HEADER_SIZE + 1, payload_length,
                                      &projection));
    ASSERT_EQ_INT(projection.kind, XR_CLUSTER_FRAME_HEARTBEAT_PING);
    ASSERT_TRUE(projection.heartbeat_timestamp == timestamp);
    ASSERT_EQ_UINT(projection.response_length, XR_FRAME_HEADER_SIZE + 1 + 8);

    ASSERT_EQ_INT(cluster_frame_read_header(projection.response, projection.response_length,
                                            &frame_type, &payload_length),
                  0);
    ASSERT_EQ_INT(frame_type, XR_FRAME_HEARTBEAT_PONG);
    int64_t response_timestamp = 0;
    ASSERT_EQ_INT(cluster_frame_decode_heartbeat(projection.response + XR_FRAME_HEADER_SIZE + 1,
                                                 payload_length, &response_timestamp),
                  0);
    ASSERT_TRUE(response_timestamp == timestamp);
    ASSERT_FALSE(cluster_frame_project(XR_FRAME_HEARTBEAT_PING, frame + XR_FRAME_HEADER_SIZE + 1,
                                       payload_length - 1, &projection));

    uint8_t envelope[XR_CLUSTER_ENVELOPE_HEADER_SIZE] = {0};
    frame_length = cluster_frame_write_transport(frame, sizeof(frame), 3, "events.user", 11,
                                                 envelope, sizeof(envelope));
    ASSERT_TRUE(frame_length > 0);
    ASSERT_EQ_INT(
        cluster_frame_read_header(frame, (size_t) frame_length, &frame_type, &payload_length), 0);
    ASSERT_TRUE(cluster_frame_project(frame_type, frame + XR_FRAME_HEADER_SIZE + 1, payload_length,
                                      &projection));
    ASSERT_EQ_INT(projection.kind, XR_CLUSTER_FRAME_TRANSPORT);
    ASSERT_STR_EQ(projection.transport.topic, "events.user");
    ASSERT_EQ_INT(projection.transport.hop_limit, 3);
    ASSERT_EQ_UINT(projection.transport.envelope_length, sizeof(envelope));

    frame_length = cluster_frame_encode_coro_exit(frame, sizeof(frame), "worker.7", "finished");
    ASSERT_TRUE(frame_length > 0);
    ASSERT_EQ_INT(
        cluster_frame_read_header(frame, (size_t) frame_length, &frame_type, &payload_length), 0);
    ASSERT_TRUE(cluster_frame_project(frame_type, frame + XR_FRAME_HEADER_SIZE + 1, payload_length,
                                      &projection));
    ASSERT_EQ_INT(projection.kind, XR_CLUSTER_FRAME_CORO_EXIT);
    ASSERT_STR_EQ(projection.coro_name, "worker.7");
    ASSERT_STR_EQ(projection.coro_reason, "finished");
}

/*
 * coroMon  := u8(len(name)) name
 * coroExit := u8(len(name)) name u8(len(reason)) reason
 */
TEST(cluster_coro_frames_round_trip) {
    static const char coro[] = "worker.7";
    static const char reason[] = "killed";
    const size_t coro_len = sizeof(coro) - 1u;
    const size_t reason_len = sizeof(reason) - 1u;

    uint8_t frame[128] = {0};
    int wrote =
        cluster_frame_encode_coro_monitor(frame, sizeof(frame), XR_FRAME_CORO_MONITOR, coro);
    ASSERT_EQ_INT(wrote, (int) (XR_FRAME_HEADER_SIZE + 1u + 1u + coro_len));
    ASSERT_EQ_INT(frame[0], 0x00);
    ASSERT_EQ_INT(frame[1], 0x00);
    ASSERT_EQ_INT(frame[2], 0x00);
    ASSERT_EQ_INT(frame[3], (int) (1u + 1u + coro_len));
    ASSERT_EQ_INT(frame[4], XR_FRAME_CORO_MONITOR);
    ASSERT_EQ_INT(frame[5], (int) coro_len);
    ASSERT_EQ_INT(memcmp(frame + 6, coro, coro_len), 0);

    char name_out[XR_CORO_NAME_MAX + 1];
    memset(name_out, 0, sizeof(name_out));
    ASSERT_EQ_INT(cluster_frame_decode_coro_monitor(frame + 5, (uint32_t) (1u + coro_len), name_out,
                                                    sizeof(name_out)),
                  0);
    ASSERT_STR_EQ(name_out, coro);

    /* CORO_DEMONITOR shares the payload grammar under a different type octet. */
    memset(frame, 0, sizeof(frame));
    ASSERT_EQ_INT(
        cluster_frame_encode_coro_monitor(frame, sizeof(frame), XR_FRAME_CORO_DEMONITOR, coro),
        (int) (XR_FRAME_HEADER_SIZE + 2u + coro_len));
    ASSERT_EQ_INT(frame[4], XR_FRAME_CORO_DEMONITOR);
    ASSERT_EQ_INT(frame[5], (int) coro_len);
    ASSERT_EQ_INT(memcmp(frame + 6, coro, coro_len), 0);

    memset(frame, 0, sizeof(frame));
    wrote = cluster_frame_encode_coro_exit(frame, sizeof(frame), coro, reason);
    ASSERT_EQ_INT(wrote, (int) (XR_FRAME_HEADER_SIZE + 1u + 1u + coro_len + 1u + reason_len));
    ASSERT_EQ_INT(frame[3], (int) (1u + 1u + coro_len + 1u + reason_len));
    ASSERT_EQ_INT(frame[4], XR_FRAME_CORO_EXIT);
    ASSERT_EQ_INT(frame[5], (int) coro_len);
    ASSERT_EQ_INT(memcmp(frame + 6, coro, coro_len), 0);
    ASSERT_EQ_INT(frame[6 + coro_len], (int) reason_len);
    ASSERT_EQ_INT(memcmp(frame + 7 + coro_len, reason, reason_len), 0);

    char reason_out[64];
    memset(name_out, 0, sizeof(name_out));
    memset(reason_out, 0, sizeof(reason_out));
    ASSERT_EQ_INT(cluster_frame_decode_coro_exit(frame + 5, (uint32_t) (2u + coro_len + reason_len),
                                                 name_out, sizeof(name_out), reason_out,
                                                 sizeof(reason_out)),
                  0);
    ASSERT_STR_EQ(name_out, coro);
    ASSERT_STR_EQ(reason_out, reason);

    /* A name past CORO_NAME_MAX never reaches the wire in either coro frame. */
    char long_name[XR_CORO_NAME_MAX + 2];
    memset(long_name, 'x', sizeof(long_name) - 1u);
    long_name[sizeof(long_name) - 1u] = '\0';
    ASSERT_EQ_INT(
        cluster_frame_encode_coro_monitor(frame, sizeof(frame), XR_FRAME_CORO_MONITOR, long_name),
        -1);
    ASSERT_EQ_INT(cluster_frame_encode_coro_exit(frame, sizeof(frame), long_name, reason), -1);
}

/*
 * Every decoder is fed one octet less than the shortest legal payload for its
 * frame, plus the length and capacity limits each one is required to enforce.
 */
TEST(cluster_frame_decoders_reject_truncated_payloads) {
    uint8_t payload[256];
    memset(payload, 0, sizeof(payload));

    /* The shortest handshake carries a one-byte printable node name. */
    const uint32_t req_min = 1u + 1u + XR_NONCE_SIZE + 4u;
    XrFrameHandshakeReq req;
    payload[0] = XR_CLUSTER_HANDSHAKE_VERSION;
    payload[1] = 1;
    payload[2] = 'a';
    ASSERT_EQ_INT(cluster_frame_decode_handshake_req(payload, req_min, &req), -1);
    ASSERT_EQ_INT(cluster_frame_decode_handshake_req(payload, req_min + 1u, &req), 0);

    /* hsAck minimum adds proof[32]. */
    const uint32_t ack_min = 1u + 1u + XR_NONCE_SIZE + XR_PROOF_SIZE + 4u;
    XrFrameHandshakeAck ack;
    ASSERT_EQ_INT(cluster_frame_decode_handshake_ack(payload, ack_min, &ack), -1);
    ASSERT_EQ_INT(cluster_frame_decode_handshake_ack(payload, ack_min + 1u, &ack), 0);

    XrFrameHandshakeDone done;
    ASSERT_EQ_INT(cluster_frame_decode_handshake_done(payload, XR_PROOF_SIZE - 1u, &done), -1);
    ASSERT_EQ_INT(cluster_frame_decode_handshake_done(payload, XR_PROOF_SIZE, &done), 0);

    int64_t timestamp = 0;
    ASSERT_EQ_INT(cluster_frame_decode_heartbeat(payload, 7, &timestamp), -1);
    ASSERT_EQ_INT(cluster_frame_decode_heartbeat(payload, 8, &timestamp), 0);

    char name_out[XR_CORO_NAME_MAX + 1];
    char reason_out[XR_CORO_NAME_MAX + 1];
    ASSERT_EQ_INT(cluster_frame_decode_coro_monitor(payload, 0, name_out, sizeof(name_out)), -1);
    payload[0] = 1;
    payload[1] = 'a';
    ASSERT_EQ_INT(cluster_frame_decode_coro_monitor(payload, 2, name_out, sizeof(name_out)), 0);
    ASSERT_EQ_INT(cluster_frame_decode_coro_exit(payload, 1, name_out, sizeof(name_out), reason_out,
                                                 sizeof(reason_out)),
                  -1);
    payload[2] = 0;
    ASSERT_EQ_INT(cluster_frame_decode_coro_exit(payload, 3, name_out, sizeof(name_out), reason_out,
                                                 sizeof(reason_out)),
                  0);

    /* A declared name longer than NODE_NAME_MAX cannot fit the struct's field. */
    memset(payload, 0, sizeof(payload));
    payload[0] = XR_CLUSTER_HANDSHAKE_VERSION;
    payload[1] = (uint8_t) (XR_NODE_NAME_MAX + 1);
    ASSERT_EQ_INT(cluster_frame_decode_handshake_req(payload, (uint32_t) sizeof(payload), &req),
                  -1);
    ASSERT_EQ_INT(cluster_frame_decode_handshake_ack(payload, (uint32_t) sizeof(payload), &ack),
                  -1);
    payload[1] = XR_NODE_NAME_MAX;
    memset(payload + 2, 'a', XR_NODE_NAME_MAX);
    ASSERT_EQ_INT(cluster_frame_decode_handshake_req(payload, req_min + XR_NODE_NAME_MAX, &req), 0);
    ASSERT_EQ_INT(cluster_frame_decode_handshake_ack(payload, ack_min + XR_NODE_NAME_MAX, &ack), 0);

    /* A name that fits the limit but overruns the frame is still rejected. */
    payload[1] = 10;
    ASSERT_EQ_INT(cluster_frame_decode_handshake_req(payload, req_min, &req), -1);
    ASSERT_EQ_INT(cluster_frame_decode_handshake_ack(payload, ack_min, &ack), -1);

    /* The coro decoders need room for the name plus its terminator. */
    memset(payload, 'a', sizeof(payload));
    payload[0] = 4;
    ASSERT_EQ_INT(cluster_frame_decode_coro_monitor(payload, 5, name_out, 4), -1);
    ASSERT_EQ_INT(cluster_frame_decode_coro_monitor(payload, 5, name_out, 5), 0);

    payload[0] = 4;
    payload[5] = 3;
    ASSERT_EQ_INT(
        cluster_frame_decode_coro_exit(payload, 9, name_out, 4, reason_out, sizeof(reason_out)),
        -1);
    ASSERT_EQ_INT(cluster_frame_decode_coro_exit(payload, 9, name_out, 5, reason_out, 3), -1);
    ASSERT_EQ_INT(cluster_frame_decode_coro_exit(payload, 9, name_out, 5, reason_out, 4), 0);

    /* Every fixed-shape decoder rejects trailing bytes rather than treating
     * them as a second, invisible message. */
    memset(payload, 0, sizeof(payload));
    ASSERT_EQ_INT(cluster_frame_decode_handshake_done(payload, XR_PROOF_SIZE + 1u, &done), -1);
    ASSERT_EQ_INT(cluster_frame_decode_heartbeat(payload, 9, &timestamp), -1);

    ASSERT_EQ_INT(cluster_frame_read_header(NULL, 0, NULL, NULL), -1);
    ASSERT_EQ_INT(cluster_frame_decode_heartbeat(NULL, 8, &timestamp), -1);
    ASSERT_EQ_INT(
        cluster_frame_encode_heartbeat(payload, sizeof(payload), XR_FRAME_HANDSHAKE_REQ, timestamp),
        -1);

    payload[0] = 1;
    payload[1] = 'a';
    payload[2] = 1;
    payload[3] = 0xFF;
    ASSERT_EQ_INT(cluster_frame_decode_coro_exit(payload, 4, name_out, sizeof(name_out), reason_out,
                                                 sizeof(reason_out)),
                  -1);
}

TEST(cluster_phi_projection_uses_bounded_history) {
    XrPhiDetector detector;
    xr_phi_detector_init(&detector, 5000.0);
    xr_phi_detector_record(&detector, 1000);
    xr_phi_detector_record(&detector, 6000);
    xr_phi_detector_record(&detector, 11000);
    ASSERT_EQ_INT(detector.sample_count, 2);
    ASSERT_TRUE(xr_phi_detector_value(&detector, 11000) < 0.001);
    double one_interval_late = xr_phi_detector_value(&detector, 16000);
    ASSERT_TRUE(one_interval_late > 0.30 && one_interval_late < 0.31);

    for (int i = 0; i < XR_PHI_WINDOW_SIZE + 20; i++)
        xr_phi_detector_record(&detector, 16000 + (int64_t) i * 5000);
    ASSERT_EQ_INT(detector.sample_count, XR_PHI_WINDOW_SIZE);
}

/*
 * WHY THIS CASE EXISTS:
 *   stdlib/cluster/cluster.xr is the normative statement of the cluster wire
 *   format and is compiled by both the VM and the AOT backend;
 *   xcluster_wire.h, cluster_internal.h and xtopic_registry.h are the native
 *   reader-loop projection of the same numbers. Nothing in the build makes one
 *   derive from the other, so the two can drift silently. The literals below
 *   are transcribed from the `export const` block of cluster.xr on purpose —
 *   they are not spelled with the C macros — which makes this case the one
 *   mechanical link between the two statements. Editing either side alone
 *   turns this case red.
 */
TEST(cluster_wire_constants_match_the_xray_surface) {
    ASSERT_EQ_INT(XR_CLUSTER_HANDSHAKE_VERSION, 6);
    ASSERT_EQ_INT(XR_CLUSTER_HANDSHAKE_TIMEOUT_MS, 5000);
    ASSERT_EQ_INT(XR_TOPIC_DEFAULT_HOP_LIMIT, 3);
    ASSERT_EQ_INT(XR_NODE_NAME_MAX, 63);
    ASSERT_EQ_INT(XR_ADDRESS_HOST_MAX, 255);
    ASSERT_EQ_INT(XR_CLUSTER_SECRET_MAX, 63);

    /* cluster.xr states TOPIC_PATTERN_MAX; the inbound projection mirrors it. */
    ASSERT_EQ_INT(XR_TOPIC_PATTERN_MAX, 127);

    ASSERT_EQ_INT(XR_CORO_NAME_MAX, 127);
    ASSERT_EQ_INT(XR_FRAME_HEADER_SIZE, 4);
    ASSERT_EQ_INT(XR_FRAME_MAX_PAYLOAD, 16777216);
    ASSERT_EQ_INT(XR_NONCE_SIZE, 16);
    ASSERT_EQ_INT(XR_PROOF_SIZE, 32);
    ASSERT_EQ_INT(XR_CLUSTER_ENVELOPE_HEADER_SIZE, 64);
    ASSERT_EQ_INT(XR_CLUSTER_SUBSCRIPTION_CAPACITY_MAX, 1048576);

    ASSERT_EQ_INT(XR_FRAME_HANDSHAKE_REQ, 0x01);
    ASSERT_EQ_INT(XR_FRAME_HANDSHAKE_ACK, 0x02);
    ASSERT_EQ_INT(XR_FRAME_HANDSHAKE_DONE, 0x03);
    ASSERT_EQ_INT(XR_FRAME_HANDSHAKE_ERR, 0x04);
    ASSERT_EQ_INT(XR_FRAME_HEARTBEAT_PING, 0x05);
    ASSERT_EQ_INT(XR_FRAME_HEARTBEAT_PONG, 0x06);
    ASSERT_EQ_INT(XR_FRAME_TRANSPORT_ENVELOPE, 0x07);
    ASSERT_EQ_INT(XR_FRAME_CORO_MONITOR, 0x08);
    ASSERT_EQ_INT(XR_FRAME_CORO_DEMONITOR, 0x09);
    ASSERT_EQ_INT(XR_FRAME_CORO_EXIT, 0x0A);
}

TEST(cluster_topic_projection_matches_the_xray_surface) {
    ASSERT_TRUE(xr_topic_name_valid("events.user"));
    ASSERT_FALSE(xr_topic_name_valid(""));
    ASSERT_FALSE(xr_topic_name_valid("events..user"));
    ASSERT_FALSE(xr_topic_name_valid("events.*"));
    ASSERT_FALSE(
        xr_topic_name_valid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));

    ASSERT_TRUE(xr_topic_pattern_matches("events.*", "events.user"));
    ASSERT_FALSE(xr_topic_pattern_matches("events.*", "events.user.login"));
    ASSERT_TRUE(xr_topic_pattern_matches("events.>", "events.user"));
    ASSERT_TRUE(xr_topic_pattern_matches("events.>", "events.user.login"));
    ASSERT_FALSE(xr_topic_pattern_matches("events.>", "events"));
    ASSERT_FALSE(xr_topic_pattern_matches("events.>.tail", "events.user.tail"));
}

TEST(cluster_tombstone_projection_expires_and_refreshes) {
    XrTombstoneRegistry *registry = xr_tombstone_registry_new(2, 1000);
    ASSERT_NOT_NULL(registry);

    ASSERT_TRUE(xr_tombstone_registry_add(registry, "node-a", 100));
    ASSERT_TRUE(xr_tombstone_registry_contains(registry, "node-a", 1099));
    ASSERT_EQ_INT(xr_tombstone_registry_count(registry, 1099), 1);
    ASSERT_FALSE(xr_tombstone_registry_contains(registry, "node-a", 1100));
    ASSERT_EQ_INT(xr_tombstone_registry_count(registry, 1100), 0);

    ASSERT_TRUE(xr_tombstone_registry_add(registry, "node-a", 2000));
    ASSERT_TRUE(xr_tombstone_registry_add(registry, "node-a", 2500));
    ASSERT_EQ_INT(xr_tombstone_registry_count(registry, 3499), 1);
    ASSERT_FALSE(xr_tombstone_registry_contains(registry, "node-a", 3500));

    xr_tombstone_registry_destroy(registry);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Cluster transport protocol");
RUN_TEST(cluster_transport_frame_is_byte_exact);
RUN_TEST(cluster_transport_frame_rejects_invalid_shapes);
RUN_TEST(cluster_frame_header_is_big_endian_length_prefix);
RUN_TEST(cluster_handshake_req_round_trips);
RUN_TEST(cluster_handshake_ack_round_trips);
RUN_TEST(cluster_handshake_done_round_trips);
RUN_TEST(cluster_handshake_projection_shares_one_proof_flow);
RUN_TEST(cluster_heartbeat_round_trips);
RUN_TEST(cluster_frame_projection_is_backend_neutral);
RUN_TEST(cluster_coro_frames_round_trip);
RUN_TEST(cluster_frame_decoders_reject_truncated_payloads);
RUN_TEST(cluster_phi_projection_uses_bounded_history);
RUN_TEST(cluster_wire_constants_match_the_xray_surface);
RUN_TEST(cluster_topic_projection_matches_the_xray_surface);
RUN_TEST(cluster_tombstone_projection_expires_and_refreshes);

TEST_MAIN_END()
