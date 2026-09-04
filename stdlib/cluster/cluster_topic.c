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
 *   cluster.xr decides topic grammar, local/remote publication order, result
 *   precedence and hop budgets. This file only projects an inbound socket
 *   frame and broadcasts bytes to connected native transports.
 */

#include "cluster_internal.h"

#include "../../src/coro/xtopic_registry.h"

#include <string.h>

XrClusterDelivery cluster_transport_broadcast(XrCluster *cluster, uint64_t excluded_generation,
                                              uint8_t hop_limit, const char *topic,
                                              const uint8_t *envelope, uint32_t envelope_length) {
    if (!cluster || !topic || !envelope)
        return XR_CLUSTER_DELIVERY_UNAVAILABLE;
    uint8_t topic_length = (uint8_t) strlen(topic);
    int connected = 0;
    int accepted = 0;
    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *node = cluster->nodes; node; node = node->next) {
        if ((excluded_generation != 0 && node->generation_token == excluded_generation) ||
            node->state != XR_NODE_CONNECTED)
            continue;
        connected++;
        if (cluster_node_send_transport_frame(node, hop_limit, topic, topic_length, envelope,
                                              envelope_length) == 0)
            accepted++;
    }
    xr_amutex_unlock(&cluster->nodes_lock);
    if (accepted > 0)
        return XR_CLUSTER_DELIVERY_ACCEPTED;
    if (connected > 0)
        return XR_CLUSTER_DELIVERY_OVERLOADED;
    return XR_CLUSTER_DELIVERY_DISCONNECTED;
}
