/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdns.c - Per-runtime DNS resolver with LRU cache
 *
 * KEY CONCEPT:
 *   The cache is owned by XrIoRuntime. Cache lookup is O(1) via a
 *   hash bucket chain; eviction is LRU. Each entry holds up to
 *   XR_DNS_MAX_ADDRS addresses with a round-robin index so successive
 *   resolves of the same hostname spread across the resolved IP set.
 *
 *   Higher-level failover (try every address until one connects)
 *   lives in stdlib/net/conn_pool.c — this resolver only deals with
 *   name -> address(es).
 */

#include "xdns.h"
#include "xio_runtime.h"
#include "../coro/xworker.h"
#include "../coro/xasync.h"
#include "../base/xmalloc.h"
#include "../base/xhash.h"
#include "../base/xchecks.h"
#include "../os/os_time.h"
#include "../os/os_thread.h"
#include "../runtime/xisolate_internal.h"

#include <stdlib.h>
#include <string.h>

/* ========== Cache entry layout ==========
 * Defined here (not in the header) so xio_runtime.c can stay free of
 * sockaddr details — runtime destroy reaches into cache state only
 * via xr_io_dns_cache_destroy(). */

typedef struct XrDnsCacheEntry {
    char *hostname;
    XrSockAddr addrs[XR_DNS_MAX_ADDRS];
    int addr_count;
    int current_idx;       // round-robin window into addrs[]
    uint64_t expire_time;  // monotonic ms; entry is stale once now >= expire_time
    struct XrDnsCacheEntry *hash_next;
    struct XrDnsCacheEntry *lru_next;  // toward tail (older)
    struct XrDnsCacheEntry *lru_prev;  // toward head (newer)
} XrDnsCacheEntry;

/* ========== Cache lifecycle (called by xio_runtime) ========== */

void xr_io_dns_cache_init(struct XrIoDnsCache *cache) {
    XR_DCHECK(cache != NULL, "dns_cache_init: NULL cache");
    if (cache->inited)
        return;
    memset(cache, 0, sizeof(*cache));
    xr_mutex_init(&cache->mu);
    cache->inited = true;
}

void xr_io_dns_cache_destroy(struct XrIoDnsCache *cache) {
    if (!cache || !cache->inited)
        return;

    XrDnsCacheEntry *entry = cache->head;
    while (entry) {
        XrDnsCacheEntry *next = entry->lru_next;
        xr_free(entry->hostname);
        xr_free(entry);
        entry = next;
    }

    memset(cache->buckets, 0, sizeof(cache->buckets));
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;

    xr_mutex_destroy(&cache->mu);
    cache->inited = false;
}

/* ========== Cache resolution ========== */

static struct XrIoDnsCache *cache_for(struct XrayIsolate *X) {
    if (!X)
        return NULL;
    XrRuntime *rt = (XrRuntime *) X->vm.runtime;
    if (!rt || !rt->io)
        return NULL;
    return &rt->io->dns;
}

static uint64_t now_ms(void) {
    return xr_time_monotonic_ms();
}

static unsigned int hash_hostname(const char *hostname) {
    uint32_t h = xr_hash_bytes(hostname, strlen(hostname));
    return h % XR_IO_DNS_CACHE_SIZE;
}

static XrDnsCacheEntry *cache_find_locked(struct XrIoDnsCache *cache, const char *hostname) {
    unsigned int idx = hash_hostname(hostname);
    XrDnsCacheEntry *entry = cache->buckets[idx];
    while (entry) {
        if (strcmp(entry->hostname, hostname) == 0)
            return entry;
        entry = entry->hash_next;
    }
    return NULL;
}

