/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2_client.c - HTTP/2 client implementation
 *
 * KEY CONCEPT:
 *   - ALPN automatic negotiation
 *   - Connection pool reuse
 *   - HTTPS HTTP/2 request/response
 */

#include "../../src/base/xmalloc.h"
#include "../../src/os/os_time.h"
#include "../../src/os/os_thread.h"
#include "http_internal.h"
#include "../net/io.h"
#include "../net/tls.h"
#include "../../src/io/xdns.h"
#include "../../src/os/os_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_H2_POOL_MAX_HOSTS 64
#define XR_H2_CONN_IDLE_TIMEOUT 60000

typedef struct XrH2PoolEntry {
    char *host;
    int port;
    XrH2Conn *conn;
    XrTlsConn *tls_conn;
    XrTlsContext *tls_ctx;
    uint64_t last_used;
    int active_streams;
    bool in_use;
    struct XrH2PoolEntry *next;
} XrH2PoolEntry;

struct XrH2Pool {
    XrH2PoolEntry *hosts[XR_H2_POOL_MAX_HOSTS];
    xr_mutex_t lock;
};

// ALPN protocol list: h2, http/1.1
static const unsigned char ALPN_PROTOS[] = "\x02h2\x08http/1.1";
#define ALPN_PROTOS_LEN 12

/* ========== Per-Isolate Connection Pool ========== */

// Create per-isolate pool
XrH2Pool *http2_client_pool_create(void) {
    XrH2Pool *pool = (XrH2Pool *) xr_calloc(1, sizeof(XrH2Pool));
    if (!pool)
        return NULL;

    xr_mutex_init(&pool->lock);
    return pool;
}

// Free per-isolate pool
void http2_client_pool_destroy(XrH2Pool *pool) {
    if (!pool)
        return;

    xr_mutex_lock(&pool->lock);

    for (int i = 0; i < XR_H2_POOL_MAX_HOSTS; i++) {
        XrH2PoolEntry *entry = pool->hosts[i];
        while (entry) {
            XrH2PoolEntry *next = entry->next;

            if (entry->conn)
                http2_conn_free(entry->conn);
            if (entry->tls_conn) {
                xr_tls_conn_close(entry->tls_conn);
                xr_tls_conn_free(entry->tls_conn);
            }
            if (entry->tls_ctx)
                xr_tls_context_free(entry->tls_ctx);
            xr_free(entry->host);
            xr_free(entry);

            entry = next;
        }
        pool->hosts[i] = NULL;
    }

    xr_mutex_unlock(&pool->lock);
    xr_mutex_destroy(&pool->lock);
    xr_free(pool);
}

// Forward declarations
static XrH2PoolEntry *create_h2_connection(const char *host, int port);
static uint64_t get_time_ms(void);
static unsigned int hash_host(const char *host, int port);
static void h2_pool_entry_free(XrH2PoolEntry *entry);
static void h2_pool_discard_entry(XrH2Pool *pool, XrH2PoolEntry *target);

// Acquire connection from per-isolate pool
static XrH2PoolEntry *http2_client_pool_acquire(XrH2Pool *pool, const char *host, int port) {
    if (!pool || !host)
        return NULL;

    xr_mutex_lock(&pool->lock);

    unsigned int idx = hash_host(host, port);
    XrH2PoolEntry *entry = pool->hosts[idx];
    XrH2PoolEntry *prev = NULL;
    uint64_t now = get_time_ms();

    while (entry) {
        if (strcmp(entry->host, host) == 0 && entry->port == port && !entry->in_use) {
            if (entry->conn &&
                entry->active_streams <
                    (int) entry->conn->remote_settings[XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS] &&
                (now - entry->last_used) <= XR_H2_CONN_IDLE_TIMEOUT) {
                entry->in_use = true;
                entry->last_used = now;
                xr_mutex_unlock(&pool->lock);
                return entry;
            }
            // Connection invalid, remove
            if (prev)
                prev->next = entry->next;
            else
                pool->hosts[idx] = entry->next;

            if (entry->conn)
                http2_conn_free(entry->conn);
            if (entry->tls_conn) {
                xr_tls_conn_close(entry->tls_conn);
                xr_tls_conn_free(entry->tls_conn);
            }
            if (entry->tls_ctx)
                xr_tls_context_free(entry->tls_ctx);
            xr_free(entry->host);

            XrH2PoolEntry *next = entry->next;
            xr_free(entry);
            entry = next;
            continue;
        }
        prev = entry;
        entry = entry->next;
    }

    xr_mutex_unlock(&pool->lock);

    // Create new connection
    entry = create_h2_connection(host, port);
    if (!entry)
        return NULL;

    // Add to pool
    xr_mutex_lock(&pool->lock);
    entry->next = pool->hosts[idx];
    pool->hosts[idx] = entry;
    xr_mutex_unlock(&pool->lock);

    return entry;
}

