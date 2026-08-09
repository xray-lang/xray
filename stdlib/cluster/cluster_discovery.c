/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * cluster_discovery.c - LAN node auto-discovery via UDP multicast
 *
 * KEY CONCEPT:
 *   A native coroutine sends periodic announce datagrams to a multicast
 *   group and listens for announces from other nodes. On receiving a
 *   new node announce, it spawns the netpoll-driven outgoing join state
 *   machine with full challenge-response authentication.
 *
 * WIRE FORMAT (announce datagram):
 *   [magic 4B BE] [version 1B] [name_len 1B] [name ...] [port 2B BE]
 *   [cluster_hash 8B BE]
 *
 *   cluster_hash = FNV-1a 64-bit of the shared secret, used to filter
 *   announces from different clusters without revealing the secret.
 *
 * BYTE ORDER: big-endian (network byte order), matching the cluster
 * TCP protocol in cluster_proto.c. Consistency across all wire formats
 * keeps the codebase simple and avoids LE/BE mixup bugs.
 */

#include "cluster_internal.h"
#include "../../stdlib/net/io.h"
#include "../../src/base/xchecks.h"
#include "../../src/base/xhash.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/os/os_time.h"

#include "../../src/os/os_net.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Multicast defaults. These are discovery implementation details; the
// authenticated cluster protocol continues over TCP after auto-join.
#define XR_DISCOVERY_MCAST_GROUP "239.42.42.42"
#define XR_DISCOVERY_MCAST_PORT 47200
#define XR_DISCOVERY_INTERVAL_MS 3000
#define XR_DISCOVERY_MAGIC 0x58524459
#define XR_DISCOVERY_VERSION 2

struct XrClusterDiscovery {
    int mcast_fd;
    uint16_t mcast_port;
    int interval_ms;
    XrCluster *cluster;
    uint64_t cluster_hash;
    bool coro_spawned;
    _Atomic(bool) coro_exited;
    int64_t next_announce_ms;
    uint8_t announce[128];
    int announce_len;
};

/* ========== Announce Packet ========== */

#define ANNOUNCE_MAX_SIZE 128

static int build_announce(uint8_t *buf, size_t buf_size, const char *name, uint16_t port,
                          uint64_t cluster_hash) {
    uint8_t name_len = (uint8_t) strlen(name);
    size_t total = 4 + 1 + 1 + name_len + 2 + 8;
    if (total > buf_size)
        return -1;

    uint8_t *p = buf;

    // magic (BE)
    uint32_t magic = htonl(XR_DISCOVERY_MAGIC);
    memcpy(p, &magic, 4);
    p += 4;

    // version
    *p++ = XR_DISCOVERY_VERSION;

    // name
    *p++ = name_len;
    memcpy(p, name, name_len);
    p += name_len;

    // port (BE)
    uint16_t port_be = htons(port);
    memcpy(p, &port_be, 2);
    p += 2;

    // cluster_hash (BE)
    uint64_t hash_be;
    for (int i = 0; i < 8; i++)
        ((uint8_t *) &hash_be)[i] = (uint8_t) (cluster_hash >> (56 - i * 8));
    memcpy(p, &hash_be, 8);
    p += 8;

    return (int) (p - buf);
}

static int parse_announce(const uint8_t *buf, size_t len, char *name_out, size_t name_cap,
                          uint16_t *port_out, uint64_t *hash_out) {
    if (len < 4 + 1 + 1 + 0 + 2 + 8)
        return -1;
    const uint8_t *p = buf;

    // magic (BE)
    uint32_t magic_be;
    memcpy(&magic_be, p, 4);
    p += 4;
    if (ntohl(magic_be) != XR_DISCOVERY_MAGIC)
        return -1;

    // version
    uint8_t ver = *p++;
    if (ver != XR_DISCOVERY_VERSION)
        return -1;

    // name
    uint8_t name_len = *p++;
    if (name_len == 0 || (size_t) (4 + 1 + 1 + name_len + 2 + 8) > len)
        return -1;
    if (name_len >= name_cap)
        return -1;
    memcpy(name_out, p, name_len);
    name_out[name_len] = '\0';
    p += name_len;

    // port (BE)
    uint16_t port_be;
    memcpy(&port_be, p, 2);
    p += 2;
    *port_out = ntohs(port_be);

    // cluster_hash (BE)
    uint64_t h = 0;
    for (int i = 0; i < 8; i++)
        h = (h << 8) | p[i];
    *hash_out = h;

    return 0;
}

