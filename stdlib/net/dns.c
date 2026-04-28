/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * dns.c - DNS resolver implementation
 *
 * KEY CONCEPT:
 *   LRU cache (256 entries) with 5-minute TTL. Round-robin across
 *   resolved IPs for load balancing. Higher-level failover (retry on
 *   remaining IPs after a connect() failure) is implemented by the
 *   connection pool in stdlib/net/conn_pool.c; this keeps DNS a pure
 *   name-resolution layer with no socket I/O of its own.
 */

#include "../../src/base/xmalloc.h"
#include "../../src/os/os_time.h"
#include "dns.h"
#include "../../src/base/xhash.h"
#include "../../src/coro/xasync.h"
#include <stdlib.h>
#include <string.h>
#include "../../src/os/os_thread.h"
#include "../../src/os/os_net.h"

/* ========== Constants ========== */

#define DNS_CACHE_SIZE 256          // Max cache entries
#define DNS_TTL_MS (5 * 60 * 1000)  // 5 minute TTL

/* ========== Cache Entry (dual-stack) ========== */

typedef struct XrDnsCacheEntry {
    char *hostname;
    XrSockAddr addrs[XR_DNS_MAX_ADDRS];  // Mixed IPv4/IPv6
    int addr_count;
    int current_idx;  // Round-robin index
    uint64_t expire_time;
    struct XrDnsCacheEntry *hash_next;  // Hash bucket chain
    struct XrDnsCacheEntry *lru_next;   // LRU: toward tail (older)
    struct XrDnsCacheEntry *lru_prev;   // LRU: toward head (newer)
} XrDnsCacheEntry;

/* ========== Async Request ========== */

struct XrDnsRequest {
    char *hostname;
    XrSockAddr addr;
    XrDnsStatus status;
    xr_mutex_t mutex;
    xr_cond_t cond;
    int refcount;
};

/* ========== Global State ========== */

static struct {
    XrDnsCacheEntry *cache_head;  // LRU head (most recent)
    XrDnsCacheEntry *cache_tail;  // LRU tail (least recent)
    XrDnsCacheEntry *cache_hash[DNS_CACHE_SIZE];
    int cache_count;
    xr_mutex_t cache_mutex;
    bool initialized;
} g_dns;

/* ========== Utility Functions ========== */

static uint64_t get_time_ms(void) {
    return xr_time_monotonic_ms();
}

static unsigned int hash_hostname(const char *hostname) {
    uint32_t h = xr_hash_bytes(hostname, strlen(hostname));
    return h % DNS_CACHE_SIZE;
}

/* ========== Cache Operations ========== */

static XrDnsCacheEntry *cache_find_locked(const char *hostname) {
    unsigned int idx = hash_hostname(hostname);
    XrDnsCacheEntry *entry = g_dns.cache_hash[idx];

    while (entry) {
        if (strcmp(entry->hostname, hostname) == 0) {
            return entry;
        }
        entry = entry->hash_next;
    }
    return NULL;
}

static void cache_move_to_front_locked(XrDnsCacheEntry *entry) {
    if (entry == g_dns.cache_head)
        return;

    // Remove from LRU list
    if (entry->lru_prev)
        entry->lru_prev->lru_next = entry->lru_next;
    if (entry->lru_next)
        entry->lru_next->lru_prev = entry->lru_prev;
    if (entry == g_dns.cache_tail)
        g_dns.cache_tail = entry->lru_prev;

    // Insert at LRU head
    entry->lru_prev = NULL;
    entry->lru_next = g_dns.cache_head;
    if (g_dns.cache_head)
        g_dns.cache_head->lru_prev = entry;
    g_dns.cache_head = entry;
    if (!g_dns.cache_tail)
        g_dns.cache_tail = entry;
}