// Release connection to per-isolate pool
static void http2_client_pool_release(XrH2Pool *pool, XrH2PoolEntry *entry) {
    if (!pool || !entry)
        return;

    xr_mutex_lock(&pool->lock);
    entry->in_use = false;
    entry->last_used = get_time_ms();
    xr_mutex_unlock(&pool->lock);
}

// Get current time (milliseconds)
static uint64_t get_time_ms(void) {
    return xr_time_monotonic_ms();
}

// Calculate host hash
static unsigned int hash_host(const char *host, int port) {
    unsigned int h = 0;
    while (*host) {
        h = h * 31 + (unsigned char) *host++;
    }
    h = h * 31 + port;
    return h % XR_H2_POOL_MAX_HOSTS;
}

static void h2_pool_entry_free(XrH2PoolEntry *entry) {
    if (!entry)
        return;

    if (entry->conn)
        http2_conn_free(entry->conn);
    if (entry->tls_conn) {
        xr_tls_conn_close(entry->tls_conn);
        xr_tls_conn_free(entry->tls_conn);
    }
    if (entry->tls_ctx)
        xr_tls_context_free(entry->tls_ctx);
    xr_free(entry->host);
    xr_free(entry);
}

static void h2_pool_discard_entry(XrH2Pool *pool, XrH2PoolEntry *target) {
    if (!pool || !target || !target->host)
        return;

    unsigned int idx = hash_host(target->host, target->port);

    xr_mutex_lock(&pool->lock);
    XrH2PoolEntry *entry = pool->hosts[idx];
    XrH2PoolEntry *prev = NULL;

    while (entry) {
        if (entry == target) {
            if (prev)
                prev->next = entry->next;
            else
                pool->hosts[idx] = entry->next;
            xr_mutex_unlock(&pool->lock);
            h2_pool_entry_free(entry);
            return;
        }
        prev = entry;
        entry = entry->next;
    }

    xr_mutex_unlock(&pool->lock);
}

