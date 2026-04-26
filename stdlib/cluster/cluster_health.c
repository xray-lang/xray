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
#include "../../include/xray_platform.h" /* xr_random_bytes */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

/* ========== Health & Robustness ========== */

// Phi threshold: 8.0 is recommended by Akka (low false-positive rate)
#define XR_PHI_THRESHOLD 8.0

void xr_cluster_check_heartbeats(XrCluster *c) {
    if (!c)
        return;

    int64_t now = xr_cluster_now_ms();

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

    xr_mutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        bool is_dead = false;
        if (node->state == XR_NODE_CONNECTED) {
            // Use Phi Accrual detector if enough samples, else fallback
            if (node->phi.sample_count >= 3) {
                double phi = xr_phi_value(&node->phi, now);
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
    xr_mutex_unlock(&c->nodes_lock);

    for (int i = 0; i < remove_count; i++) {
        XrClusterNode *dead = to_remove[i];
        xr_cluster_mark_dead(c, dead->name);
        xr_cluster_remove_all_subscribers_for_node(c, dead);
        xr_cluster_fire_monitors(c, dead->name);
        if (c->on_node_removed)
            c->on_node_removed(dead->name);
        xr_cluster_remove_node(c, dead);
        xr_cluster_node_free(dead);
    }

    if (to_remove != inline_slots)
        xr_free(to_remove);
}

void xr_cluster_send_heartbeats(XrCluster *c) {
    if (!c)
        return;

    xr_mutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node->state == XR_NODE_CONNECTED && node->conn) {
            xr_cluster_node_send_ping(node);
        }
        node = node->next;
    }
    xr_mutex_unlock(&c->nodes_lock);
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
    if (base_ms <= 1)
        return base_ms;
    uint8_t rnd[4];
    xr_random_bytes(rnd, sizeof(rnd));
    uint32_t r = ((uint32_t) rnd[0] << 24) | ((uint32_t) rnd[1] << 16) | ((uint32_t) rnd[2] << 8) |
                 (uint32_t) rnd[3];
    /* span = base_ms, offset = base_ms/2. Result is in [base/2, 3*base/2). */
    int span = base_ms;
    int offset = base_ms / 2;
    int jitter = (int) (r % (uint32_t) span);
    return offset + jitter;
}

/*
 * Sleep in a way that yields the coroutine rather than pinning the
 * worker thread, and that wakes immediately when xr_cluster_stop
 * closes the cluster's stop_pipe. Behaviour:
 *   - If called from a coroutine while the cluster is running,
 *     delegates to xr_cluster_sleep_interruptible which drives a
 *     read(2) + timer-wheel deadline on stop_pipe[0]. Worker stays
 *     free to run other coroutines.
 *   - If stop_pipe was never provisioned (rare — pipe() failure at
 *     start_ex), falls back to a 100ms nanosleep slice loop inside
 *     xr_cluster_sleep_interruptible so stop-latency is still
 *     bounded.
 *
 * Callers should treat the function as synchronous to their control
 * flow (backoff elapsed, now retry); the coroutine-yield behaviour
 * is an implementation detail.
 */
static void interruptible_sleep_ms(XrCluster *c, int ms) {
    (void) xr_cluster_sleep_interruptible(c, ms);
}

int xr_cluster_reconnect(XrCluster *c, const char *host, uint16_t port, int base_ms, int max_ms,
                         int max_attempts) {
    if (!c)
        return -1;

    if (base_ms <= 0)
        base_ms = 500;
    if (max_ms <= 0)
        max_ms = 30000;
    if (max_attempts <= 0)
        max_attempts = 10;

    /*
     * Decorrelated-jitter-ish backoff: we grow the ceiling exponentially
     * (delay_ceiling *= 2, capped at max_ms) but actually sleep for a
     * jittered value drawn inside that ceiling. This is the pattern AWS
     * SDK recommends for reconnect loops; it balances per-attempt
     * variance with bounded total retry time.
     */
    int delay_ceiling = base_ms;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (!atomic_load(&c->running))
            return -1;

        XrClusterNode *node = xr_cluster_node_new(NULL, host, port);
        if (!node)
            return -1;

        if (xr_cluster_node_connect(c, node) == 0) {
            if (xr_cluster_is_dead(c, node->name)) {
                xr_cluster_node_free(node);
                return -1;
            }
            xr_cluster_add_node(c, node);
            xr_cluster_node_start_writer(node, c->isolate);
            xr_cluster_node_start_reader(c, node);
            if (c->on_node_added)
                c->on_node_added(node->name);
            return 0;
        }

        xr_cluster_node_free(node);

        int sleep_ms = jittered_backoff_ms(delay_ceiling);
        if (sleep_ms > max_ms)
            sleep_ms = max_ms;
        interruptible_sleep_ms(c, sleep_ms);

        delay_ceiling *= 2;
        if (delay_ceiling > max_ms)
            delay_ceiling = max_ms;
    }
    return -1;
}