static void cache_insert_locked(const char *hostname, XrSockAddr *addrs, int addr_count) {
    unsigned int idx = hash_hostname(hostname);

    // Check if exists
    XrDnsCacheEntry *entry = cache_find_locked(hostname);
    if (entry) {
        entry->addr_count = addr_count < XR_DNS_MAX_ADDRS ? addr_count : XR_DNS_MAX_ADDRS;
        memcpy(entry->addrs, addrs, sizeof(XrSockAddr) * entry->addr_count);
        entry->expire_time = get_time_ms() + DNS_TTL_MS;
        cache_move_to_front_locked(entry);
        return;
    }

    // Evict LRU if cache full
    if (g_dns.cache_count >= DNS_CACHE_SIZE && g_dns.cache_tail) {
        XrDnsCacheEntry *old = g_dns.cache_tail;
        unsigned int old_idx = hash_hostname(old->hostname);

        // Remove from hash chain (may be non-head node)
        XrDnsCacheEntry **pp = &g_dns.cache_hash[old_idx];
        while (*pp && *pp != old) {
            pp = &(*pp)->hash_next;
        }
        if (*pp)
            *pp = old->hash_next;

        // Remove from LRU tail
        if (old->lru_prev)
            old->lru_prev->lru_next = NULL;
        g_dns.cache_tail = old->lru_prev;
        if (g_dns.cache_head == old)
            g_dns.cache_head = NULL;

        xr_free(old->hostname);
        xr_free(old);
        g_dns.cache_count--;
    }

    // Create new entry
    entry = (XrDnsCacheEntry *) xr_malloc(sizeof(XrDnsCacheEntry));
    if (!entry)
        return;
    entry->hostname = xr_strdup(hostname);
    entry->addr_count = addr_count < XR_DNS_MAX_ADDRS ? addr_count : XR_DNS_MAX_ADDRS;
    memcpy(entry->addrs, addrs, sizeof(XrSockAddr) * entry->addr_count);
    entry->current_idx = 0;
    entry->expire_time = get_time_ms() + DNS_TTL_MS;

    // Insert into hash chain
    entry->hash_next = g_dns.cache_hash[idx];
    g_dns.cache_hash[idx] = entry;

    // Insert at LRU head
    entry->lru_prev = NULL;
    entry->lru_next = g_dns.cache_head;
    if (g_dns.cache_head)
        g_dns.cache_head->lru_prev = entry;
    g_dns.cache_head = entry;
    if (!g_dns.cache_tail)
        g_dns.cache_tail = entry;

    g_dns.cache_count++;
}

// Round-robin: get next address from cache
static bool cache_lookup(const char *hostname, XrSockAddr *addr, XrAddrFamily family) {
    xr_mutex_lock(&g_dns.cache_mutex);

    XrDnsCacheEntry *entry = cache_find_locked(hostname);
    if (entry && entry->addr_count > 0) {
        uint64_t now = get_time_ms();
        if (now < entry->expire_time) {
            // Filter by address family if specified
            if (family == XR_AF_UNSPEC) {
                *addr = entry->addrs[entry->current_idx];
                entry->current_idx = (entry->current_idx + 1) % entry->addr_count;
            } else {
                // Find matching address family
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
                    xr_mutex_unlock(&g_dns.cache_mutex);
                    return false;
                }
                *addr = entry->addrs[found];
                entry->current_idx = (found + 1) % entry->addr_count;
            }
            cache_move_to_front_locked(entry);
            xr_mutex_unlock(&g_dns.cache_mutex);
            return true;
        }
    }

    xr_mutex_unlock(&g_dns.cache_mutex);
    return false;
}

// Get all addresses from cache
static int cache_lookup_all(const char *hostname, XrSockAddr *addrs, int max_addrs,
                            XrAddrFamily family) {
    xr_mutex_lock(&g_dns.cache_mutex);

    XrDnsCacheEntry *entry = cache_find_locked(hostname);
    if (entry && entry->addr_count > 0) {
        uint64_t now = get_time_ms();
        if (now < entry->expire_time) {
            int count = 0;
            for (int i = 0; i < entry->addr_count && count < max_addrs; i++) {
                if (family == XR_AF_UNSPEC) {
                    addrs[count++] = entry->addrs[i];
                } else {
                    sa_family_t target = (family == XR_AF_INET) ? AF_INET : AF_INET6;
                    if (entry->addrs[i].family == target) {
                        addrs[count++] = entry->addrs[i];
                    }
                }
            }
            if (count > 0) {
                cache_move_to_front_locked(entry);
            }
            xr_mutex_unlock(&g_dns.cache_mutex);
            return count;
        }
    }

    xr_mutex_unlock(&g_dns.cache_mutex);
    return 0;
}

/* ========== DNS Resolution ========== */

// Resolve all addresses and cache
static int do_resolve_all(const char *hostname, XrSockAddr *addrs, int max_addrs,
                          XrAddrFamily family) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));

    // Set address family
    switch (family) {
        case XR_AF_INET:
            hints.ai_family = AF_INET;
            break;
        case XR_AF_INET6:
            hints.ai_family = AF_INET6;
            break;
        default:
            hints.ai_family = AF_UNSPEC;  // Resolve both IPv4 and IPv6
            break;
    }
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(hostname, NULL, &hints, &res);
    if (err != 0 || !res) {
        return 0;
    }

    // Collect addresses split by family first, then interleave into
    // the output in IPv6 → IPv4 alternating order (RFC 8305 §4).
    // Rationale: when both families resolve, Happy-Eyeballs-style
    // callers want to attempt IPv6 first but fall over quickly to IPv4
    // if v6 is broken. Interleaving gives them that behaviour even with
    // purely serial connect attempts.
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
        if (i4 < n4) {
            addrs[count++] = v4[i4++];
        }
    }

    freeaddrinfo(res);

    if (count > 0) {
        // Update cache
        xr_mutex_lock(&g_dns.cache_mutex);
        cache_insert_locked(hostname, addrs, count);
        xr_mutex_unlock(&g_dns.cache_mutex);
    }

    return count;
}

