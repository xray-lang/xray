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
 *   Background thread sends periodic announce datagrams to a multicast
 *   group and listens for announces from other nodes. On receiving a
 *   new node announce, it triggers xr_cluster_join() to establish the
 *   TCP connection with full challenge-response authentication.
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

#include "cluster_discovery.h"
#include "cluster.h"
#include "cluster_node.h"
#include "../net/io.h"
#include "../../src/base/xhash.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/*
 * Forward-declare the two xsocket entry points we need. We cannot
 * include src/coro/xsocket.h here because its include chain pulls in
 * include/xray_platform.h whose `static inline void xr_random_bytes`
 * clashes with stdlib/crypto/crypto.h's non-static declaration (same
 * collision that forced the workaround in cluster.c and
 * cluster_node.c). Link-time checks enforce signature agreement
 * against xsocket.c's canonical definitions.
 */
extern int xr_socket_wait_readable(struct XrayIsolate *X, int fd, int timeout_ms);

// xr_coro_create_native is not declared in any public header
extern struct XrCoroutine *xr_coro_create_native(struct XrayIsolate *X, void (*func)(void*), void *arg,
                                                  const char *name);
extern void xr_coro_spawn(struct XrayIsolate *X, struct XrCoroutine *coro);

/* ========== Announce Packet ========== */

#define ANNOUNCE_MAX_SIZE 128

static int build_announce(uint8_t *buf, size_t buf_size,
                          const char *name, uint16_t port,
                          uint64_t cluster_hash) {
    uint8_t name_len = (uint8_t)strlen(name);
    size_t total = 4 + 1 + 1 + name_len + 2 + 8;
    if (total > buf_size) return -1;

    uint8_t *p = buf;

    // magic (BE)
    uint32_t magic = htonl(XR_DISCOVERY_MAGIC);
    memcpy(p, &magic, 4); p += 4;

    // version
    *p++ = XR_DISCOVERY_VERSION;

    // name
    *p++ = name_len;
    memcpy(p, name, name_len); p += name_len;

    // port (BE)
    uint16_t port_be = htons(port);
    memcpy(p, &port_be, 2); p += 2;

    // cluster_hash (BE)
    uint64_t hash_be;
    for (int i = 0; i < 8; i++)
        ((uint8_t *)&hash_be)[i] = (uint8_t)(cluster_hash >> (56 - i * 8));
    memcpy(p, &hash_be, 8); p += 8;

    return (int)(p - buf);
}

