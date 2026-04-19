/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_health.c - Health monitoring, reconnection, gossip, tombstones
 *
 * KEY CONCEPT:
 *   Implements heartbeat checking (Phi Accrual), exponential backoff
 *   reconnection, gossip-based node discovery, and dead node tombstones.
 */

#include "cluster.h"
#include "cluster_node.h"
#include "../../include/xray_platform.h"  /* xr_random_bytes */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

/* ========== Health & Robustness ========== */

// Phi threshold: 8.0 is recommended by Akka (low false-positive rate)
#define XR_PHI_THRESHOLD 8.0

void xr_cluster_check_heartbeats(XrCluster *c) {
    if (!c) return;

    int64_t now = xr_cluster_now_ms();
    XrClusterNode *to_remove[64];
    int remove_count = 0;

    xr_spinlock_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node && remove_count < 64) {
        if (node->state == XR_NODE_CONNECTED) {
            // Use Phi Accrual detector if enough samples, else fallback
            if (node->phi.sample_count >= 3) {
                double phi = xr_phi_value(&node->phi, now);
                if (phi > XR_PHI_THRESHOLD) {
                    to_remove[remove_count++] = node;
                }
            } else {
                // Fallback to simple timeout for first few heartbeats
                int64_t elapsed = now - node->last_heartbeat_recv;
                if (elapsed > c->heartbeat_timeout_ms) {
                    node->missed_heartbeats++;
                    if ((int)node->missed_heartbeats >= c->max_missed_heartbeats) {
                        to_remove[remove_count++] = node;
                    }
                }
            }
        }
        node = node->next;
    }
    xr_spinlock_unlock(&c->nodes_lock);

    for (int i = 0; i < remove_count; i++) {
        XrClusterNode *dead = to_remove[i];
        xr_cluster_mark_dead(c, dead->name);
        xr_cluster_remove_all_subscribers_for_node(c, dead);
        xr_cluster_fire_monitors(c, dead->name);
        if (c->on_node_removed) c->on_node_removed(dead->name);
        xr_cluster_remove_node(c, dead);
        xr_cluster_node_free(dead);
    }
}

void xr_cluster_send_heartbeats(XrCluster *c) {
    if (!c) return;

    xr_spinlock_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node->state == XR_NODE_CONNECTED && node->conn) {
            xr_cluster_node_send_ping(node);
        }
        node = node->next;
    }
    xr_spinlock_unlock(&c->nodes_lock);
}

/*
 * Return a jittered backoff in [base/2, base*3/2) using a uniform
 * random sample. Jitter prevents "thundering herd" reconnects when
 * many nodes lose the same peer at the same instant — every retryer
 * picks an independent delay rather than all hammering the target on
 * identical millisecond boundaries.
 *
 * We draw 4 bytes from the platform CSPRNG. That is overkill for
 * jitter but keeps us on the same RNG we already trust for handshake
 * nonces, so there is one less quality tier in the codebase.
 */
static int jittered_backoff_ms(int base_ms) {
    if (base_ms <= 1) return base_ms;
    uint8_t rnd[4];
    xr_random_bytes(rnd, sizeof(rnd));
    uint32_t r = ((uint32_t)rnd[0] << 24) | ((uint32_t)rnd[1] << 16) |
                 ((uint32_t)rnd[2] << 8)  | (uint32_t)rnd[3];
    /* span = base_ms, offset = base_ms/2. Result is in [base/2, 3*base/2). */
    int span = base_ms;
    int offset = base_ms / 2;
    int jitter = (int)(r % (uint32_t)span);
    return offset + jitter;
}

/*
 * Sleep in small slices so an xr_cluster_stop() landing during backoff
 * is observed within XR_RECONNECT_SLICE_MS rather than after the full
 * (possibly 30s) delay. Any caller that wants true coroutine-friendly
 * reconnect should issue the retries from a coroutine and yield between
 * attempts; this function is documented as synchronous.
 */
#define XR_RECONNECT_SLICE_MS 100
static void interruptible_sleep_ms(XrCluster *c, int ms) {
    while (ms > 0 && atomic_load(&c->running)) {
        int chunk = ms > XR_RECONNECT_SLICE_MS ? XR_RECONNECT_SLICE_MS : ms;
        struct timespec ts = {
            .tv_sec  = chunk / 1000,
            .tv_nsec = (chunk % 1000) * 1000000L
        };
        nanosleep(&ts, NULL);
        ms -= chunk;
    }
}

int xr_cluster_reconnect(XrCluster *c, const char *host, uint16_t port,
                          int base_ms, int max_ms, int max_attempts) {
    if (!c) return -1;

    if (base_ms <= 0) base_ms = 500;
    if (max_ms <= 0) max_ms = 30000;
    if (max_attempts <= 0) max_attempts = 10;

    /*
     * Decorrelated-jitter-ish backoff: we grow the ceiling exponentially
     * (delay_ceiling *= 2, capped at max_ms) but actually sleep for a
     * jittered value drawn inside that ceiling. This is the pattern AWS
     * SDK recommends for reconnect loops; it balances per-attempt
     * variance with bounded total retry time.
     */
    int delay_ceiling = base_ms;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (!atomic_load(&c->running)) return -1;

        XrClusterNode *node = xr_cluster_node_new(NULL, host, port);
        if (!node) return -1;

        if (xr_cluster_node_connect(c, node) == 0) {
            if (xr_cluster_is_dead(c, node->name)) {
                xr_cluster_node_free(node);
                return -1;
            }
            xr_cluster_add_node(c, node);
            xr_cluster_node_start_writer(node, c->isolate);
            if (c->on_node_added) c->on_node_added(node->name);
            return 0;
        }

        xr_cluster_node_free(node);

        int sleep_ms = jittered_backoff_ms(delay_ceiling);
        if (sleep_ms > max_ms) sleep_ms = max_ms;
        interruptible_sleep_ms(c, sleep_ms);

        delay_ceiling *= 2;
        if (delay_ceiling > max_ms) delay_ceiling = max_ms;
    }
    return -1;
}

