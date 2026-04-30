/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdns.h - DNS resolver with per-runtime LRU cache
 *
 * KEY CONCEPT:
 *   DNS resolution is part of the IO runtime. The cache is owned by
 *   XrIoRuntime (embedded in XrRuntime), not a process-wide global,
 *   so different runtimes never observe each other's DNS state and
 *   cache lifetime is bound to runtime lifetime.
 *
 *   The resolver supports IPv4/IPv6 dual-stack with RFC 8305 §4
 *   destination address sorting (IPv6/IPv4 interleaved) so callers
 *   that try addresses in order get coarse-grained Happy Eyeballs
 *   behaviour without writing the failover logic themselves.
 *
 *   Cache miss falls through to a synchronous getaddrinfo() at the
 *   xr_dns_resolve entry point; xr_dns_resolve_async submits the
 *   miss to runtime->async_pool so the calling coroutine can yield
 *   instead of blocking the worker thread.
 */
#ifndef XR_IO_XDNS_H
#define XR_IO_XDNS_H

#include "../base/xdefs.h"
#include "../os/os_net.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct XrayIsolate;
struct XrCoroutine;
struct XrAsyncPool;
struct XrIoDnsCache;

/* ========== Address family preference ========== */

typedef enum {
    XR_AF_UNSPEC = 0,  // Auto: prefer IPv6, fall back to IPv4 (RFC 8305)
    XR_AF_INET = 1,    // IPv4 only
    XR_AF_INET6 = 2    // IPv6 only
} XrAddrFamily;

/* ========== Unified socket address ========== */

typedef struct XrSockAddr {
    sa_family_t family;  // AF_INET or AF_INET6
    union {
        struct sockaddr_in v4;
        struct sockaddr_in6 v6;
    } addr;
} XrSockAddr;

#define XR_DNS_MAX_ADDRS 8

/* ========== Public API ========== */

/* Synchronous resolve. Cache hit returns the next address in the
 * round-robin window. Cache miss falls through to a blocking
 * getaddrinfo() — DO NOT call from worker hot paths; use
 * xr_dns_resolve_async there instead.
 *
 * X is required: it is the only path to reach the runtime-owned cache.
 * Returns true and fills *addr on success, false otherwise. */
XR_FUNC bool xr_dns_resolve(struct XrayIsolate *X, const char *hostname, XrSockAddr *addr,
                            XrAddrFamily family);

/* Resolve every known address for a hostname (returned in IPv6/IPv4
 * interleaved order — see file header). Used by failover paths in
 * conn_pool.c and Happy Eyeballs callers. Returns the count written
 * into addrs[] (0 on resolution failure). */
XR_FUNC int xr_dns_resolve_all(struct XrayIsolate *X, const char *hostname, XrSockAddr *addrs,
                               int max_addrs, XrAddrFamily family);

/* Coroutine-friendly resolve. Cache hit returns immediately; cache
 * miss submits the resolution to runtime->async_pool and the caller
 * is expected to suspend on the resulting XrAsyncJob. Returns true
 * if either path made progress, false if no async_pool was available
 * and synchronous fallback also failed. */
XR_FUNC bool xr_dns_resolve_async(struct XrayIsolate *X, struct XrCoroutine *coro, int worker_id,
                                  const char *hostname, XrSockAddr *addr, XrAddrFamily family);

/* Discard all cached entries on this runtime. Used by tests and by
 * xr_io_runtime_destroy. */
XR_FUNC void xr_dns_cache_clear(struct XrayIsolate *X);

/* Warm the cache by performing a synchronous resolve and discarding
 * the result. */
XR_FUNC void xr_dns_prefetch(struct XrayIsolate *X, const char *hostname);

/* ========== Internal: cache lifecycle ========== */

/* Initialize an XrIoDnsCache in place (mutex + empty hash table).
 * Called from xr_io_runtime_init. */
XR_FUNC void xr_io_dns_cache_init(struct XrIoDnsCache *cache);

/* Free every entry on the cache and destroy its mutex. Called from
 * xr_io_runtime_destroy. */
XR_FUNC void xr_io_dns_cache_destroy(struct XrIoDnsCache *cache);

#ifdef __cplusplus
}
#endif

#endif  // XR_IO_XDNS_H