// Resolve single address
static bool do_resolve(const char *hostname, XrSockAddr *addr, XrAddrFamily family) {
    XrSockAddr addrs[XR_DNS_MAX_ADDRS];
    int count = do_resolve_all(hostname, addrs, XR_DNS_MAX_ADDRS, family);
    if (count > 0) {
        *addr = addrs[0];
        return true;
    }
    return false;
}

/* ========== Public API ========== */

void xr_dns_init(void) {
    if (g_dns.initialized)
        return;

    memset(&g_dns, 0, sizeof(g_dns));
    xr_mutex_init(&g_dns.cache_mutex);
    g_dns.initialized = true;
}

void xr_dns_shutdown(void) {
    if (!g_dns.initialized)
        return;

    // Clear cache
    xr_dns_cache_clear();
    xr_mutex_destroy(&g_dns.cache_mutex);
    g_dns.initialized = false;
}

bool xr_dns_resolve(const char *hostname, XrSockAddr *addr, XrAddrFamily family) {
    // Validate parameters
    if (!hostname || !hostname[0] || !addr) {
        return false;
    }

    // Check cache first
    if (cache_lookup(hostname, addr, family)) {
        return true;
    }

    // Synchronous resolution
    return do_resolve(hostname, addr, family);
}

/* ========== XrAsyncPool Integration ========== */

// Async task data
typedef struct {
    char hostname[256];
    XrAddrFamily family;
    XrSockAddr addr;
    bool success;
} XrDnsAsyncData;

// Async execution function (runs in async thread)
static void dns_async_invoke(void *data) {
    XrDnsAsyncData *d = (XrDnsAsyncData *) data;
    d->success = do_resolve(d->hostname, &d->addr, d->family);
}

/*
 * Async DNS resolve using XrAsyncPool:
 * 1. Check cache first, return immediately if hit
 * 2. Submit task to async pool on cache miss
 * 3. Coroutine suspends until async thread completes
 * 4. Coroutine resumes with result
 */
bool xr_dns_resolve_on_async(XrAsyncPool *pool, struct XrCoroutine *coro, int worker_id,
                             const char *hostname, XrSockAddr *addr, XrAddrFamily family) {
    if (!g_dns.initialized) {
        xr_dns_init();
    }

    // Check cache first
    if (cache_lookup(hostname, addr, family)) {
        return true;
    }

    // Fall back to sync resolve if no async pool
    if (!pool) {
        return do_resolve(hostname, addr, family);
    }

    // Create async task data
    XrDnsAsyncData *data = (XrDnsAsyncData *) xr_malloc(sizeof(XrDnsAsyncData));
    if (!data) {
        return do_resolve(hostname, addr, family);
    }

    strncpy(data->hostname, hostname, sizeof(data->hostname) - 1);
    data->hostname[sizeof(data->hostname) - 1] = '\0';
    data->family = family;
    data->success = false;

    // Create async job
    XrAsyncJob *job = xr_async_job_create(coro, worker_id, dns_async_invoke, data);
    if (!job) {
        xr_free(data);
        return do_resolve(hostname, addr, family);
    }

    // Submit job (coroutine will suspend)
    xr_async_submit(pool, job);

    // Note: coroutine suspended, code below runs after resume
    // Result available via coro->async_result

    return true;  // Task submitted
}

void xr_dns_cache_clear(void) {
    xr_mutex_lock(&g_dns.cache_mutex);

    XrDnsCacheEntry *entry = g_dns.cache_head;
    while (entry) {
        XrDnsCacheEntry *next = entry->lru_next;
        xr_free(entry->hostname);
        xr_free(entry);
        entry = next;
    }

    memset(g_dns.cache_hash, 0, sizeof(g_dns.cache_hash));
    g_dns.cache_head = NULL;
    g_dns.cache_tail = NULL;
    g_dns.cache_count = 0;

    xr_mutex_unlock(&g_dns.cache_mutex);
}

void xr_dns_prefetch(const char *hostname) {
    // Warm up cache with sync resolve
    XrSockAddr addr;
    do_resolve(hostname, &addr, XR_AF_UNSPEC);
}

int xr_dns_resolve_all(const char *hostname, XrSockAddr *addrs, int max_addrs,
                       XrAddrFamily family) {
    // Check cache first
    int count = cache_lookup_all(hostname, addrs, max_addrs, family);
    if (count > 0) {
        return count;
    }

    // Synchronous resolution
    return do_resolve_all(hostname, addrs, max_addrs, family);
}

// Connection-level failover (Happy-Eyeballs serial fallback) lives in
// conn_pool.c::create_connection.
