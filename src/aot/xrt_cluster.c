/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_cluster.c - Standalone AOT cluster ABI leaves
 *
 * KEY CONCEPT:
 *   These functions translate AOT tagged values into the backend-neutral
 *   blocking cluster provider. No cluster protocol or lifecycle policy lives
 *   at this ABI boundary.
 */

#include "xrt_cluster.h"

#include "../coro/xaot_coro.h"
#include "../coro/xchannel.h"
#include "../coro/xcluster_blocking_runtime.h"
#include "../coro/xtopic_registry.h"

#include <limits.h>

XrValue xrt_cluster_start(const char *name, int64_t name_len, XrValue port_value,
                          const char *secret, int64_t secret_len, XrValue tls_enabled,
                          const char *ca_file, int64_t ca_file_len, const char *cert_file,
                          int64_t cert_file_len, const char *key_file, int64_t key_file_len,
                          XrValue insecure, XrValue heartbeat_interval_ms,
                          XrValue heartbeat_timeout_ms, XrValue max_missed_heartbeats,
                          XrValue phi_min_samples, XrValue phi_threshold,
                          XrValue queue_and_topic_limits, XrValue tombstone_retention_ms) {
    (void) ca_file;
    (void) ca_file_len;
    (void) cert_file;
    (void) cert_file_len;
    (void) key_file;
    (void) key_file_len;
    (void) insecure;
    XrAotRuntime *aot = xr_aot_runtime_current();
    if (!aot || name_len < 0 || secret_len < 0 || !XR_IS_INT(port_value) ||
        !XR_IS_BOOL(tls_enabled) || XR_TO_BOOL(tls_enabled) || !XR_IS_INT(heartbeat_interval_ms) ||
        !XR_IS_INT(heartbeat_timeout_ms) || !XR_IS_INT(max_missed_heartbeats) ||
        !XR_IS_INT(phi_min_samples) || !XR_IS_FLOAT(phi_threshold) ||
        !XR_IS_INT(queue_and_topic_limits) ||
        !XR_IS_INT(tombstone_retention_ms))
        return XR_FALSE_VAL;
    int64_t port = XR_TO_INT(port_value);
    int64_t packed_limits_value = XR_TO_INT(queue_and_topic_limits);
    uint64_t packed_limits = packed_limits_value > 0 ? (uint64_t) packed_limits_value : 0;
    uint64_t queue_high_watermark = packed_limits >> 32;
    uint32_t fanout_max = (uint32_t) packed_limits;
    if (port < 0 || port > UINT16_MAX || queue_high_watermark == 0 ||
        queue_high_watermark > SIZE_MAX || fanout_max == 0)
        return XR_FALSE_VAL;
    XrTopicRegistry *topics = xr_topic_registry_new_aot(aot, fanout_max);
    if (!topics)
        return XR_FALSE_VAL;
    XrClusterBlockingRuntimeConfig config = {
        .name = name,
        .name_length = (size_t) name_len,
        .secret = secret,
        .secret_length = (size_t) secret_len,
        .port = (uint16_t) port,
        .output_queue_high_watermark = (size_t) queue_high_watermark,
        .topics = topics,
    };
    uint16_t actual_port = 0;
    XrClusterBlockingRuntime *runtime = xr_cluster_blocking_runtime_new(&config, &actual_port);
    (void) actual_port;
    if (!runtime) {
        xr_topic_registry_destroy(topics);
        return XR_FALSE_VAL;
    }
    if (!xr_aot_runtime_service_install(aot, XR_AOT_SERVICE_SLOT_CLUSTER, runtime,
                                        xr_cluster_blocking_runtime_destroy)) {
        xr_cluster_blocking_runtime_destroy(runtime);
        return XR_FALSE_VAL;
    }
    if (!xr_cluster_blocking_runtime_start(runtime)) {
        (void) xr_aot_runtime_service_remove(aot, XR_AOT_SERVICE_SLOT_CLUSTER);
        return XR_FALSE_VAL;
    }
    return XR_TRUE_VAL;
}

XrValue xrt_cluster_join(const char *host, int64_t host_len, XrValue port_value) {
    XrAotRuntime *aot = xr_aot_runtime_current();
    if (!aot || host_len < 0 || !XR_IS_INT(port_value))
        return XR_FALSE_VAL;
    int64_t port = XR_TO_INT(port_value);
    if (port < 0 || port > UINT16_MAX)
        return XR_FALSE_VAL;
    XrClusterBlockingRuntime *runtime = (XrClusterBlockingRuntime *) xr_aot_runtime_service_acquire(
        aot, XR_AOT_SERVICE_SLOT_CLUSTER);
    if (!runtime)
        return XR_FALSE_VAL;
    bool joined =
        xr_cluster_blocking_runtime_join(runtime, host, (size_t) host_len, (uint16_t) port);
    xr_aot_runtime_service_release(aot, XR_AOT_SERVICE_SLOT_CLUSTER);
    return joined ? XR_TRUE_VAL : XR_FALSE_VAL;
}

