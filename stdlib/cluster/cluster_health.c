/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_health.c - Locked transport projection of Xray health policy
 *
 * cluster.xr owns cadence, warm-up, threshold, retry and tombstone retention
 * policy. This provider projects one tick onto native node state and sockets.
 */

#include "cluster_internal.h"

#include "../../src/coro/xmonitor_registry.h"
#include "../../src/coro/xtombstone_registry.h"
#include "../../src/os/os_time.h"

#include <string.h>

void cluster_health_tick(XrCluster *cluster, int64_t heartbeat_timeout_ms,
                         int64_t max_missed_heartbeats, int64_t phi_min_samples,
                         double phi_threshold) {
    if (!cluster)
        return;
    enum {
        INLINE_CANDIDATES = 64
    };
    XrClusterNode *inline_candidates[INLINE_CANDIDATES];
    XrClusterNode **candidates = inline_candidates;
    uint32_t candidate_count = 0;
    uint32_t candidate_capacity = INLINE_CANDIDATES;
    int64_t now = (int64_t) xr_time_monotonic_ms();

    xr_amutex_lock(&cluster->nodes_lock);
    for (XrClusterNode *node = cluster->nodes; node; node = node->next) {
        if (node->state != XR_NODE_CONNECTED)
            continue;
        if (node->conn)
            (void) cluster_node_send_ping(node);

        bool dead = false;
        if (node->phi.sample_count >= phi_min_samples) {
            dead = xr_phi_detector_value(&node->phi, now) > phi_threshold;
        } else if (now - node->last_heartbeat_recv > heartbeat_timeout_ms) {
            node->missed_heartbeats++;
            dead = (int64_t) node->missed_heartbeats >= max_missed_heartbeats;
        }
        if (!dead)
            continue;

        if (candidate_count == candidate_capacity) {
            uint32_t new_capacity =
                candidate_capacity <= UINT32_MAX / 2 ? candidate_capacity * 2 : UINT32_MAX;
            XrClusterNode **grown = NULL;
            if (new_capacity > candidate_capacity &&
                (size_t) new_capacity <= SIZE_MAX / sizeof(*grown)) {
                if (candidates == inline_candidates) {
                    grown = (XrClusterNode **) xr_malloc((size_t) new_capacity * sizeof(*grown));
                    if (grown)
                        memcpy(grown, candidates, (size_t) candidate_count * sizeof(*grown));
                } else {
                    grown = (XrClusterNode **) xr_realloc(candidates,
                                                          (size_t) new_capacity * sizeof(*grown));
                }
            }
            if (!grown)
                break;
            candidates = grown;
            candidate_capacity = new_capacity;
        }
        cluster_node_retain(node);
        candidates[candidate_count++] = node;
    }
    xr_amutex_unlock(&cluster->nodes_lock);

    for (uint32_t i = 0; i < candidate_count; i++) {
        XrClusterNode *dead = candidates[i];
        (void) xr_tombstone_registry_add(cluster->tombstones, dead->name, now);
        xr_monitor_registry_notify_node(cluster->monitors, cluster->isolate, dead->name);
        if (cluster_node_remove(cluster, dead)) {
            cluster_node_shutdown(dead);
            cluster_node_release(dead);
        }
        cluster_node_release(dead);
    }
    if (candidates != inline_candidates)
        xr_free(candidates);
}
