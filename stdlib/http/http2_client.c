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
 *   - HTTP/2 request/response
 */

#include "../../src/base/xmalloc.h"
#include "http2_client.h"
#include "http2.h"
#include "../net/io.h"
#include "../net/dns.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

// ALPN protocol list: h2, http/1.1
static const unsigned char ALPN_PROTOS[] = "\x02h2\x08http/1.1";
#define ALPN_PROTOS_LEN 12

/* ========== Per-Isolate Connection Pool ========== */

// Legacy global pool (deprecated)
static XrH2Pool g_pool = {0};

// Create per-isolate pool
XrH2Pool *xr_h2_pool_create(void) {
    XrH2Pool *pool = (XrH2Pool *) xr_calloc(1, sizeof(XrH2Pool));
    if (!pool)
        return NULL;

    pthread_mutex_init(&pool->lock, NULL);
    pool->initialized = true;
    return pool;
}

// Free per-isolate pool
void xr_h2_pool_destroy(XrH2Pool *pool) {
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);

    for (int i = 0; i < XR_H2_POOL_MAX_HOSTS; i++) {
        XrH2PoolEntry *entry = pool->hosts[i];
        while (entry) {
            XrH2PoolEntry *next = entry->next;

            if (entry->conn)
                xr_h2_conn_free(entry->conn);
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

    pool->host_count = 0;
    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
    xr_free(pool);
}

// Forward declarations
static XrH2PoolEntry *create_h2_connection(const char *host, int port, bool is_https);
static uint64_t get_time_ms(void);
static unsigned int hash_host(const char *host, int port);

// Acquire connection from per-isolate pool
XrH2PoolEntry *xr_h2_pool_acquire_from(XrH2Pool *pool, const char *host, int port, bool is_https) {
    if (!pool || !pool->initialized || !host)
        return NULL;

    pthread_mutex_lock(&pool->lock);

    unsigned int idx = hash_host(host, port);
    XrH2PoolEntry *entry = pool->hosts[idx];
    XrH2PoolEntry *prev = NULL;

    while (entry) {
        if (strcmp(entry->host, host) == 0 && entry->port == port && !entry->in_use) {
            if (entry->conn &&
                entry->active_streams <
                    (int) entry->conn->remote_settings[XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS]) {
                entry->in_use = true;
                entry->last_used = get_time_ms();
                pthread_mutex_unlock(&pool->lock);
                return entry;
            }
            // Connection invalid, remove
            if (prev)
                prev->next = entry->next;
            else
                pool->hosts[idx] = entry->next;

            if (entry->conn)
                xr_h2_conn_free(entry->conn);
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

    pthread_mutex_unlock(&pool->lock);

    // Create new connection
    entry = create_h2_connection(host, port, is_https);
    if (!entry)
        return NULL;

    // Add to pool
    pthread_mutex_lock(&pool->lock);
    entry->next = pool->hosts[idx];
    pool->hosts[idx] = entry;
    pthread_mutex_unlock(&pool->lock);

    return entry;
}

// Release connection to per-isolate pool
void xr_h2_pool_release_to(XrH2Pool *pool, XrH2PoolEntry *entry) {
    if (!pool || !entry)
        return;

    pthread_mutex_lock(&pool->lock);
    entry->in_use = false;
    entry->last_used = get_time_ms();
    pthread_mutex_unlock(&pool->lock);
}

// Get current time (milliseconds)
static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
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

void xr_h2_pool_init(void) {
    if (g_pool.initialized)
        return;

    memset(&g_pool, 0, sizeof(g_pool));
    pthread_mutex_init(&g_pool.lock, NULL);
    g_pool.initialized = true;
}

void xr_h2_pool_cleanup(void) {
    if (!g_pool.initialized)
        return;

    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < XR_H2_POOL_MAX_HOSTS; i++) {
        XrH2PoolEntry *entry = g_pool.hosts[i];
        while (entry) {
            XrH2PoolEntry *next = entry->next;

            if (entry->conn)
                xr_h2_conn_free(entry->conn);
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
        g_pool.hosts[i] = NULL;
    }

    g_pool.host_count = 0;
    pthread_mutex_unlock(&g_pool.lock);
    pthread_mutex_destroy(&g_pool.lock);
    g_pool.initialized = false;
}

// Create new HTTP/2 connection
static XrH2PoolEntry *create_h2_connection(const char *host, int port, bool is_https) {
    XrH2PoolEntry *entry = (XrH2PoolEntry *) xr_calloc(1, sizeof(XrH2PoolEntry));
    if (!entry)
        return NULL;

    entry->host = xr_strdup(host);
    entry->port = port;
    entry->in_use = true;
    entry->last_used = get_time_ms();

    // DNS resolution (with cache, IPv4/IPv6 dual-stack)
    XrSockAddr resolved_addr;
    if (!xr_dns_resolve(host, &resolved_addr, XR_AF_UNSPEC)) {
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
        close(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    if (is_https) {
        // TLS handshake
        entry->tls_ctx = xr_tls_context_new_client();
        if (!entry->tls_ctx) {
            close(fd);
            xr_free(entry->host);
            xr_free(entry);
            return NULL;
        }

        // Set ALPN
        xr_tls_context_set_alpn(entry->tls_ctx, ALPN_PROTOS, ALPN_PROTOS_LEN);

        entry->tls_conn = xr_tls_conn_new(entry->tls_ctx, fd);
        if (!entry->tls_conn) {
            xr_tls_context_free(entry->tls_ctx);
            close(fd);
            xr_free(entry->host);
            xr_free(entry);
            return NULL;
        }

        xr_tls_conn_set_hostname(entry->tls_conn, host);

        if (xr_tls_conn_handshake_client(entry->tls_conn) != XR_TLS_OK) {
            xr_tls_conn_free(entry->tls_conn);
            xr_tls_context_free(entry->tls_ctx);
            close(fd);
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
            close(fd);
            xr_free(entry->host);
            xr_free(entry);
            return NULL;
        }
    }

    // Create HTTP/2 connection
    entry->conn = xr_h2_conn_new(fd, entry->tls_conn, true);
    if (!entry->conn) {
        if (entry->tls_conn) {
            xr_tls_conn_close(entry->tls_conn);
            xr_tls_conn_free(entry->tls_conn);
        }
        if (entry->tls_ctx)
            xr_tls_context_free(entry->tls_ctx);
        close(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    // Send connection preface and SETTINGS
    if (xr_h2_conn_init(entry->conn) < 0) {
        xr_h2_conn_free(entry->conn);
        if (entry->tls_conn) {
            xr_tls_conn_close(entry->tls_conn);
            xr_tls_conn_free(entry->tls_conn);
        }
        if (entry->tls_ctx)
            xr_tls_context_free(entry->tls_ctx);
        close(fd);
        xr_free(entry->host);
        xr_free(entry);
        return NULL;
    }

    return entry;
}

XrH2PoolEntry *xr_h2_pool_acquire(const char *host, int port, bool is_https) {
    if (!host)
        return NULL;

    xr_h2_pool_init();

    pthread_mutex_lock(&g_pool.lock);

    unsigned int idx = hash_host(host, port);
    XrH2PoolEntry *entry = g_pool.hosts[idx];
    XrH2PoolEntry *prev = NULL;

    // Find available connection
    while (entry) {
        if (strcmp(entry->host, host) == 0 && entry->port == port && !entry->in_use) {
            // Check if connection is valid
            if (entry->conn &&
                entry->active_streams <
                    (int) entry->conn->remote_settings[XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS]) {
                entry->in_use = true;
                entry->last_used = get_time_ms();
                pthread_mutex_unlock(&g_pool.lock);
                return entry;
            }
            // Connection invalid, remove
            if (prev)
                prev->next = entry->next;
            else
                g_pool.hosts[idx] = entry->next;

            if (entry->conn)
                xr_h2_conn_free(entry->conn);
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

    pthread_mutex_unlock(&g_pool.lock);

    // Create new connection
    entry = create_h2_connection(host, port, is_https);
    if (!entry)
        return NULL;

    // Add to connection pool
    pthread_mutex_lock(&g_pool.lock);
    entry->next = g_pool.hosts[idx];
    g_pool.hosts[idx] = entry;
    pthread_mutex_unlock(&g_pool.lock);

    return entry;
}

void xr_h2_pool_release(XrH2PoolEntry *entry) {
    if (!entry)
        return;

    pthread_mutex_lock(&g_pool.lock);
    entry->in_use = false;
    entry->last_used = get_time_ms();
    pthread_mutex_unlock(&g_pool.lock);
}

void xr_h2_pool_cleanup_idle(void) {
    if (!g_pool.initialized)
        return;

    uint64_t now = get_time_ms();

    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < XR_H2_POOL_MAX_HOSTS; i++) {
        XrH2PoolEntry *entry = g_pool.hosts[i];
        XrH2PoolEntry *prev = NULL;

        while (entry) {
            if (!entry->in_use && (now - entry->last_used) > XR_H2_CONN_IDLE_TIMEOUT) {
                // Remove idle connection
                if (prev)
                    prev->next = entry->next;
                else
                    g_pool.hosts[i] = entry->next;

                if (entry->conn)
                    xr_h2_conn_free(entry->conn);
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
    }

    pthread_mutex_unlock(&g_pool.lock);
}

/* ========== HTTP/2 Request ========== */

XrH2Response *xr_h2_request(const char *url, const XrH2Request *req) {
    if (!url)
        return NULL;

    // Parse URL
    XrHttpUrl parsed;
    if (xr_http_url_parse(url, &parsed) < 0) {
        return NULL;
    }

    // Only support HTTPS (HTTP/2 over TLS)
    if (!parsed.is_https) {
        xr_http_url_free(&parsed);
        return NULL;
    }

    // Get connection
    XrH2PoolEntry *entry = xr_h2_pool_acquire(parsed.host, parsed.port, true);
    if (!entry) {
        xr_http_url_free(&parsed);
        return NULL;
    }

    // Create stream
    XrH2Stream *stream = xr_h2_stream_new(entry->conn);
    if (!stream) {
        xr_h2_pool_release(entry);
        xr_http_url_free(&parsed);
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
    if (xr_h2_send_headers(entry->conn, stream, names, name_lens, values, value_lens,
                           h2_header_count, !has_body) < 0) {
        entry->active_streams--;
        xr_h2_pool_release(entry);
        xr_http_url_free(&parsed);
        return NULL;
    }

    // Send DATA frame
    if (has_body) {
        if (xr_h2_send_data(entry->conn, stream, req->body, req->body_len, true) < 0) {
            entry->active_streams--;
            xr_h2_pool_release(entry);
            xr_http_url_free(&parsed);
            return NULL;
        }
    }

    // Receive response
    XrH2Response *resp = (XrH2Response *) xr_calloc(1, sizeof(XrH2Response));
    if (!resp) {
        entry->active_streams--;
        xr_h2_pool_release(entry);
        xr_http_url_free(&parsed);
        return NULL;
    }

    if (xr_h2_recv_stream_data(entry->conn, stream, &resp->body, &resp->body_len) < 0) {
        xr_free(resp);
        entry->active_streams--;
        xr_h2_pool_release(entry);
        xr_http_url_free(&parsed);
        return NULL;
    }
    resp->status = stream->status > 0 ? stream->status : 200;

    entry->active_streams--;
    xr_h2_pool_release(entry);
    xr_http_url_free(&parsed);

    return resp;
}

XrH2Response *xr_h2_get(const char *url) {
    XrH2Request req = {0};
    req.method = "GET";
    return xr_h2_request(url, &req);
}

XrH2Response *xr_h2_post(const char *url, const char *body, size_t body_len,
                         const char *content_type) {
    XrH2Request req = {0};
    req.method = "POST";
    req.body = body;
    req.body_len = body_len;

    XrHttpHeader headers[1];
    if (content_type) {
        headers[0].name = "content-type";
        headers[0].name_len = 12;
        headers[0].value = content_type;
        headers[0].value_len = strlen(content_type);
        req.headers = headers;
        req.header_count = 1;
    }

    return xr_h2_request(url, &req);
}

void xr_h2_response_free(XrH2Response *resp) {
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

bool xr_http_auto_version(const char *host, int port, bool is_https) {
    if (!is_https)
        return false;  // HTTP/2 requires TLS

    // Try ALPN negotiation
    XrTlsContext *ctx = xr_tls_context_new_client();
    if (!ctx)
        return false;

    xr_tls_context_set_alpn(ctx, ALPN_PROTOS, ALPN_PROTOS_LEN);

    // DNS resolution (with cache, IPv4/IPv6 dual-stack)
    XrSockAddr resolved_addr;
    if (!xr_dns_resolve(host, &resolved_addr, XR_AF_UNSPEC)) {
        xr_tls_context_free(ctx);
        return false;
    }

    // Create socket
    int fd = socket(resolved_addr.family, SOCK_STREAM, 0);
    if (fd < 0) {
        xr_tls_context_free(ctx);
        return false;
    }

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
        close(fd);
        xr_tls_context_free(ctx);
        return false;
    }

    XrTlsConn *conn = xr_tls_conn_new(ctx, fd);
    if (!conn) {
        close(fd);
        xr_tls_context_free(ctx);
        return false;
    }

    xr_tls_conn_set_hostname(conn, host);

    if (xr_tls_conn_handshake_client(conn) != XR_TLS_OK) {
        xr_tls_conn_free(conn);
        close(fd);
        xr_tls_context_free(ctx);
        return false;
    }

    const char *alpn = xr_tls_conn_get_alpn(conn);
    bool is_h2 = alpn && strcmp(alpn, "h2") == 0;

    xr_tls_conn_close(conn);
    xr_tls_conn_free(conn);
    close(fd);
    xr_tls_context_free(ctx);

    return is_h2;
}
