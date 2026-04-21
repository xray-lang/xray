/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_discovery.h - LAN node auto-discovery via UDP multicast
 *
 * KEY CONCEPT:
 *   Nodes periodically broadcast their presence on a multicast group.
 *   When a new announce is received, the cluster automatically joins
 *   the remote node via TCP. The multicast layer is only for discovery;
 *   all data transfer and authentication happen over TCP.
 *
 * WHY THIS DESIGN:
 *   - Zero-config: nodes find each other without manual addresses
 *   - Simple: plain UDP multicast, no mDNS/DNS-SD complexity
 *   - Secure: multicast only carries name+port+cluster_hash,
 *     real authentication is the SHA-256 challenge-response handshake
 */

#ifndef XR_CLUSTER_DISCOVERY_H
#define XR_CLUSTER_DISCOVERY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../../src/base/xdefs.h"

// Multicast defaults
#define XR_DISCOVERY_MCAST_GROUP   "239.42.42.42"
#define XR_DISCOVERY_MCAST_PORT    47200
#define XR_DISCOVERY_INTERVAL_MS   3000 // announce every 3 seconds
#define XR_DISCOVERY_MAGIC         0x58524459 // "XRDY"
#define XR_DISCOVERY_VERSION       2  // v2: all fields big-endian

// Forward declarations
struct XrCluster;

// Discovery state
typedef struct XrClusterDiscovery {
    int               mcast_fd;     // multicast UDP socket (non-blocking)
    uint16_t          mcast_port;   // multicast port (default 47200)
    int               interval_ms;  // announce interval
    struct XrCluster *cluster;
    uint64_t          cluster_hash; // hash of secret for filtering
    /*
     * Discovery runs as a native coroutine on the main worker pool.
     * coro_spawned is set in xr_cluster_discovery_start after
     * xr_coro_spawn succeeds; coro_exited is flipped by the coro
     * itself as its last statement so xr_cluster_discovery_stop can
     * spin-wait for clean teardown before closing mcast_fd (whose
     * PollDesc the coro may still hold via netpoll).
     */
    bool              coro_spawned;
    _Atomic(bool)     coro_exited;
} XrClusterDiscovery;

/*
 * Start LAN discovery for the given cluster.
 * Spawns a background thread that:
 *   1. Periodically sends announce datagrams to multicast group
 *   2. Receives announces from other nodes and auto-joins them
 * Returns 0 on success, -1 on error.
 */
XR_FUNC int xr_cluster_discovery_start(struct XrCluster *c);

/*
 * Stop discovery and close multicast socket.
 */
XR_FUNC void xr_cluster_discovery_stop(struct XrCluster *c);

#endif
