/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_health.c - Health monitoring and dead-node tombstones
 *
 * KEY CONCEPT:
 *   Implements heartbeat checking (Phi Accrual) and dead node tombstones.
 */

#include "cluster.h"
#include "cluster_node.h"

#include <string.h>

/* ========== Health & Robustness ========== */

// Phi threshold: 8.0 is recommended by Akka (low false-positive rate)
#define XR_PHI_THRESHOLD 8.0

void cluster_health_check_heartbeats(XrCluster *c) {
    if (!c)
        return;

    int64_t now = cluster_now_ms();

    /*
     * Collect dead nodes into a growing buffer, then act on them after
     * releasing nodes_lock. Earlier revisions used a fixed 64-entry
     * stack array (to_remove[64]) which silently dropped kills when a
     * large simultaneous network partition sent > 64 nodes past the
     * phi threshold at once — a nasty "some-but-not-all" failure mode
     * in big deployments. Growing on demand keeps the common case
     * allocation-free (inline 64-slot stack buffer) and the cold path
     * correct up to whatever number the OS can actually hold.
     */
    enum {
        XR_HB_INLINE = 64
    };
    XrClusterNode *inline_slots[XR_HB_INLINE];
    XrClusterNode **to_remove = inline_slots;
    int remove_count = 0;
    int remove_cap = XR_HB_INLINE;

    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        bool is_dead = false;
        if (node->state == XR_NODE_CONNECTED) {
            // Use Phi Accrual detector if enough samples, else fallback
            if (node->phi.sample_count >= 3) {
                double phi = cluster_phi_value(&node->phi, now);
                if (phi > XR_PHI_THRESHOLD)
                    is_dead = true;
            } else {
                // Fallback to simple timeout for first few heartbeats
                int64_t elapsed = now - node->last_heartbeat_recv;
                if (elapsed > c->heartbeat_timeout_ms) {
                    node->missed_heartbeats++;
                    if ((int) node->missed_heartbeats >= c->max_missed_heartbeats) {
                        is_dead = true;
                    }
                }
            }
        }
        if (is_dead) {
            if (remove_count >= remove_cap) {
                int new_cap = remove_cap * 2;
                XrClusterNode **grown;
                if (to_remove == inline_slots) {
                    grown = (XrClusterNode **) xr_malloc((size_t) new_cap * sizeof(*grown));
                    if (grown) {
                        memcpy(grown, to_remove, (size_t) remove_count * sizeof(*grown));
                    }
                } else {
                    grown =
                        (XrClusterNode **) xr_realloc(to_remove, (size_t) new_cap * sizeof(*grown));
                }
                if (!grown) {
                    /* OOM: stop collecting further victims this tick.
                     * They will be caught on the next heartbeat sweep.
                     * Intentionally non-fatal — health checking must
                     * never abort the cluster. */
                    break;
                }
                to_remove = grown;
                remove_cap = new_cap;
            }
            to_remove[remove_count++] = node;
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);

    for (int i = 0; i < remove_count; i++) {
        XrClusterNode *dead = to_remove[i];
        cluster_health_mark_dead(c, dead->name);
        cluster_subscriber_remove_all_for_node(c, dead);
        cluster_monitor_fire(c, dead->name);
        xr_cluster_remove_node(c, dead);
        xr_cluster_node_free(dead);
    }

    if (to_remove != inline_slots)
        xr_free(to_remove);
}

void cluster_health_send_heartbeats(XrCluster *c) {
    if (!c)
        return;

    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node->state == XR_NODE_CONNECTED && node->conn) {
            xr_cluster_node_send_ping(node);
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);
}

/* ========== Tombstone Management ========== */

void cluster_health_mark_dead(XrCluster *c, const char *name) {
    if (!c || !name)
        return;

    xr_amutex_lock(&c->dead_nodes_lock);
    // Grow dynamic array if needed
    if (c->tombstone_count >= c->tombstone_cap) {
        int new_cap = c->tombstone_cap * 2;
        void *new_arr = xr_realloc(c->tombstones, (size_t) new_cap * sizeof(c->tombstones[0]));
        if (new_arr) {
            c->tombstones = new_arr;
            c->tombstone_cap = new_cap;
        } else {
            // Fallback: sweep oldest entry to make room
            if (c->tombstone_count > 0) {
                memmove(&c->tombstones[0], &c->tombstones[1],
                        (size_t) (c->tombstone_count - 1) * sizeof(c->tombstones[0]));
                c->tombstone_count--;
            }
        }
    }
    if (c->tombstone_count < c->tombstone_cap) {
        strncpy(c->tombstones[c->tombstone_count].name, name, XR_NODE_NAME_MAX);
        c->tombstones[c->tombstone_count].name[XR_NODE_NAME_MAX] = '\0';
        c->tombstones[c->tombstone_count].time = cluster_now_ms();
        c->tombstone_count++;
    }
    xr_amutex_unlock(&c->dead_nodes_lock);
}

bool cluster_health_is_dead(XrCluster *c, const char *name) {
    if (!c || !name)
        return false;

    xr_amutex_lock(&c->dead_nodes_lock);
    for (int i = 0; i < c->tombstone_count; i++) {
        if (strcmp(c->tombstones[i].name, name) == 0) {
            xr_amutex_unlock(&c->dead_nodes_lock);
            return true;
        }
    }
    xr_amutex_unlock(&c->dead_nodes_lock);
    return false;
}
