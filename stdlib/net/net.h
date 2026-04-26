/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * net.h - Network standard library
 *
 * KEY CONCEPT:
 *   High-level network API providing TCP/UDP connections, TLS support,
 *   DNS resolution, and coroutine-friendly I/O operations.
 */

#ifndef XR_STDLIB_NET_H
#define XR_STDLIB_NET_H

#include "../../src/base/xdefs.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xneterror.h"

// Forward declarations
struct XrayIsolate;
struct XrModule;

/* ========== Network Address ========== */

typedef enum {
    XR_NET_IPV4,
    XR_NET_IPV6
} XrNetFamily;

typedef struct XrNetAddr {
    XrNetFamily family;
    char host[256];  // Hostname or IP string
    uint16_t port;
    // Resolved address (internal use)
    union {
        uint8_t ipv4[4];
        uint8_t ipv6[16];
    } addr;
} XrNetAddr;

/* ========== UDP ========== */

typedef struct XrUdpConn XrUdpConn;

/*
 * Create UDP socket
 *
 * local_addr: Bind address (NULL means system-assigned)
 * local_port: Bind port (0 means system-assigned)
 */
XR_FUNC XrUdpConn *xr_udp_new(const char *local_addr, int local_port);

/*
 * Send data to specified address
 */
XR_FUNC int xr_udp_send_to(XrUdpConn *conn, const void *buf, size_t len, const char *host,
                           int port);

/*
 * Receive data (coroutine-friendly)
 *
 * buf: Receive buffer
 * len: Buffer length
 * from: Output sender address (can be NULL)
 *
 * Returns: Actual bytes received
 */
XR_FUNC int xr_udp_recv_from(XrUdpConn *conn, void *buf, size_t len, XrNetAddr *from);

/*
 * Close UDP socket
 */
XR_FUNC void xr_udp_close(XrUdpConn *conn);

/* ========== DNS ========== */

// DNS status defined in http_dns.h, not repeated here

/*
 * Synchronous DNS resolution
 *
 * hostname: Hostname
 * addrs: Output address array
 * max_addrs: Array capacity
 *
 * Returns: Number of resolved addresses, 0 means failure
 */
XR_FUNC int xr_dns_lookup(const char *hostname, XrNetAddr *addrs, int max_addrs);

/*
 * Synchronous resolve single address (with cache and polling)
 */
XR_FUNC bool xr_net_resolve(const char *hostname, XrNetAddr *addr);

/*
 * Clear DNS cache
 */
XR_FUNC void xr_dns_cache_clear(void);

/*
 * Preheat DNS cache
 */
XR_FUNC void xr_dns_prefetch(const char *hostname);

/* ========== Utilities ========== */

/*
 * Parse address string "host:port"
 */
XR_FUNC int xr_net_parse_addr(const char *addr_str, char *host, size_t host_len, int *port);

/*
 * Format address "host:port"
 */
XR_FUNC int xr_net_format_addr(const XrNetAddr *addr, char *buf, size_t buf_len);

/* ========== Module Lifecycle ========== */

XR_FUNC void xr_net_init(void);
XR_FUNC void xr_net_shutdown(void);

/*
 * Load net module
 */
XR_FUNC struct XrModule *xr_load_module_net(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_NET_H
