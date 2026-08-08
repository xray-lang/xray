/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cluster_proto.c - Cluster transport frame construction tests
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
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Cluster transport protocol");
RUN_TEST(cluster_transport_frame_is_byte_exact);
RUN_TEST(cluster_transport_frame_rejects_invalid_shapes);

TEST_MAIN_END()
