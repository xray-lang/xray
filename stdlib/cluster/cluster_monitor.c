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

#include "../../src/coro/xmonitor_registry.h"

#include <string.h>

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
    bool queued =
        length > 0 && xr_cluster_output_queue_push_copy(node->outq, frame, (uint32_t) length) == 0;
    if (!queued)
        (void) xr_monitor_registry_remove_remote(cluster->monitors, node_name, coroutine_name,
                                                 channel);
    cluster_node_release(node);
    return queued;
}
