/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_topic.c - Native cluster transport projection for Xray topic policy
 *
 * KEY CONCEPT:
 *   cluster.xr decides topic grammar, constructs the complete transport frame,
 *   and owns publication order, result precedence and hop budgets. This file
 *   only broadcasts those bytes to connected native transports.
 */

#include "cluster_internal.h"

#include "../../src/coro/xtopic_registry.h"

XrClusterDelivery cluster_transport_broadcast(XrCluster *cluster, uint64_t excluded_generation,
                                              const uint8_t *wire, uint32_t wire_length) {
    if (!cluster || !wire || wire_length == 0)
        return XR_CLUSTER_DELIVERY_UNAVAILABLE;
    int connected = 0;
    int accepted = 0;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *node = cluster->nodes; node; node = node->next) {
        if ((excluded_generation != 0 && node->generation_token == excluded_generation) ||
            node->state != XR_NODE_CONNECTED)
            continue;
        connected++;
        if (xr_cluster_output_queue_push_copy(node->outq, wire, wire_length) == 0)
            accepted++;
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    if (accepted > 0)
        return XR_CLUSTER_DELIVERY_ACCEPTED;
    if (connected > 0)
        return XR_CLUSTER_DELIVERY_OVERLOADED;
    return XR_CLUSTER_DELIVERY_DISCONNECTED;
}