// Create new HTTPS HTTP/2 connection
static XrH2PoolEntry *create_h2_connection(const char *host, int port) {
    XrH2PoolEntry *entry = (XrH2PoolEntry *) xr_calloc(1, sizeof(XrH2PoolEntry));
    if (!entry)
        return NULL;

    entry->host = xr_strdup(host);
    entry->port = port;
    entry->in_use = true;
    entry->last_used = get_time_ms();

    // DNS resolution (with cache, IPv4/IPv6 dual-stack). H2 client has
    // no isolate plumbing yet, so this falls back to a cacheless resolve.
    XrSockAddr resolved_addr;
    if (!xr_dns_resolve(NULL, host, &resolved_addr, XR_AF_UNSPEC)) {
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    // Create socket
    int fd = socket(resolved_addr.family, SOCK_STREAM, 0);
    if (fd < 0) {
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    // Set TCP_NODELAY
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Connect
    struct sockaddr *sa;
    socklen_t sa_len;
    if (resolved_addr.family == AF_INET) {
        resolved_addr.addr.v4.sin_port = htons(port);
        sa = (struct sockaddr *) &resolved_addr.addr.v4;
        sa_len = sizeof(struct sockaddr_in);
    } else {
        resolved_addr.addr.v6.sin6_port = htons(port);
        sa = (struct sockaddr *) &resolved_addr.addr.v6;
        sa_len = sizeof(struct sockaddr_in6);
    }

    if (connect(fd, sa, sa_len) < 0) {
        xr_closesocket(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    // TLS handshake
    entry->tls_ctx = xr_tls_context_new_client();
    if (!entry->tls_ctx) {
        xr_closesocket(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    // Set ALPN
    xr_tls_context_set_alpn(entry->tls_ctx, ALPN_PROTOS, ALPN_PROTOS_LEN);

    entry->tls_conn = xr_tls_conn_new(entry->tls_ctx, fd);
    if (!entry->tls_conn) {
        xr_tls_context_free(entry->tls_ctx);
        xr_closesocket(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    xr_tls_conn_set_hostname(entry->tls_conn, host);

    if (xr_tls_conn_handshake_client(NULL, entry->tls_conn) != XR_TLS_OK) {
        xr_tls_conn_free(entry->tls_conn);
        xr_tls_context_free(entry->tls_ctx);
        xr_closesocket(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    // Check ALPN negotiation result
    const char *alpn = xr_tls_conn_get_alpn(entry->tls_conn);
    if (!alpn || strcmp(alpn, "h2") != 0) {
        // HTTP/2 not supported, close connection
        xr_tls_conn_close(entry->tls_conn);
        xr_tls_conn_free(entry->tls_conn);
        xr_tls_context_free(entry->tls_ctx);
        xr_closesocket(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    // Create HTTP/2 connection
    entry->conn = http2_conn_new(fd, entry->tls_conn, true);
    if (!entry->conn) {
        if (entry->tls_conn) {
            xr_tls_conn_close(entry->tls_conn);
            xr_tls_conn_free(entry->tls_conn);
        }
        if (entry->tls_ctx)
            xr_tls_context_free(entry->tls_ctx);
        xr_closesocket(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    // Send connection preface and SETTINGS
    if (http2_conn_init(entry->conn) < 0) {
        http2_conn_free(entry->conn);
        if (entry->tls_conn) {
            xr_tls_conn_close(entry->tls_conn);
            xr_tls_conn_free(entry->tls_conn);
        }
        if (entry->tls_ctx)
            xr_tls_context_free(entry->tls_ctx);
        xr_closesocket(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    return entry;
}

/* ========== HTTP/2 Request ========== */

XrH2Response *http2_client_request(XrH2Pool *pool, const char *url, const XrH2Request *req) {
    if (!pool || !url)
        return NULL;

    // Parse URL
    XrHttpUrl parsed;
    if (http_url_parse(url, &parsed) < 0) {
        return NULL;
    }

    // Only support HTTPS (HTTP/2 over TLS)
    if (!parsed.is_https) {
        http_url_free(&parsed);
        return NULL;
    }

    // Get connection
    XrH2PoolEntry *entry = http2_client_pool_acquire(pool, parsed.host, parsed.port);
    if (!entry) {
        http_url_free(&parsed);
        return NULL;
    }

    // Create stream
    XrH2Stream *stream = http2_stream_new(entry->conn);
    if (!stream) {
        http2_client_pool_release(pool, entry);
        http_url_free(&parsed);
        return NULL;
    }

    entry->active_streams++;

    // Build request headers
    const char *names[32];
    size_t name_lens[32];
    const char *values[32];
    size_t value_lens[32];
    int h2_header_count = 0;

    // Pseudo headers
    names[h2_header_count] = ":method";
    name_lens[h2_header_count] = 7;
    values[h2_header_count] = req && req->method ? req->method : "GET";
    value_lens[h2_header_count] = strlen(values[h2_header_count]);
    h2_header_count++;

    names[h2_header_count] = ":path";
    name_lens[h2_header_count] = 5;
    values[h2_header_count] = req && req->path ? req->path : parsed.path;
    value_lens[h2_header_count] = strlen(values[h2_header_count]);
    h2_header_count++;

    names[h2_header_count] = ":scheme";
    name_lens[h2_header_count] = 7;
    values[h2_header_count] = "https";
    value_lens[h2_header_count] = 5;
    h2_header_count++;

    names[h2_header_count] = ":authority";
    name_lens[h2_header_count] = 10;
    values[h2_header_count] = parsed.host;
    value_lens[h2_header_count] = strlen(parsed.host);
    h2_header_count++;

    // Additional headers
    if (req && req->headers) {
        for (int i = 0; i < req->header_count && h2_header_count < 32; i++) {
            names[h2_header_count] = req->headers[i].name;
            name_lens[h2_header_count] = req->headers[i].name_len;
            values[h2_header_count] = req->headers[i].value;
            value_lens[h2_header_count] = req->headers[i].value_len;
            h2_header_count++;
        }
    }

    // Send HEADERS frame
    bool has_body = req && req->body && req->body_len > 0;
    if (http2_send_headers(entry->conn, stream, names, name_lens, values, value_lens,
                           h2_header_count, !has_body) < 0) {
        entry->active_streams--;
        h2_pool_discard_entry(pool, entry);
        http_url_free(&parsed);
        return NULL;
    }

    // Send DATA frame
    if (has_body) {
        if (http2_send_data(entry->conn, stream, req->body, req->body_len, true) < 0) {
            entry->active_streams--;
            h2_pool_discard_entry(pool, entry);
            http_url_free(&parsed);
            return NULL;
        }
    }

    // Receive response
    XrH2Response *resp = (XrH2Response *) xr_calloc(1, sizeof(XrH2Response));
    if (!resp) {
        entry->active_streams--;
        http2_client_pool_release(pool, entry);
        http_url_free(&parsed);
        return NULL;
    }

    if (http2_recv_stream_data(entry->conn, stream, &resp->body, &resp->body_len) < 0) {
        xr_free(resp);
        entry->active_streams--;
        h2_pool_discard_entry(pool, entry);
        http_url_free(&parsed);
        return NULL;
    }
    resp->status = stream->status > 0 ? stream->status : 200;

    entry->active_streams--;
    http2_client_pool_release(pool, entry);
    http_url_free(&parsed);

    return resp;
}

void http2_client_response_free(XrH2Response *resp) {
    if (!resp)
        return;

    if (resp->headers) {
        for (int i = 0; i < resp->header_count; i++) {
            xr_free((void *) resp->headers[i].name);
            xr_free((void *) resp->headers[i].value);
        }
        xr_free(resp->headers);
    }
    xr_free(resp->body);
    xr_free(resp->error_msg);
    xr_free(resp);
}