XrValue xrt_cluster_stop(void) {
    XrAotRuntime *aot = xr_aot_runtime_current();
    if (aot)
        (void) xr_aot_runtime_service_remove(aot, XR_AOT_SERVICE_SLOT_CLUSTER);
    return XR_NULL_VAL;
}

int64_t xrt_cluster_publish_local(const char *topic, int64_t topic_len, XrValue envelope) {
    XrAotRuntime *aot = xr_aot_runtime_current();
    if (!aot || topic_len < 0)
        return XR_CLUSTER_BLOCKING_DELIVERY_UNAVAILABLE;
    XrClusterBlockingRuntime *runtime = (XrClusterBlockingRuntime *) xr_aot_runtime_service_acquire(
        aot, XR_AOT_SERVICE_SLOT_CLUSTER);
    if (!runtime)
        return XR_CLUSTER_BLOCKING_DELIVERY_UNAVAILABLE;
    const XrAotValueOps *values = xr_aot_runtime_value_ops(aot);
    const uint8_t *bytes = NULL;
    size_t length = 0;
    XrClusterBlockingDelivery result = XR_CLUSTER_BLOCKING_DELIVERY_INVALID_ENVELOPE;
    if (values && values->buffer_bytes && values->buffer_bytes(envelope, &bytes, &length) &&
        length <= UINT32_MAX) {
        result = xr_cluster_blocking_runtime_publish_local(runtime, topic, (size_t) topic_len,
                                                           bytes, (uint32_t) length);
    }
    xr_aot_runtime_service_release(aot, XR_AOT_SERVICE_SLOT_CLUSTER);
    return result;
}

int64_t xrt_cluster_publish_remote(const char *topic, int64_t topic_len, XrValue envelope,
                                   XrValue hop_limit) {
    XrAotRuntime *aot = xr_aot_runtime_current();
    int64_t hop = XR_IS_INT(hop_limit) ? XR_TO_INT(hop_limit) : -1;
    if (!aot || topic_len < 0 || hop < 0 || hop > UINT8_MAX)
        return XR_CLUSTER_BLOCKING_DELIVERY_INVALID_TOPIC;
    XrClusterBlockingRuntime *runtime = (XrClusterBlockingRuntime *) xr_aot_runtime_service_acquire(
        aot, XR_AOT_SERVICE_SLOT_CLUSTER);
    if (!runtime)
        return XR_CLUSTER_BLOCKING_DELIVERY_UNAVAILABLE;
    const XrAotValueOps *values = xr_aot_runtime_value_ops(aot);
    const uint8_t *bytes = NULL;
    size_t length = 0;
    XrClusterBlockingDelivery result = XR_CLUSTER_BLOCKING_DELIVERY_INVALID_ENVELOPE;
    if (values && values->buffer_bytes && values->buffer_bytes(envelope, &bytes, &length) &&
        length <= UINT32_MAX) {
        result = xr_cluster_blocking_runtime_publish_remote(
            runtime, (uint8_t) hop, topic, (size_t) topic_len, bytes, (uint32_t) length);
    }
    xr_aot_runtime_service_release(aot, XR_AOT_SERVICE_SLOT_CLUSTER);
    return result;
}

XrValue xrt_cluster_listen(const char *pattern, int64_t pattern_len, XrValue capacity_value) {
    XrAotRuntime *aot = xr_aot_runtime_current();
    int64_t capacity = XR_IS_INT(capacity_value) ? XR_TO_INT(capacity_value) : -1;
    if (!aot || pattern_len < 0 || capacity <= 0 || capacity > UINT32_MAX)
        return XR_NULL_VAL;
    XrClusterBlockingRuntime *runtime = (XrClusterBlockingRuntime *) xr_aot_runtime_service_acquire(
        aot, XR_AOT_SERVICE_SLOT_CLUSTER);
    if (!runtime)
        return XR_NULL_VAL;
    XrChannel *channel = xr_cluster_blocking_runtime_subscribe(
        runtime, pattern, (size_t) pattern_len, (uint32_t) capacity);
    xr_aot_runtime_service_release(aot, XR_AOT_SERVICE_SLOT_CLUSTER);
    return channel ? xr_value_from_channel(channel) : XR_NULL_VAL;
}
