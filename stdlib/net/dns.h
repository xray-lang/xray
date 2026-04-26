/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * dns.h - DNS resolver with caching
 *
 * KEY CONCEPT:
 *   Async-friendly DNS resolver with LRU cache, TTL expiration, and
 *   round-robin load balancing. Supports IPv4/IPv6 dual-stack.
 *
 * WHY THIS DESIGN:
 *   - LRU cache avoids repeated getaddrinfo() syscalls
 *   - Round-robin distributes load across multiple IPs
 *   - Async pool integration keeps coroutines non-blocking
 */

#ifndef XR_STDLIB_DNS_H
#define XR_STDLIB_DNS_H

#include "../../src/base/xdefs.h"
#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <sys/socket.h>

// Forward declarations
struct XrAsyncPool;
struct XrCoroutine;

/* ========== DNS Status ========== */

typedef enum {
    XR_DNS_PENDING,
    XR_DNS_OK,
    XR_DNS_ERROR,
    XR_DNS_TIMEOUT
} XrDnsStatus;

/* ========== Address Family ========== */

typedef enum {
    XR_AF_UNSPEC = 0,  // Auto (prefer IPv4)
    XR_AF_INET = 1,    // IPv4 only
    XR_AF_INET6 = 2    // IPv6 only
} XrAddrFamily;

/* ========== Unified Socket Address (IPv4/IPv6) ========== */

typedef struct XrSockAddr {
    sa_family_t family;  // AF_INET or AF_INET6
    union {
        struct sockaddr_in v4;
        struct sockaddr_in6 v6;
    } addr;
} XrSockAddr;

/* ========== DNS Result ========== */

#define XR_DNS_MAX_ADDRS 8  // Max cached IPs per hostname

typedef struct {
    XrSockAddr addrs[XR_DNS_MAX_ADDRS];  // Mixed IPv4/IPv6 addresses
    int addr_count;
    int current_idx;  // Round-robin index
    XrDnsStatus status;
    uint64_t expire_time;  // Cache expiration (ms)
} XrDnsResult;

/* ========== API ========== */

XR_FUNC void xr_dns_init(void);
XR_FUNC void xr_dns_shutdown(void);

// Synchronous DNS resolve (cached + round-robin)
XR_FUNC bool xr_dns_resolve(const char *hostname, XrSockAddr *addr, XrAddrFamily family);

// Get all IPs for hostname (for failover scenarios).
// Addresses are returned with IPv6/IPv4 interleaved (RFC 8305 §4
// destination address sorting) so callers that try them in order get
// coarse-grained Happy Eyeballs behaviour for free.
XR_FUNC int xr_dns_resolve_all(const char *hostname, XrSockAddr *addrs, int max_addrs,
                               XrAddrFamily family);

// Async DNS resolve (coroutine-friendly)
// Returns true if cache hit or task submitted
XR_FUNC bool xr_dns_resolve_on_async(struct XrAsyncPool *pool, struct XrCoroutine *coro,
                                     int worker_id, const char *hostname, XrSockAddr *addr,
                                     XrAddrFamily family);

XR_FUNC void xr_dns_cache_clear(void);
XR_FUNC void xr_dns_prefetch(const char *hostname);

#endif  // XR_STDLIB_DNS_H