static void cache_move_to_front_locked(struct XrIoDnsCache *cache, XrDnsCacheEntry *entry) {
    if (entry == cache->head)
        return;
    if (entry->lru_prev)
        entry->lru_prev->lru_next = entry->lru_next;
    if (entry->lru_next)
        entry->lru_next->lru_prev = entry->lru_prev;
    if (entry == cache->tail)
        cache->tail = entry->lru_prev;

    entry->lru_prev = NULL;
    entry->lru_next = cache->head;
    if (cache->head)
        cache->head->lru_prev = entry;
    cache->head = entry;
    if (!cache->tail)
        cache->tail = entry;
}

static void cache_insert_locked(struct XrIoDnsCache *cache, const char *hostname, XrSockAddr *addrs,
                                int addr_count) {
    unsigned int idx = hash_hostname(hostname);

    XrDnsCacheEntry *entry = cache_find_locked(cache, hostname);
    if (entry) {
        entry->addr_count = addr_count < XR_DNS_MAX_ADDRS ? addr_count : XR_DNS_MAX_ADDRS;
        memcpy(entry->addrs, addrs, sizeof(XrSockAddr) * entry->addr_count);
        entry->expire_time = now_ms() + XR_IO_DNS_TTL_MS;
        cache_move_to_front_locked(cache, entry);
        return;
    }

    // Evict LRU when full. Walk the bucket chain manually since the
    // entry may not be the head of its bucket.
    if (cache->count >= XR_IO_DNS_CACHE_SIZE && cache->tail) {
        XrDnsCacheEntry *old = cache->tail;
        unsigned int old_idx = hash_hostname(old->hostname);

        XrDnsCacheEntry **pp = &cache->buckets[old_idx];
        while (*pp && *pp != old)
            pp = &(*pp)->hash_next;
        if (*pp)
            *pp = old->hash_next;

        if (old->lru_prev)
            old->lru_prev->lru_next = NULL;
        cache->tail = old->lru_prev;
        if (cache->head == old)
            cache->head = NULL;

        xr_free(old->hostname);
        xr_free(old);
        cache->count--;
    }

    entry = (XrDnsCacheEntry *) xr_malloc(sizeof(XrDnsCacheEntry));
    if (!entry)
        return;
    entry->hostname = xr_strdup(hostname);
    if (!entry->hostname) {
        xr_free(entry);
        return;
    }
    entry->addr_count = addr_count < XR_DNS_MAX_ADDRS ? addr_count : XR_DNS_MAX_ADDRS;
    memcpy(entry->addrs, addrs, sizeof(XrSockAddr) * entry->addr_count);
    entry->current_idx = 0;
    entry->expire_time = now_ms() + XR_IO_DNS_TTL_MS;

    entry->hash_next = cache->buckets[idx];
    cache->buckets[idx] = entry;

    entry->lru_prev = NULL;
    entry->lru_next = cache->head;
    if (cache->head)
        cache->head->lru_prev = entry;
    cache->head = entry;
    if (!cache->tail)
        cache->tail = entry;

    cache->count++;
}

// Round-robin lookup of a single address matching the requested family.
static bool cache_lookup(struct XrIoDnsCache *cache, const char *hostname, XrSockAddr *addr,
                         XrAddrFamily family) {
    xr_mutex_lock(&cache->mu);

    XrDnsCacheEntry *entry = cache_find_locked(cache, hostname);
    if (entry && entry->addr_count > 0 && now_ms() < entry->expire_time) {
        if (family == XR_AF_UNSPEC) {
            *addr = entry->addrs[entry->current_idx];
            entry->current_idx = (entry->current_idx + 1) % entry->addr_count;
        } else {
            sa_family_t target = (family == XR_AF_INET) ? AF_INET : AF_INET6;
            int start = entry->current_idx;
            int found = -1;
            for (int i = 0; i < entry->addr_count; i++) {
                int idx = (start + i) % entry->addr_count;
                if (entry->addrs[idx].family == target) {
                    found = idx;
                    break;
                }
            }
            if (found < 0) {
                xr_mutex_unlock(&cache->mu);
                return false;
            }
            *addr = entry->addrs[found];
            entry->current_idx = (found + 1) % entry->addr_count;
        }
        cache_move_to_front_locked(cache, entry);
        xr_mutex_unlock(&cache->mu);
        return true;
    }

    xr_mutex_unlock(&cache->mu);
    return false;
}

