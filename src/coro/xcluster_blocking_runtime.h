/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcluster_blocking_runtime.h - Blocking cluster service provider
 *
 * KEY CONCEPT:
 *   Standalone runtimes delegate socket, thread and channel coordination to
 *   this opaque provider. Protocol and API policy remain in cluster.xr.
 */

#ifndef XR_CORO_CLUSTER_BLOCKING_RUNTIME_H
#define XR_CORO_CLUSTER_BLOCKING_RUNTIME_H

#include "../base/xdefs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct XrChannel;
struct XrTopicRegistry;
typedef struct XrClusterBlockingRuntime XrClusterBlockingRuntime;

typedef enum {
    XR_CLUSTER_BLOCKING_DELIVERY_ACCEPTED = 0,
    XR_CLUSTER_BLOCKING_DELIVERY_INVALID_TOPIC = 1,
    XR_CLUSTER_BLOCKING_DELIVERY_INVALID_ENVELOPE = 2,
    XR_CLUSTER_BLOCKING_DELIVERY_UNAVAILABLE = 3,
    XR_CLUSTER_BLOCKING_DELIVERY_OVERLOADED = 4,
    XR_CLUSTER_BLOCKING_DELIVERY_DISCONNECTED = 5,
} XrClusterBlockingDelivery;

typedef struct {
    const char *name;
    size_t name_length;
    const char *secret;
    size_t secret_length;
    uint16_t port;
    size_t output_queue_high_watermark;
    struct XrTopicRegistry *topics;
} XrClusterBlockingRuntimeConfig;

/* A successful construction transfers ownership of config->topics. */
XR_FUNC XrClusterBlockingRuntime *
xr_cluster_blocking_runtime_new(const XrClusterBlockingRuntimeConfig *config,
                                uint16_t *actual_port);
XR_FUNC bool xr_cluster_blocking_runtime_start(XrClusterBlockingRuntime *runtime);
XR_FUNC void xr_cluster_blocking_runtime_destroy(void *runtime);
XR_FUNC bool xr_cluster_blocking_runtime_join(XrClusterBlockingRuntime *runtime, const char *host,
                                              size_t host_length, uint16_t port);
XR_FUNC XrClusterBlockingDelivery xr_cluster_blocking_runtime_publish_local(
    XrClusterBlockingRuntime *runtime, const char *topic, size_t topic_length,
    const uint8_t *envelope, uint32_t envelope_length);
XR_FUNC XrClusterBlockingDelivery xr_cluster_blocking_runtime_publish_remote(
    XrClusterBlockingRuntime *runtime, uint8_t hop_limit, const char *topic, size_t topic_length,
    const uint8_t *envelope, uint32_t envelope_length);
XR_FUNC struct XrChannel *xr_cluster_blocking_runtime_subscribe(XrClusterBlockingRuntime *runtime,
                                                                const char *pattern,
                                                                size_t pattern_length,
                                                                uint32_t capacity);

#endif  // XR_CORO_CLUSTER_BLOCKING_RUNTIME_H