static int parse_announce(const uint8_t *buf, size_t len,
                          char *name_out, size_t name_cap,
                          uint16_t *port_out,
                          uint64_t *hash_out) {
    if (len < 4 + 1 + 1 + 0 + 2 + 8) return -1;
    const uint8_t *p = buf;

    // magic (BE)
    uint32_t magic_be;
    memcpy(&magic_be, p, 4); p += 4;
    if (ntohl(magic_be) != XR_DISCOVERY_MAGIC) return -1;

    // version
    uint8_t ver = *p++;
    if (ver != XR_DISCOVERY_VERSION) return -1;

    // name
    uint8_t name_len = *p++;
    if (name_len == 0 || (size_t)(4 + 1 + 1 + name_len + 2 + 8) > len) return -1;
    if (name_len >= name_cap) return -1;
    memcpy(name_out, p, name_len);
    name_out[name_len] = '\0';
    p += name_len;

    // port (BE)
    uint16_t port_be;
    memcpy(&port_be, p, 2); p += 2;
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
    if (fd < 0) return -1;

    // Allow multiple listeners on same port
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    // Bind to multicast port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    // Join multicast group
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(XR_DISCOVERY_MCAST_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        close(fd);
        return -1;
    }

    // Set TTL=1 (link-local only)
    unsigned char ttl = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Disable loopback (don't receive own announces)
    unsigned char loop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    // Non-blocking for poll
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

/* ========== Discovery Thread ========== */

/*
 * Check if a node with the given name is already connected or is self.
 */
static bool should_connect(XrCluster *c, const char *name) {
    // Don't connect to self
    if (strcmp(c->self_name, name) == 0) return false;

    // Don't connect if node is in tombstone
    if (xr_cluster_is_dead(c, name)) return false;

    // Don't connect if already connected
    xr_mutex_lock(&c->nodes_lock);
    XrClusterNode *node = c->nodes;
    while (node) {
        if (strcmp(node->name, name) == 0) {
            xr_mutex_unlock(&c->nodes_lock);
            return false;
        }
        node = node->next;
    }
    xr_mutex_unlock(&c->nodes_lock);

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
 * statement so xr_cluster_discovery_stop can spin-wait for clean
 * teardown before closing mcast_fd (whose PollDesc the coro still
 * holds via netpoll until exit).
 */
static void discovery_coro(void *arg) {
    XrClusterDiscovery *disc = (XrClusterDiscovery *)arg;
    if (!disc) return;
    XrCluster *c = disc->cluster;

    xr_io_set_isolate(c->isolate);

    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_addr.s_addr = inet_addr(XR_DISCOVERY_MCAST_GROUP);
    mcast_addr.sin_port = htons(disc->mcast_port);

    uint8_t announce_buf[ANNOUNCE_MAX_SIZE];
    int announce_len = build_announce(announce_buf, sizeof(announce_buf),
                                      c->self_name, c->listen_port,
                                      disc->cluster_hash);
    if (announce_len < 0) {
        atomic_store(&disc->coro_exited, true);
        return;
    }

    while (atomic_load(&c->running)) {
        // Send announce (best-effort; kernel-enqueue failures ignored).
        (void)sendto(disc->mcast_fd, announce_buf, (size_t)announce_len, 0,
                     (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));

        /*
         * Wait for announces up to interval_ms, yielding via netpoll.
         * On POLLIN we drain every queued datagram with recvfrom
         * (EAGAIN means the socket is dry; we go back to waiting for
         * the remainder of the interval).
         *
         * Each individual wait is capped at SLICE_MS so the coro
         * observes c->running=false (set by xr_cluster_stop) within a
         * bounded latency. Without this cap, a ~3 s interval would
         * delay clean cluster shutdown by up to a full interval.
         */
        const int SLICE_MS = 500;
        int remaining_ms = disc->interval_ms;
        while (remaining_ms > 0 && atomic_load(&c->running)) {
            int wait_ms = remaining_ms < SLICE_MS ? remaining_ms : SLICE_MS;

            int64_t t0_ns = 0;
            {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                t0_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
            }

            int r = xr_socket_wait_readable(c->isolate, disc->mcast_fd,
                                            wait_ms);

            // Deduct the elapsed portion of the deadline so partial
            // wakes (early POLLIN) do not inflate total wait time.
            {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                int64_t t1_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
                int elapsed_ms = (int)((t1_ns - t0_ns) / 1000000LL);
                if (elapsed_ms < 0) elapsed_ms = 0;
                remaining_ms -= elapsed_ms;
            }

            if (r < 0) {
                // Error — most likely fd closed during stop. Break the
                // inner loop; the outer running check will bail out.
                break;
            }
            if (r == 0) {
                // Slice deadline fired with no data. Fall through to
                // the top of the inner while to check running flag
                // and remaining budget — do NOT break, we may still
                // have interval_ms left to spend.
                continue;
            }

            // POLLIN: drain every queued datagram non-blockingly.
            for (;;) {
                uint8_t recv_buf[ANNOUNCE_MAX_SIZE];
                struct sockaddr_in sender;
                socklen_t sender_len = sizeof(sender);
                ssize_t n = recvfrom(disc->mcast_fd, recv_buf,
                                     sizeof(recv_buf), 0,
                                     (struct sockaddr *)&sender, &sender_len);
                if (n <= 0) break;

                char peer_name[XR_NODE_NAME_MAX + 1];
                uint16_t peer_port;
                uint64_t peer_hash;
                if (parse_announce(recv_buf, (size_t)n,
                                   peer_name, sizeof(peer_name),
                                   &peer_port, &peer_hash) != 0) {
                    continue;
                }

                // Filter: must be same cluster (same secret hash).
                if (peer_hash != disc->cluster_hash) continue;

                if (!should_connect(c, peer_name)) continue;

                // Resolve sender IP → dotted-quad for xr_cluster_join.
                char host[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sender.sin_addr, host, sizeof(host));

                // Auto-join (TCP connect + handshake).
                xr_cluster_join(c, host, peer_port);
            }
        }
    }

    atomic_store(&disc->coro_exited, true);
}

/* ========== Public API ========== */

int xr_cluster_discovery_start(XrCluster *c) {
    if (!c || !atomic_load(&c->running)) return -1;
    if (c->discovery) return -1; // already started

    XrClusterDiscovery *disc = (XrClusterDiscovery *)xr_calloc(1, sizeof(*disc));
    if (!disc) return -1;

    disc->cluster = c;
    disc->mcast_port = XR_DISCOVERY_MCAST_PORT;
    disc->interval_ms = XR_DISCOVERY_INTERVAL_MS;

    // Compute cluster hash from secret
    size_t slen = strlen(c->secret);
    disc->cluster_hash = (slen > 0)
        ? xr_hash_bytes64(c->secret, slen)
        : 0;

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
     * on poll(), one isolate stop_pipe to wake every background task.
     */
    XrCoroutine *coro = xr_coro_create_native(c->isolate, discovery_coro,
                                              disc, "cluster_discovery");
    if (!coro) {
        close(disc->mcast_fd);
        c->discovery = NULL;
        xr_free(disc);
        return -1;
    }
    xr_coro_spawn(c->isolate, coro);
    disc->coro_spawned = true;

    return 0;
}

void xr_cluster_discovery_stop(XrCluster *c) {
    if (!c || !c->discovery) return;

    XrClusterDiscovery *disc = c->discovery;

    /*
     * Spin-wait (bounded 1s) for the discovery coroutine to flip
     * coro_exited before closing mcast_fd. Closing the fd while the
     * coro is yielded inside xr_socket_wait_readable would dangle a
     * PollDesc entry in netpoll — the coro's blocked state references
     * mcast_fd's pd. 100 × 10ms = 1s is ample headroom for the
     * interval tick (3s) to observe c->running=false via the
     * per-inner-loop check.
     */
    if (disc->coro_spawned) {
        for (int i = 0; i < 100 && !atomic_load(&disc->coro_exited); i++) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        disc->coro_spawned = false;
    }

    if (disc->mcast_fd >= 0) {
        // Leave multicast group
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(XR_DISCOVERY_MCAST_GROUP);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(disc->mcast_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                   &mreq, sizeof(mreq));
        close(disc->mcast_fd);
        disc->mcast_fd = -1;
    }

    c->discovery = NULL;
    xr_free(disc);
}
