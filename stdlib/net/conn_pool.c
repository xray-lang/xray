/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * conn_pool.c - HTTP connection pool implementation
 *
 * KEY CONCEPT:
 *   Hash-based connection pool with LRU-style idle connection management.
 *   Thread-safe via global mutex.
 */

#include "conn_pool.h"
#include "dns.h"
#include "../../src/base/xhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

// Monotonic milliseconds (no syscall on most platforms via vDSO)
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ========== Helper Functions ========== */

// Generate host key: "host:port:https"
static void make_host_key(char *key, size_t key_len, 
                          const char *host, uint16_t port, bool is_https) {
    snprintf(key, key_len, "%s:%d:%d", host, port, is_https ? 1 : 0);
}

static uint32_t hash_string(const char *str) {
    return xr_hash_bytes(str, strlen(str));
}

// Create new connection with DNS resolution and optional TLS
static XrPooledConn* create_connection(XrConnPool *pool, const char *host, uint16_t port, bool is_https) {
    (void)pool;
    // DNS resolve (cached, dual-stack)
    XrSockAddr resolved_addr;
    if (!xr_dns_resolve(host, &resolved_addr, XR_AF_UNSPEC)) return NULL;
    
    // Create socket
    int fd = socket(resolved_addr.family, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    
    // TCP_NODELAY for low latency
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    // Connect
    struct sockaddr *sa;
    socklen_t sa_len;
    if (resolved_addr.family == AF_INET) {
        resolved_addr.addr.v4.sin_port = htons(port);
        sa = (struct sockaddr*)&resolved_addr.addr.v4;
        sa_len = sizeof(struct sockaddr_in);
    } else {
        resolved_addr.addr.v6.sin6_port = htons(port);
        sa = (struct sockaddr*)&resolved_addr.addr.v6;
        sa_len = sizeof(struct sockaddr_in6);
    }
    
    if (connect(fd, sa, sa_len) < 0) {
        close(fd);
        return NULL;
    }
    
    // Create pooled connection
    XrPooledConn *conn = (XrPooledConn*)calloc(1, sizeof(XrPooledConn));
    if (!conn) {
        close(fd);
        return NULL;
    }
    
    conn->fd = fd;
    conn->state = XR_CONN_IN_USE;
    conn->created_ms = now_ms();
    conn->last_used_ms = conn->created_ms;
    conn->is_https = is_https;
    conn->tls_conn = NULL;
    
#ifdef XR_ENABLE_TLS
    // HTTPS: TLS handshake
    if (is_https && pool) {
        if (!pool->tls_ctx) {
            pool->tls_ctx = xr_tls_context_new_client();
        }
        
        if (pool->tls_ctx) {
            conn->tls_conn = xr_tls_conn_new(pool->tls_ctx, fd);
            if (conn->tls_conn) {
                xr_tls_conn_set_hostname(conn->tls_conn, host);
                if (xr_tls_conn_handshake_client(conn->tls_conn) != XR_TLS_OK) {
                    xr_tls_conn_free(conn->tls_conn);
                    conn->tls_conn = NULL;
                    close(fd);
                    free(conn);
                    return NULL;
                }
            }
        }
    }
#endif
    
    return conn;
}

// Close connection and cleanup TLS
static void close_connection(XrPooledConn *conn) {
    if (!conn) return;
    
#ifdef XR_ENABLE_TLS
    if (conn->tls_conn) {
        xr_tls_conn_close(conn->tls_conn);
        xr_tls_conn_free(conn->tls_conn);
        conn->tls_conn = NULL;
    }
#endif
    
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    
    conn->state = XR_CONN_CLOSED;
}

/* ========== Connection Pool API ========== */

XrConnPool* xr_conn_pool_new(void) {
    XrConnPool *pool = (XrConnPool*)calloc(1, sizeof(XrConnPool));
    if (!pool) return NULL;
    
    pthread_mutex_init(&pool->lock, NULL);
    pool->initialized = true;
    pool->idle_timeout_ms = (uint64_t)XR_POOL_MAX_IDLE_TIME * 1000;
    return pool;
}

void xr_conn_pool_init(XrConnPool *pool) {
    if (!pool || pool->initialized) return;
    
    memset(pool, 0, sizeof(XrConnPool));
    pthread_mutex_init(&pool->lock, NULL);
    pool->initialized = true;
    pool->idle_timeout_ms = (uint64_t)XR_POOL_MAX_IDLE_TIME * 1000;
}

void xr_conn_pool_destroy(XrConnPool *pool) {
    if (!pool || !pool->initialized) return;
    
    pthread_mutex_lock(&pool->lock);
    
    // Close all connections
    for (int i = 0; i < XR_POOL_MAX_HOSTS; i++) {
        XrHostPool *hp = pool->buckets[i];
        while (hp) {
            XrHostPool *next_hp = hp->next;
            
            XrPooledConn *conn = hp->conns;
            while (conn) {
                XrPooledConn *next_conn = conn->next;
                close_connection(conn);
                free(conn);
                conn = next_conn;
            }
            
            free(hp);
            hp = next_hp;
        }
        pool->buckets[i] = NULL;
    }
    
#ifdef XR_ENABLE_TLS
    if (pool->tls_ctx) {
        xr_tls_context_free(pool->tls_ctx);
        pool->tls_ctx = NULL;
    }
#endif
    
    pool->total_conns = 0;
    pool->initialized = false;
    
    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
}

XrPooledConn* xr_conn_pool_get(XrConnPool *pool, 
                                const char *host, 
                                uint16_t port, 
                                bool is_https) {
    if (!pool || !pool->initialized || !host) return NULL;
    
    char key[XR_POOL_HOST_KEY_LEN];
    make_host_key(key, sizeof(key), host, port, is_https);
    uint32_t bucket = hash_string(key) % XR_POOL_MAX_HOSTS;
    
    pthread_mutex_lock(&pool->lock);
    
    // Find host pool
    XrHostPool *hp = pool->buckets[bucket];
    while (hp && strcmp(hp->key, key) != 0) {
        hp = hp->next;
    }
    
    XrPooledConn *result = NULL;
    
    if (hp) {
        /* Get first idle connection (no liveness check — lazy detection).
         * Skipping recv(MSG_PEEK) saves ~1μs per request. If the peer
         * has closed, the caller will get an error on first read/write
         * and can retry with a fresh connection. */
        XrPooledConn **pp = &hp->conns;
        uint64_t now = now_ms();
        while (*pp) {
            XrPooledConn *conn = *pp;
            if (conn->state == XR_CONN_IDLE) {
                // Skip obviously expired (monotonic, no syscall)
                if (now - conn->last_used_ms > pool->idle_timeout_ms) {
                    *pp = conn->next;
                    close_connection(conn);
                    free(conn);
                    hp->conn_count--;
                    hp->idle_count--;
                    pool->total_conns--;
                    continue;
                }
                conn->state = XR_CONN_IN_USE;
                conn->last_used_ms = now;
                hp->idle_count--;
                result = conn;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    
    pthread_mutex_unlock(&pool->lock);
    
    // No idle connection, create new one
    if (!result) {
        result = create_connection(pool, host, port, is_https);
    }
    
    return result;
}

void xr_conn_pool_put(XrConnPool *pool, 
                       XrPooledConn *conn,
                       const char *host,
                       uint16_t port,
                       bool is_https,
                       bool keep_alive) {
    if (!pool || !pool->initialized || !conn) return;
    
    // Not keeping alive, close directly
    if (!keep_alive) {
        close_connection(conn);
        free(conn);
        return;
    }
    
    char key[XR_POOL_HOST_KEY_LEN];
    make_host_key(key, sizeof(key), host, port, is_https);
    uint32_t bucket = hash_string(key) % XR_POOL_MAX_HOSTS;
    
    pthread_mutex_lock(&pool->lock);
    
    // Find or create host pool
    XrHostPool *hp = pool->buckets[bucket];
    while (hp && strcmp(hp->key, key) != 0) {
        hp = hp->next;
    }
    
    if (!hp) {
        // Create new host pool
        hp = (XrHostPool*)calloc(1, sizeof(XrHostPool));
        if (!hp) {
            pthread_mutex_unlock(&pool->lock);
            close_connection(conn);
            free(conn);
            return;
        }
        
        strncpy(hp->key, key, sizeof(hp->key) - 1);
        strncpy(hp->host, host, sizeof(hp->host) - 1);
        hp->port = port;
        hp->is_https = is_https;
        
        hp->next = pool->buckets[bucket];
        pool->buckets[bucket] = hp;
    }
    
    // Check connection limit
    if (hp->idle_count >= XR_POOL_MAX_CONNS_PER_HOST) {
        pthread_mutex_unlock(&pool->lock);
        close_connection(conn);
        free(conn);
        return;
    }
    
    // Return connection to pool
    conn->state = XR_CONN_IDLE;
    conn->last_used_ms = now_ms();
    conn->next = hp->conns;
    hp->conns = conn;
    hp->conn_count++;
    hp->idle_count++;
    pool->total_conns++;
    
    pthread_mutex_unlock(&pool->lock);
}

void xr_conn_pool_close(XrConnPool *pool, XrPooledConn *conn) {
    (void)pool;
    if (!conn) return;
    close_connection(conn);
    free(conn);
}

int xr_conn_pool_evict_idle(XrConnPool *pool) {
    if (!pool || !pool->initialized) return 0;
    
    uint64_t now = now_ms();
    int evicted = 0;
    
    pthread_mutex_lock(&pool->lock);
    
    for (int i = 0; i < XR_POOL_MAX_HOSTS; i++) {
        XrHostPool *hp = pool->buckets[i];
        while (hp) {
            XrPooledConn **pp = &hp->conns;
            while (*pp) {
                XrPooledConn *conn = *pp;
                if (conn->state == XR_CONN_IDLE &&
                    now - conn->last_used_ms > pool->idle_timeout_ms) {
                    *pp = conn->next;
                    close_connection(conn);
                    free(conn);
                    hp->conn_count--;
                    hp->idle_count--;
                    pool->total_conns--;
                    evicted++;
                } else {
                    pp = &(*pp)->next;
                }
            }
            hp = hp->next;
        }
    }
    
    pthread_mutex_unlock(&pool->lock);
    return evicted;
}

void xr_conn_pool_cleanup(XrConnPool *pool) {
    xr_conn_pool_evict_idle(pool);
}

void xr_conn_pool_stats(XrConnPool *pool, int *total, int *idle) {
    if (!pool || !pool->initialized) {
        if (total) *total = 0;
        if (idle) *idle = 0;
        return;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    int t = 0, i = 0;
    for (int b = 0; b < XR_POOL_MAX_HOSTS; b++) {
        XrHostPool *hp = pool->buckets[b];
        while (hp) {
            t += hp->conn_count;
            i += hp->idle_count;
            hp = hp->next;
        }
    }
    
    if (total) *total = t;
    if (idle) *idle = i;
    
    pthread_mutex_unlock(&pool->lock);
}

/* ========== Connection Read/Write Helpers ========== */

int xr_pooled_conn_read(XrPooledConn *conn, void *buf, size_t len) {
    if (!conn || conn->fd < 0) return -1;
    
#ifdef XR_ENABLE_TLS
    if (conn->tls_conn) {
        return xr_tls_conn_read(conn->tls_conn, buf, len);
    }
#endif
    
    return (int)recv(conn->fd, buf, len, 0);
}

int xr_pooled_conn_write(XrPooledConn *conn, const void *buf, size_t len) {
    if (!conn || conn->fd < 0) return -1;
    
#ifdef XR_ENABLE_TLS
    if (conn->tls_conn) {
        return xr_tls_conn_write(conn->tls_conn, buf, len);
    }
#endif
    
    return (int)send(conn->fd, buf, len, 0);
}