/* ========== Multicast Socket Setup ========== */

static int create_mcast_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    // Allow multiple listeners on same port
    xr_socket_set_reuseaddr(fd, true);
    xr_socket_set_reuseport(fd, true);

    // Bind to multicast port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        xr_closesocket(fd);
        return -1;
    }

    // Join multicast group
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(XR_DISCOVERY_MCAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *) &mreq, sizeof(mreq)) < 0) {
        xr_closesocket(fd);
        return -1;
    }

    // Set TTL=1 (link-local only)
    unsigned char ttl = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl));

    // Disable loopback (don't receive own announces)
    unsigned char loop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *) &loop, sizeof(loop));

    // Non-blocking for poll
    xr_socket_set_nonblocking(fd);

    return fd;
}

/* ========== Discovery Thread ========== */

/*
 * Check if a node with the given name is already connected or is self.
 */
static bool should_connect(XrCluster *c, const char *name) {
    // Don't connect to self
    if (strcmp(c->self_name, name) == 0)
        return false;

    // Don't connect if node is in tombstone
    if (cluster_health_is_dead(c, name))
        return false;

    // Don't connect if already connected
    xr_amutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (strcmp(node->name, name) == 0) {
            xr_amutex_unlock(&c->nodes_lock);
            return false;
        }
        node = node->next;
    }
    xr_amutex_unlock(&c->nodes_lock);

    return true;
}

/*
 * Native coroutine body — one tick = send an announce, then drain any
 * announces that arrive during the interval window.
 *
 * Flow of each tick:
 *   1. sendto() on the non-blocking mcast_fd (datagram; success here
 *      just means "enqueued to kernel"). Errors are ignored because
 *      LAN discovery is best-effort and a failed send just retries
 *      on the next tick.
 *   2. xr_socket_wait_readable yields the coroutine until either
 *      mcast_fd becomes POLLIN-ready or the interval_ms deadline
 *      fires. The worker thread is free to run other coroutines
 *      during the wait — unlike the original pthread which blocked in
 *      poll().
 *   3. On readable, recvfrom() drains every pending datagram in a
 *      non-blocking loop. Using xr_socket_wait_readable (which does
 *      not consume bytes) instead of xr_socket_read preserves the
 *      full UDP datagram — an xr_socket_read with a 1-byte buffer
 *      would truncate the datagram and drop 99 bytes of announce
 *      payload per POSIX recv semantics.
 *
 * Exit contract: disc->coro_exited is flipped true as the last
 * statement so cluster_discovery_stop can spin-wait for clean
 * teardown before closing mcast_fd (whose PollDesc the coro still
 * holds via netpoll until exit).
 */
static void discovery_context_destroy(void *context) {
    XrClusterDiscovery *disc = (XrClusterDiscovery *) context;
    if (!disc)
        return;
    atomic_store(&disc->coro_exited, true);
    cluster_runtime_release(disc->cluster);
}

static void discovery_drain(XrClusterDiscovery *disc) {
    XrCluster *c = disc->cluster;
    for (;;) {
        uint8_t recv_buf[ANNOUNCE_MAX_SIZE];
        struct sockaddr_in sender;
        socklen_t sender_len = sizeof(sender);
        ssize_t n = recvfrom(disc->mcast_fd, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *) &sender, &sender_len);
        if (n <= 0)
            return;

        char peer_name[XR_NODE_NAME_MAX + 1];
        uint16_t peer_port;
        uint64_t peer_hash;
        if (parse_announce(recv_buf, (size_t) n, peer_name, sizeof(peer_name), &peer_port,
                           &peer_hash) != 0 ||
            peer_hash != disc->cluster_hash || !should_connect(c, peer_name))
            continue;

        char host[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sender.sin_addr, host, sizeof(host)))
            (void) cluster_runtime_join_spawn(c, host, peer_port);
    }
}

static XrCFuncResult discovery_drive(XrVMRuntime *X, XrClusterDiscovery *disc, XrValue *result);

static XrCFuncResult discovery_continue(XrVMRuntime *X, int status, XrValue resume_value,
                                        void *context, XrValue *result) {
    (void) resume_value;
    if (status == XR_RESUME_CANCELLED || status == XR_RESUME_ERROR)
        return XR_CFUNC_DONE;
    return discovery_drive(X, (XrClusterDiscovery *) context, result);
}