void xr_cluster_gossip_to_node(XrCluster *c, XrClusterNode *target) {
    if (!c || !target || target->state != XR_NODE_CONNECTED) return;

    // Build NODE_INFO payload: [count 2B] [name_len 1B, name, host_len 1B, host, port 2B] ...
    uint8_t payload[4096];
    uint8_t *p = payload;
    uint16_t count = 0;
    p += 2; // reserve for count

    xr_spinlock_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node && (size_t)(p - payload) < sizeof(payload) - 300) {
        if (node != target && node->state == XR_NODE_CONNECTED) {
            uint8_t name_len = (uint8_t)strlen(node->name);
            uint8_t host_len = (uint8_t)strlen(node->host);
            *p++ = name_len;
            memcpy(p, node->name, name_len); p += name_len;
            *p++ = host_len;
            memcpy(p, node->host, host_len); p += host_len;
            p[0] = (uint8_t)(node->port >> 8);
            p[1] = (uint8_t)(node->port);
            p += 2;
            count++;
        }
        node = node->next;
    }
    xr_spinlock_unlock(&c->nodes_lock);

    // Write count
    payload[0] = (uint8_t)(count >> 8);
    payload[1] = (uint8_t)(count);

    uint32_t payload_len = (uint32_t)(p - payload);
    xr_cluster_node_send_frame(target, XR_FRAME_NODE_INFO, payload, payload_len);
}

void xr_cluster_handle_node_info(XrCluster *c, const uint8_t *payload, uint32_t len) {
    if (!c || len < 2) return;

    uint16_t count = ((uint16_t)payload[0] << 8) | payload[1];
    const uint8_t *p = payload + 2;
    const uint8_t *end = payload + len;

    for (uint16_t i = 0; i < count && p < end; i++) {
        if (p >= end) break;
        uint8_t name_len = *p++;
        if (p + name_len > end) break;
        char name[XR_NODE_NAME_MAX + 1] = {0};
        memcpy(name, p, name_len); p += name_len;

        if (p >= end) break;
        uint8_t host_len = *p++;
        if (p + host_len + 2 > end) break;
        char host[256] = {0};
        memcpy(host, p, host_len); p += host_len;

        uint16_t port = ((uint16_t)p[0] << 8) | p[1];
        p += 2;

        // Skip if already connected or self or dead
        if (strcmp(name, c->self_name) == 0) continue;
        if (xr_cluster_find_node(c, name)) continue;
        if (xr_cluster_is_dead(c, name)) continue;

        // Try to connect (non-blocking attempt)
        XrClusterNode *new_node = xr_cluster_node_new(name, host, port);
        if (new_node && xr_cluster_node_connect(c, new_node) == 0) {
            xr_cluster_add_node(c, new_node);
            xr_cluster_node_start_writer(new_node, c->isolate);
            if (c->on_node_added) c->on_node_added(new_node->name);
        } else if (new_node) {
            xr_cluster_node_free(new_node);
        }
    }
}

/* ========== Tombstone Management ========== */

void xr_cluster_mark_dead(XrCluster *c, const char *name) {
    if (!c || !name) return;

    xr_spinlock_lock(&c->dead_nodes_lock);
    // Grow dynamic array if needed
    if (c->tombstone_count >= c->tombstone_cap) {
        int new_cap = c->tombstone_cap * 2;
        void *new_arr = xr_realloc(c->tombstones, (size_t)new_cap * sizeof(c->tombstones[0]));
        if (new_arr) {
            c->tombstones = new_arr;
            c->tombstone_cap = new_cap;
        } else {
            // Fallback: sweep oldest entry to make room
            if (c->tombstone_count > 0) {
                memmove(&c->tombstones[0], &c->tombstones[1],
                        (size_t)(c->tombstone_count - 1) * sizeof(c->tombstones[0]));
                c->tombstone_count--;
            }
        }
    }
    if (c->tombstone_count < c->tombstone_cap) {
        strncpy(c->tombstones[c->tombstone_count].name, name, XR_NODE_NAME_MAX);
        c->tombstones[c->tombstone_count].name[XR_NODE_NAME_MAX] = '\0';
        c->tombstones[c->tombstone_count].time = xr_cluster_now_ms();
        c->tombstone_count++;
    }
    xr_spinlock_unlock(&c->dead_nodes_lock);
}

bool xr_cluster_is_dead(XrCluster *c, const char *name) {
    if (!c || !name) return false;

    xr_spinlock_lock(&c->dead_nodes_lock);
    for (int i = 0; i < c->tombstone_count; i++) {
        if (strcmp(c->tombstones[i].name, name) == 0) {
            xr_spinlock_unlock(&c->dead_nodes_lock);
            return true;
        }
    }
    xr_spinlock_unlock(&c->dead_nodes_lock);
    return false;
}

void xr_cluster_sweep_tombstones(XrCluster *c, int64_t max_age_ms) {
    if (!c) return;

    int64_t now = xr_cluster_now_ms();

    xr_spinlock_lock(&c->dead_nodes_lock);
    int i = 0;
    while (i < c->tombstone_count) {
        if (now - c->tombstones[i].time > max_age_ms) {
            // Remove by swapping with last
            c->tombstone_count--;
            if (i < c->tombstone_count) {
                c->tombstones[i] = c->tombstones[c->tombstone_count];
            }
        } else {
            i++;
        }
    }
    xr_spinlock_unlock(&c->dead_nodes_lock);
}
