/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_monitor.c - Native transport projection for coroutine monitoring
 *
 * Xray constructs notification channels and owns target admission. The
 * synchronized monitor registry and scheduler wait live in src/coro; this file
 * only connects those providers to cluster transport frames.
 */

#include "cluster_internal.h"

#include "../../src/coro/xcoro_monitor_forward.h"
#include "../../src/coro/xmonitor_registry.h"
#include "../../src/runtime/object/xstring.h"

#include <string.h>

typedef struct XrClusterCoroForward {
    XrClusterNode *node;
    char coroutine_name[XR_CORO_NAME_MAX + 1];
} XrClusterCoroForward;

static bool cluster_monitor_send_exit(XrClusterNode *node, const char *coroutine_name,
                                      const char *reason) {
    if (!node || node->state != XR_NODE_CONNECTED)
        return false;
    uint8_t frame[256];
    int length = cluster_frame_encode_coro_exit(frame, sizeof(frame), coroutine_name, reason);
    return length > 0 && cluster_node_enqueue(node, frame, (uint32_t) length) == 0;
}

static void cluster_monitor_forward_destroy(void *context) {
    XrClusterCoroForward *forward = (XrClusterCoroForward *) context;
    if (!forward)
        return;
    cluster_node_release(forward->node);
    xr_free(forward);
}

static void cluster_monitor_forward_exit(void *context, XrValue reason_value) {
    XrClusterCoroForward *forward = (XrClusterCoroForward *) context;
    if (!forward)
        return;
    const char *reason = XR_IS_STRING(reason_value) ? XR_TO_STRING(reason_value)->data : "normal";
    (void) cluster_monitor_send_exit(forward->node, forward->coroutine_name, reason);
}

bool cluster_monitor_register_remote(XrCluster *cluster, const char *node_name,
                                     const char *coroutine_name, XrChannel *channel) {
    if (!cluster || !node_name || !coroutine_name || !channel)
        return false;
    XrClusterNode *node = cluster_node_find(cluster, node_name);
    if (!node)
        return false;
    if (node->state != XR_NODE_CONNECTED ||
        !xr_monitor_registry_add_remote(cluster->monitors, node_name, coroutine_name, channel)) {
        cluster_node_release(node);
        return false;
    }

    uint8_t frame[256];
    int length = cluster_frame_encode_coro_monitor(frame, sizeof(frame), XR_FRAME_CORO_MONITOR,
                                                   coroutine_name);
    bool queued = length > 0 && cluster_node_enqueue(node, frame, (uint32_t) length) == 0;
    if (!queued)
        (void) xr_monitor_registry_remove_remote(cluster->monitors, node_name, coroutine_name,
                                                 channel);
    cluster_node_release(node);
    return queued;
}

void cluster_monitor_handle_coro_request(XrCluster *cluster, XrClusterNode *node,
                                         const char *coroutine_name) {
    if (!cluster || !cluster->isolate || !node || !coroutine_name)
        return;
    XrClusterCoroForward *forward = (XrClusterCoroForward *) xr_calloc(1, sizeof(*forward));
    if (!forward) {
        (void) cluster_monitor_send_exit(node, coroutine_name, "noproc");
        return;
    }
    forward->node = node;
    cluster_node_retain(node);
    strncpy(forward->coroutine_name, coroutine_name, XR_CORO_NAME_MAX);
    forward->coroutine_name[XR_CORO_NAME_MAX] = '\0';
    if (!xr_coro_monitor_forward(cluster->isolate, coroutine_name, cluster_monitor_forward_exit,
                                 forward, cluster_monitor_forward_destroy))
        (void) cluster_monitor_send_exit(node, coroutine_name, "noproc");
}