static XrCFuncResult discovery_drive(XrVMRuntime *X, XrClusterDiscovery *disc, XrValue *result) {
    if (!disc || !atomic_load(&disc->cluster->running))
        return XR_CFUNC_DONE;

    discovery_drain(disc);
    int64_t now = cluster_now_ms();
    if (now >= disc->next_announce_ms) {
        struct sockaddr_in mcast_addr;
        memset(&mcast_addr, 0, sizeof(mcast_addr));
        mcast_addr.sin_family = AF_INET;
        mcast_addr.sin_addr.s_addr = inet_addr(XR_DISCOVERY_MCAST_GROUP);
        mcast_addr.sin_port = htons(disc->mcast_port);
        (void) sendto(disc->mcast_fd, disc->announce, (size_t) disc->announce_len, 0,
                      (struct sockaddr *) &mcast_addr, sizeof(mcast_addr));
        disc->next_announce_ms = now + disc->interval_ms;
    }

    int64_t timeout_ms = disc->next_announce_ms - cluster_now_ms();
    if (timeout_ms < 1)
        timeout_ms = 1;
    return xr_yield_for_io(X, disc->mcast_fd, XR_WAIT_READ, timeout_ms, discovery_continue, disc,
                           result);
}

static XrCFuncResult discovery_entry(XrVMRuntime *X, void *context, XrValue *result) {
    return discovery_drive(X, (XrClusterDiscovery *) context, result);
}

/* ========== Internal Discovery Lifecycle ========== */

int cluster_discovery_start(XrCluster *c) {
    if (!c || !atomic_load(&c->running))
        return -1;
    if (c->discovery)
        return -1;  // already started

    XrClusterDiscovery *disc = (XrClusterDiscovery *) xr_calloc(1, sizeof(*disc));
    if (!disc)
        return -1;

    disc->cluster = c;
    disc->mcast_port = XR_DISCOVERY_MCAST_PORT;
    disc->interval_ms = XR_DISCOVERY_INTERVAL_MS;

    // Compute cluster hash from secret
    size_t slen = strlen(c->secret);
    disc->cluster_hash = (slen > 0) ? xr_hash_bytes64(c->secret, slen) : 0;
    disc->announce_len = build_announce(disc->announce, sizeof(disc->announce), c->self_name,
                                        c->listen_port, disc->cluster_hash);
    if (disc->announce_len < 0) {
        xr_free(disc);
        return -1;
    }
    disc->next_announce_ms = 0;

    // Create multicast socket
    disc->mcast_fd = create_mcast_socket(disc->mcast_port);
    if (disc->mcast_fd < 0) {
        xr_free(disc);
        return -1;
    }

    c->discovery = disc;
    atomic_store(&disc->coro_exited, false);

    /*
     * Spawn discovery as a native coroutine on the worker pool rather
     * than a dedicated pthread: no private scheduling, no thread-block
     * on poll(); cancellation and fd readiness stay in the shared runtime.
     */
    cluster_runtime_retain(c);
    XrCoroutine *coro = xr_coro_create_native_yieldable(
        c->isolate, discovery_entry, disc, discovery_context_destroy, "cluster_discovery");
    if (!coro) {
        xr_closesocket(disc->mcast_fd);
        c->discovery = NULL;
        xr_free(disc);
        return -1;
    }
    xr_coro_spawn(c->isolate, coro);
    disc->coro_spawned = true;

    return 0;
}

void cluster_discovery_stop(XrCluster *c) {
    if (!c || !c->discovery)
        return;

    XrClusterDiscovery *disc = c->discovery;

    /* The discovery coroutine owns a cluster reference. This destructor is
     * reached only after that reference is released, so the PollDesc can no
     * longer be using mcast_fd and no stop-time spin wait is required. */
    XR_DCHECK(!disc->coro_spawned || atomic_load(&disc->coro_exited),
              "cluster discovery destroyed while its coroutine is live");
    disc->coro_spawned = false;

    if (disc->mcast_fd >= 0) {
        // Leave multicast group
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(XR_DISCOVERY_MCAST_GROUP);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(disc->mcast_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char *) &mreq,
                   sizeof(mreq));
        xr_closesocket(disc->mcast_fd);
        disc->mcast_fd = -1;
    }

    c->discovery = NULL;
    xr_free(disc);
}
