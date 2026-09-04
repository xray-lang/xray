/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cluster_native_providers.c - Mechanical cluster provider tests
 *
 * Protocol, handshake and authentication known answers belong to the Xray
 * cluster source tests. This target covers only native storage/queue kernels.
 */

#include "../test_framework.h"
#include "../../../src/coro/xcluster_output_queue.h"

TEST(cluster_output_queue_reports_raw_admission_reasons) {
    uint8_t byte = 0x2A;
    XrClusterOutputQueue *queue = xr_cluster_output_queue_new(1);
    ASSERT_NOT_NULL(queue);

    ASSERT_EQ_INT(xr_cluster_output_queue_push_copy(queue, &byte, 1), XR_CLUSTER_OUTPUT_ACCEPTED);
    ASSERT_EQ_INT(xr_cluster_output_queue_push_copy(queue, &byte, 1), XR_CLUSTER_OUTPUT_FULL);
    xr_cluster_output_queue_stop(queue);
    ASSERT_EQ_INT(xr_cluster_output_queue_push_copy(queue, &byte, 1), XR_CLUSTER_OUTPUT_STOPPED);
    xr_cluster_output_queue_destroy(queue);

    ASSERT_EQ_INT(xr_cluster_output_queue_push_copy(NULL, &byte, 1),
                  XR_CLUSTER_OUTPUT_RESOURCE_UNAVAILABLE);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Cluster native providers");
RUN_TEST(cluster_output_queue_reports_raw_admission_reasons);

TEST_MAIN_END()