static int cache_lookup_all(struct XrIoDnsCache *cache, const char *hostname, XrSockAddr *addrs,
                            int max_addrs, XrAddrFamily family) {
    xr_mutex_lock(&cache->mu);

    XrDnsCacheEntry *entry = cache_find_locked(cache, hostname);
    if (entry && entry->addr_count > 0 && now_ms() < entry->expire_time) {
        int count = 0;
        for (int i = 0; i < entry->addr_count && count < max_addrs; i++) {
            if (family == XR_AF_UNSPEC) {
                addrs[count++] = entry->addrs[i];
            } else {
                sa_family_t target = (family == XR_AF_INET) ? AF_INET : AF_INET6;
                if (entry->addrs[i].family == target)
                    addrs[count++] = entry->addrs[i];
            }
        }
        if (count > 0)
            cache_move_to_front_locked(cache, entry);
        xr_mutex_unlock(&cache->mu);
        return count;
    }

    xr_mutex_unlock(&cache->mu);
    return 0;
}

/* ========== Synchronous resolution ========== */

// Resolve all addresses via getaddrinfo and write them into the cache.
// Output addresses are interleaved IPv6/IPv4 (RFC 8305 §4) so callers
// that try them in order get coarse Happy Eyeballs behaviour without
// explicit failover code.
static int do_resolve_all(struct XrIoDnsCache *cache, const char *hostname, XrSockAddr *addrs,
                          int max_addrs, XrAddrFamily family) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));

    switch (family) {
        case XR_AF_INET:
            hints.ai_family = AF_INET;
            break;
        case XR_AF_INET6:
            hints.ai_family = AF_INET6;
            break;
        default:
            hints.ai_family = AF_UNSPEC;
            break;
    }
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(hostname, NULL, &hints, &res);
    if (err != 0 || !res)
        return 0;

    XrSockAddr v6[XR_DNS_MAX_ADDRS];
    XrSockAddr v4[XR_DNS_MAX_ADDRS];
    int n6 = 0, n4 = 0;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET6 && (family == XR_AF_UNSPEC || family == XR_AF_INET6) &&
            n6 < XR_DNS_MAX_ADDRS) {
            v6[n6].family = AF_INET6;
            memcpy(&v6[n6].addr.v6, rp->ai_addr, sizeof(struct sockaddr_in6));
            n6++;
        } else if (rp->ai_family == AF_INET && (family == XR_AF_UNSPEC || family == XR_AF_INET) &&
                   n4 < XR_DNS_MAX_ADDRS) {
            v4[n4].family = AF_INET;
            memcpy(&v4[n4].addr.v4, rp->ai_addr, sizeof(struct sockaddr_in));
            n4++;
        }
    }

    int count = 0;
    int i6 = 0, i4 = 0;
    while (count < max_addrs && (i6 < n6 || i4 < n4)) {
        if (i6 < n6) {
            addrs[count++] = v6[i6++];
            if (count >= max_addrs)
                break;
        }
        if (i4 < n4)
            addrs[count++] = v4[i4++];
    }

    freeaddrinfo(res);

    if (count > 0 && cache) {
        xr_mutex_lock(&cache->mu);
        cache_insert_locked(cache, hostname, addrs, count);
        xr_mutex_unlock(&cache->mu);
    }

    return count;
}

static bool do_resolve(struct XrIoDnsCache *cache, const char *hostname, XrSockAddr *addr,
                       XrAddrFamily family) {
    XrSockAddr addrs[XR_DNS_MAX_ADDRS];
    int count = do_resolve_all(cache, hostname, addrs, XR_DNS_MAX_ADDRS, family);
    if (count > 0) {
        *addr = addrs[0];
        return true;
    }
    return false;
}

/* ========== Public API ========== */

