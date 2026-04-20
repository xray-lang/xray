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
 *   [magic 4B LE] [version 1B] [name_len 1B] [name ...] [port 2B LE]
 *   [cluster_hash 8B LE]
 *
 *   cluster_hash = FNV-1a 64-bit of the shared secret, used to filter
 *   announces from different clusters without revealing the secret.
 */

#include "cluster_discovery.h"
#include "cluster.h"
#include "cluster_node.h"
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
#include <poll.h>

/* ========== Announce Packet ========== */

#define ANNOUNCE_MAX_SIZE 128

static int build_announce(uint8_t *buf, size_t buf_size,
                          const char *name, uint16_t port,
                          uint64_t cluster_hash) {
    uint8_t name_len = (uint8_t)strlen(name);
    size_t total = 4 + 1 + 1 + name_len + 2 + 8;
    if (total > buf_size) return -1;

    uint8_t *p = buf;

    // magic (LE)
    uint32_t magic = XR_DISCOVERY_MAGIC;
    memcpy(p, &magic, 4); p += 4;

    // version
    *p++ = XR_DISCOVERY_VERSION;

    // name
    *p++ = name_len;
    memcpy(p, name, name_len); p += name_len;

    // port (LE)
    memcpy(p, &port, 2); p += 2;

    // cluster_hash (LE)
    memcpy(p, &cluster_hash, 8); p += 8;

    return (int)(p - buf);
}

static int parse_announce(const uint8_t *buf, size_t len,
                          char *name_out, size_t name_cap,
                          uint16_t *port_out,
                          uint64_t *hash_out) {
    if (len < 4 + 1 + 1 + 0 + 2 + 8) return -1;
    const uint8_t *p = buf;

    // magic
    uint32_t magic;
    memcpy(&magic, p, 4); p += 4;
    if (magic != XR_DISCOVERY_MAGIC) return -1;

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

    // port
    memcpy(port_out, p, 2); p += 2;

    // cluster_hash
    memcpy(hash_out, p, 8);

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

static void *discovery_thread_func(void *arg) {
    XrClusterDiscovery *disc = (XrClusterDiscovery *)arg;
    XrCluster *c = disc->cluster;

    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_addr.s_addr = inet_addr(XR_DISCOVERY_MCAST_GROUP);
    mcast_addr.sin_port = htons(disc->mcast_port);

    uint8_t announce_buf[ANNOUNCE_MAX_SIZE];
    int announce_len = build_announce(announce_buf, sizeof(announce_buf),
                                      c->self_name, c->listen_port,
                                      disc->cluster_hash);
    if (announce_len < 0) return NULL;

    struct pollfd pfd;
    pfd.fd = disc->mcast_fd;
    pfd.events = POLLIN;

    while (atomic_load(&c->running)) {
        // Send announce
        sendto(disc->mcast_fd, announce_buf, (size_t)announce_len, 0,
               (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));

        // Poll for incoming announces (up to interval_ms)
        int remaining_ms = disc->interval_ms;
        while (remaining_ms > 0 && atomic_load(&c->running)) {
            int poll_ms = remaining_ms > 500 ? 500 : remaining_ms;
            int ret = poll(&pfd, 1, poll_ms);
            remaining_ms -= poll_ms;

            if (ret <= 0) continue;
            if (!(pfd.revents & POLLIN)) continue;

            // Read incoming announce
            uint8_t recv_buf[ANNOUNCE_MAX_SIZE];
            struct sockaddr_in sender;
            socklen_t sender_len = sizeof(sender);
            ssize_t n = recvfrom(disc->mcast_fd, recv_buf, sizeof(recv_buf), 0,
                                 (struct sockaddr *)&sender, &sender_len);
            if (n <= 0) continue;

            char peer_name[XR_NODE_NAME_MAX + 1];
            uint16_t peer_port;
            uint64_t peer_hash;

            if (parse_announce(recv_buf, (size_t)n,
                               peer_name, sizeof(peer_name),
                               &peer_port, &peer_hash) != 0) {
                continue;
            }

            // Filter: must be same cluster (same secret hash)
            if (peer_hash != disc->cluster_hash) continue;

            // Check if we should connect
            if (!should_connect(c, peer_name)) continue;

            // Resolve sender IP to string for join
            char host[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sender.sin_addr, host, sizeof(host));

            // Auto-join (TCP connect + handshake)
            xr_cluster_join(c, host, peer_port);
        }
    }

    return NULL;
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

    // Spawn discovery thread
    if (pthread_create(&disc->thread, NULL, discovery_thread_func, disc) != 0) {
        close(disc->mcast_fd);
        c->discovery = NULL;
        xr_free(disc);
        return -1;
    }
    disc->thread_started = true;

    return 0;
}

void xr_cluster_discovery_stop(XrCluster *c) {
    if (!c || !c->discovery) return;

    XrClusterDiscovery *disc = c->discovery;

    // Thread exits when c->running becomes false (set by xr_cluster_stop)
    if (disc->thread_started) {
        pthread_join(disc->thread, NULL);
        disc->thread_started = false;
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
