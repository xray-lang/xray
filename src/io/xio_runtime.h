/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xio_runtime.h - Runtime-owned IO subsystem (DNS cache today, typed
 * handle registry / IO deadline policy in later stages).
 *
 * KEY CONCEPT:
 *   The IO runtime is the single owner of every IO state that used to
 *   live as process-wide globals (g_io / g_dns / tls_isolate). It is
 *   embedded into XrRuntime so every isolate that shares a runtime
 *   shares the same DNS cache, and different runtimes never observe
 *   each other's IO state.
 *
 *   netpoll and async pool are owned directly by XrRuntime (already
 *   true today); xio_runtime is the home for everything else IO-related.
 *
 *   All public IO entry points take an XrayIsolate* and resolve the
 *   runtime via X->vm.runtime, eliminating the thread-local fallback
 *   that hid concurrency bugs and pinned IO semantics to thread layout.
 *
 *   Today the runtime owns the DNS cache. Future stages will fold in
 *   the typed handle registry (replacing stdlib's Json fd handles) and
 *   IO-level deadline / cancel policy.
 */

#ifndef XR_IO_XIO_RUNTIME_H
#define XR_IO_XIO_RUNTIME_H

#include "../base/xdefs.h"
#include "../os/os_thread.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct XrayIsolate;
struct XrRuntime;

/* ========== DNS cache ========== */
/*
 * The DNS cache is an LRU keyed by hostname with TTL expiry and
 * round-robin address selection. Implementation lives in
 * stdlib/net/dns.c so that this header stays free of getaddrinfo
 * details and platform sockaddr definitions.
 *
 * Capacity and TTL constants are duplicated here so that the cache
 * struct can be embedded by value (no malloc on runtime startup).
 */
#define XR_IO_DNS_CACHE_SIZE 256
#define XR_IO_DNS_TTL_MS (5 * 60 * 1000)
#define XR_IO_DNS_MAX_ADDRS 8

struct XrDnsCacheEntry;  // opaque, see stdlib/net/dns.c

typedef struct XrIoDnsCache {
    struct XrDnsCacheEntry *head;
    struct XrDnsCacheEntry *tail;
    struct XrDnsCacheEntry *buckets[XR_IO_DNS_CACHE_SIZE];
    int count;
    xr_mutex_t mu;
    bool inited;
} XrIoDnsCache;

/* ========== IO runtime ========== */

typedef struct XrIoRuntime {
    XrIoDnsCache dns;
    bool inited;
} XrIoRuntime;

/* ========== Lifecycle ========== */

/* Allocate and initialize an XrIoRuntime on the heap. Returns NULL on
 * allocation failure. Owned by xr_runtime_create / xr_runtime_destroy;
 * other callers should not free the returned pointer. */
XR_FUNC XrIoRuntime *xr_io_runtime_new(void);

/* Tear down and free an XrIoRuntime. Releases the DNS cache and any
 * other IO state owned here. Safe to call with NULL. */
XR_FUNC void xr_io_runtime_free(XrIoRuntime *io);

/* ========== Accessors ========== */

/* Resolve the IO runtime owning the IO state for an isolate.
 * Returns NULL if X has no attached runtime (bootstrap / tooling). */
XR_FUNC XrIoRuntime *xr_io_runtime_from_isolate(struct XrayIsolate *X);

/* Resolve the IO runtime from an XrRuntime directly. */
XR_FUNC XrIoRuntime *xr_io_runtime_from_runtime(struct XrRuntime *rt);

#ifdef __cplusplus
}
#endif

#endif  // XR_IO_XIO_RUNTIME_H