bool xr_dns_resolve(struct XrayIsolate *X, const char *hostname, XrSockAddr *addr,
                    XrAddrFamily family) {
    if (!hostname || !hostname[0] || !addr)
        return false;

    struct XrIoDnsCache *cache = cache_for(X);
    if (!cache) {
        // Bootstrap / tooling without a runtime: resolve once, no caching.
        return do_resolve(NULL, hostname, addr, family);
    }
    if (cache_lookup(cache, hostname, addr, family))
        return true;
    return do_resolve(cache, hostname, addr, family);
}

int xr_dns_resolve_all(struct XrayIsolate *X, const char *hostname, XrSockAddr *addrs,
                       int max_addrs, XrAddrFamily family) {
    if (!hostname || !hostname[0] || !addrs || max_addrs <= 0)
        return 0;

    struct XrIoDnsCache *cache = cache_for(X);
    if (cache) {
        int count = cache_lookup_all(cache, hostname, addrs, max_addrs, family);
        if (count > 0)
            return count;
    }
    return do_resolve_all(cache, hostname, addrs, max_addrs, family);
}

/* ========== Async resolution via XrAsyncPool ========== */

typedef struct {
    char hostname[256];
    XrAddrFamily family;
    XrSockAddr addr;
    bool success;
    struct XrIoDnsCache *cache;  // forwarded so the worker thread can update LRU
} XrDnsAsyncData;

static void dns_async_invoke(void *data) {
    XrDnsAsyncData *d = (XrDnsAsyncData *) data;
    d->success = do_resolve(d->cache, d->hostname, &d->addr, d->family);
}

bool xr_dns_resolve_async(struct XrayIsolate *X, struct XrCoroutine *coro, int worker_id,
                          const char *hostname, XrSockAddr *addr, XrAddrFamily family) {
    if (!hostname || !hostname[0] || !addr)
        return false;

    struct XrIoDnsCache *cache = cache_for(X);
    XrRuntime *rt = (X && X->vm.runtime) ? (XrRuntime *) X->vm.runtime : NULL;
    XrAsyncPool *pool = rt ? rt->async_pool : NULL;

    if (cache && cache_lookup(cache, hostname, addr, family))
        return true;

    if (!pool) {
        // No async pool: fall back to a synchronous resolve. This keeps
        // bootstrap / unit tests that never start workers functional.
        return do_resolve(cache, hostname, addr, family);
    }

    XrDnsAsyncData *data = (XrDnsAsyncData *) xr_malloc(sizeof(XrDnsAsyncData));
    if (!data)
        return do_resolve(cache, hostname, addr, family);

    strncpy(data->hostname, hostname, sizeof(data->hostname) - 1);
    data->hostname[sizeof(data->hostname) - 1] = '\0';
    data->family = family;
    data->success = false;
    data->cache = cache;

    XrAsyncJob *job = xr_async_job_create(coro, worker_id, dns_async_invoke, data);
    if (!job) {
        xr_free(data);
        return do_resolve(cache, hostname, addr, family);
    }

    xr_async_submit(pool, job);
    return true;  // suspended; result populated when the worker resumes the coro
}

/* ========== Cache control ========== */

void xr_dns_cache_clear(struct XrayIsolate *X) {
    struct XrIoDnsCache *cache = cache_for(X);
    if (!cache || !cache->inited)
        return;

    xr_mutex_lock(&cache->mu);
    XrDnsCacheEntry *entry = cache->head;
    while (entry) {
        XrDnsCacheEntry *next = entry->lru_next;
        xr_free(entry->hostname);
        xr_free(entry);
        entry = next;
    }
    memset(cache->buckets, 0, sizeof(cache->buckets));
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;
    xr_mutex_unlock(&cache->mu);
}

void xr_dns_prefetch(struct XrayIsolate *X, const char *hostname) {
    if (!hostname || !hostname[0])
        return;
    XrSockAddr tmp;
    do_resolve(cache_for(X), hostname, &tmp, XR_AF_UNSPEC);
}