void xr_cluster_gossip_to_node(XrCluster *c, XrClusterNode *target) {
    if (!c || !target || target->state != XR_NODE_CONNECTED)
        return;

    /*
     * Build the NODE_INFO payload into a growing buffer.
     *
     * Wire format (still matches xr_cluster_handle_node_info):
     *   [count u16 BE]
     *   count × {
     *     [name_len u8] [name]
     *     [host_len u8] [host]
     *     [port u16 BE]
     *   }
     *
     * Earlier revisions used a single stack buffer (uint8_t payload[4096])
     * and truncated the gossip once p got within 300 bytes of the end.
     * With a realistic 256-byte host + 64-byte node name + 4 framing
     * bytes, a 4096 buffer stops well before 100 nodes — silently
     * limiting cluster size. Switching to a grow-on-demand buffer keeps
     * the hot path allocation-free for small clusters (we size the
     * initial capacity so < 64 nodes never reallocate) and correct as
     * clusters grow.
     */
    enum {
        XR_GOSSIP_INITIAL = 4096,
        XR_NODE_ENTRY_MAX = XR_NODE_NAME_MAX + 256 + 4
    };
    size_t cap = XR_GOSSIP_INITIAL;
    uint8_t *payload = (uint8_t *) xr_malloc(cap);
    if (!payload)
        return;  // OOM is non-fatal for a gossip tick
    uint8_t *p = payload;
    uint16_t count = 0;
    p += 2;  // reserve for count

    xr_mutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (node != target && node->state == XR_NODE_CONNECTED) {
            uint8_t name_len = (uint8_t) strlen(node->name);
            uint8_t host_len = (uint8_t) strlen(node->host);
            size_t entry_size = 1u + name_len + 1u + host_len + 2u;
            size_t used = (size_t) (p - payload);
            if (used + entry_size > cap) {
                size_t new_cap = cap * 2;
                while (new_cap < used + entry_size)
                    new_cap *= 2;
                uint8_t *grown = (uint8_t *) xr_realloc(payload, new_cap);
                if (!grown) {
                    /* OOM during growth: emit what we have collected so
                     * far. gossip is eventually-consistent, so missing
                     * entries get picked up on the next tick. */
                    break;
                }
                p = grown + used;
                payload = grown;
                cap = new_cap;
            }
            *p++ = name_len;
            memcpy(p, node->name, name_len);
            p += name_len;
            *p++ = host_len;
            memcpy(p, node->host, host_len);
            p += host_len;
            p[0] = (uint8_t) (node->port >> 8);
            p[1] = (uint8_t) (node->port);
            p += 2;
            count++;
        }
        node = node->next;
    }
    xr_mutex_unlock(&c->nodes_lock);

    // Write count
    payload[0] = (uint8_t) (count >> 8);
    payload[1] = (uint8_t) (count);

    uint32_t payload_len = (uint32_t) (p - payload);
    xr_cluster_node_send_frame(target, XR_FRAME_NODE_INFO, payload, payload_len);
    xr_free(payload);
    (void) XR_NODE_ENTRY_MAX;  // reserved for future preflight growth hint
}

void xr_cluster_handle_node_info(XrCluster *c, const uint8_t *payload, uint32_t len) {
    if (!c || len < 2)
        return;

    uint16_t count = ((uint16_t) payload[0] << 8) | payload[1];
    const uint8_t *p = payload + 2;
    const uint8_t *end = payload + len;

    for (uint16_t i = 0; i < count && p < end; i++) {
        if (p >= end)
            break;
        uint8_t name_len = *p++;
        if (p + name_len > end)
            break;
        char name[XR_NODE_NAME_MAX + 1] = {0};
        memcpy(name, p, name_len);
        p += name_len;

        if (p >= end)
            break;
        uint8_t host_len = *p++;
        if (p + host_len + 2 > end)
            break;
        char host[256] = {0};
        memcpy(host, p, host_len);
        p += host_len;

        uint16_t port = ((uint16_t) p[0] << 8) | p[1];
        p += 2;

        // Skip if already connected or self or dead
        if (strcmp(name, c->self_name) == 0)
            continue;
        if (xr_cluster_find_node(c, name))
            continue;
        if (xr_cluster_is_dead(c, name))
            continue;

        // Try to connect (non-blocking attempt)
        XrClusterNode *new_node = xr_cluster_node_new(name, host, port);
        if (new_node && xr_cluster_node_connect(c, new_node) == 0) {
            xr_cluster_add_node(c, new_node);
            xr_cluster_node_start_writer(new_node, c->isolate);
            xr_cluster_node_start_reader(c, new_node);
            if (c->on_node_added)
                c->on_node_added(new_node->name);
        } else if (new_node) {
            xr_cluster_node_free(new_node);
        }
    }
}

/* ========== Tombstone Management ========== */

void xr_cluster_mark_dead(XrCluster *c, const char *name) {
    if (!c || !name)
        return;

    xr_mutex_lock(&c->dead_nodes_lock);
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
        c->tombstones[c->tombstone_count].time = xr_cluster_now_ms();
        c->tombstone_count++;
    }
    xr_mutex_unlock(&c->dead_nodes_lock);
}

bool xr_cluster_is_dead(XrCluster *c, const char *name) {
    if (!c || !name)
        return false;

    xr_mutex_lock(&c->dead_nodes_lock);
    for (int i = 0; i < c->tombstone_count; i++) {
        if (strcmp(c->tombstones[i].name, name) == 0) {
            xr_mutex_unlock(&c->dead_nodes_lock);
            return true;
        }
    }
    xr_mutex_unlock(&c->dead_nodes_lock);
    return false;
}

void xr_cluster_sweep_tombstones(XrCluster *c, int64_t max_age_ms) {
    if (!c)
        return;

    int64_t now = xr_cluster_now_ms();

    xr_mutex_lock(&c->dead_nodes_lock);
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
    xr_mutex_unlock(&c->dead_nodes_lock);
}
