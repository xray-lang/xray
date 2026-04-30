/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_stream.c - HTTP streaming transfer implementation
 *
 * KEY CONCEPT:
 *   Streaming downloads backed by the coroutine-friendly conn_pool.
 *   All network I/O goes through xr_pooled_conn_read / _write which
 *   yield on EAGAIN, so a download never blocks the worker thread.
 *   File output uses POSIX open/write (fd-based) instead of libc
 *   fopen/fwrite to stay compatible with future AIO integration.
 */

#include "../../src/base/xmalloc.h"
#include "http_stream.h"
#include "http_parser.h"
#include "http.h"
#include "../net/conn_pool.h"
#include "../net/io.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "../../src/os/os_net.h"
#ifdef XR_OS_WINDOWS
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

#define DEFAULT_BUFFER_SIZE (64 * 1024)  // 64KB

/* ========== Internal: per-Isolate pool accessor ========== */

static XrConnPool *stream_get_pool(struct XrayIsolate *X) {
    if (!X)
        return NULL;
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx)
        return NULL;
    if (!ctx->conn_pool) {
        ctx->conn_pool = xr_conn_pool_new();
    }
    return ctx->conn_pool;
}

/* ========== Internal: write full buffer to fd ========== */

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t) n;
    }
    return 0;
}

/* ========== API Implementation ========== */

void xr_stream_config_init(XrStreamConfig *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(XrStreamConfig));
    config->buffer_size = DEFAULT_BUFFER_SIZE;
    config->timeout_ms = 30000;
    config->follow_redirects = true;
}

XrStreamResult xr_http_download(struct XrayIsolate *X, const char *url, const char *output_path,
                                XrHttpProgressCallback on_progress, void *user_data) {
    XrStreamConfig config;
    xr_stream_config_init(&config);
    config.url = url;
    config.output_path = output_path;
    config.on_progress = on_progress;
    config.user_data = user_data;

    return xr_http_stream(X, &config);
}

XrStreamResult xr_http_stream(struct XrayIsolate *X, const XrStreamConfig *config) {
    XrStreamResult result;
    memset(&result, 0, sizeof(result));

    if (!config || !config->url) {
        result.error = XR_HTTP_ERR_URL_PARSE;
        result.error_msg = xr_strdup("Invalid URL");
        return result;
    }

    // Parse URL
    XrHttpUrl url;
    if (xr_http_url_parse(config->url, &url) < 0) {
        result.error = XR_HTTP_ERR_URL_PARSE;
        result.error_msg = xr_strdup("URL parse failed");
        return result;
    }

    XrConnPool *pool = NULL;
    XrPooledConn *conn = NULL;
    int out_fd = -1;  // POSIX fd for output file
    char *buffer = NULL;

    // --- Output file (POSIX fd, not FILE*) ---
    if (config->output_path) {
        int oflags = O_WRONLY | O_CREAT;
        // Resume mode sets range_start > 0; in that case append, else truncate.
        oflags |= (config->range_start > 0) ? O_APPEND : O_TRUNC;
        out_fd = open(config->output_path, oflags, 0644);
        if (out_fd < 0) {
            result.error = XR_HTTP_ERR_SEND;
            result.error_msg = xr_strdup("Cannot open output file");
            goto cleanup;
        }
    }

    // --- Acquire connection via conn_pool (coroutine-friendly) ---
    pool = stream_get_pool(X);
    if (!pool) {
        result.error = XR_HTTP_ERR_CONNECT;
        result.error_msg = xr_strdup("HTTP pool unavailable");
        goto cleanup;
    }

    conn = xr_conn_pool_get(X, pool, url.host, (uint16_t) url.port, url.is_https);
    if (!conn) {
        result.error = XR_HTTP_ERR_CONNECT;
        result.error_msg = xr_strdup("Connection failed");
        goto cleanup;
    }

    // --- Build and send request ---
    {
        char request[1024];
        char *p = request;
        p += sprintf(p, "GET %s HTTP/1.1\r\n", url.path ? url.path : "/");

        // IPv6 Host header needs brackets
        bool is_v6 = (strchr(url.host, ':') != NULL);
        p += sprintf(p, is_v6 ? "Host: [%s]\r\n" : "Host: %s\r\n", url.host);

        p += sprintf(p, "User-Agent: xray-http/1.0\r\n");
        p += sprintf(p, "Accept: */*\r\n");
        p += sprintf(p, "Connection: close\r\n");

        // Range header (resume download)
        if (config->range_start > 0 || config->range_end > 0) {
            if (config->range_end > 0) {
                p += sprintf(p, "Range: bytes=%zu-%zu\r\n", config->range_start, config->range_end);
            } else {
                p += sprintf(p, "Range: bytes=%zu-\r\n", config->range_start);
            }
        }

        p += sprintf(p, "\r\n");

        size_t req_len = (size_t) (p - request);
        if (xr_pooled_conn_write(X, conn, request, req_len) < 0) {
            result.error = XR_HTTP_ERR_SEND;
            result.error_msg = xr_strdup("Send failed");
            goto cleanup;
        }
    }

    // --- Allocate I/O buffer ---
    size_t buf_size = config->buffer_size > 0 ? config->buffer_size : DEFAULT_BUFFER_SIZE;
    buffer = (char *) xr_malloc(buf_size);
    if (!buffer) {
        result.error = XR_HTTP_ERR_MEMORY;
        result.error_msg = xr_strdup("Memory allocation failed");
        goto cleanup;
    }

    // --- Receive response headers ---
    {
        size_t hdr_cap = 8192;
        char *hdr_buf = (char *) xr_malloc(hdr_cap);
        if (!hdr_buf) {
            result.error = XR_HTTP_ERR_MEMORY;
            result.error_msg = xr_strdup("Memory allocation failed");
            goto cleanup;
        }
        size_t hdr_len = 0;
        const char *body_start = NULL;

        while (!body_start && hdr_len < hdr_cap - 1) {
            int n = xr_pooled_conn_read(X, conn, hdr_buf + hdr_len, hdr_cap - hdr_len - 1);
            if (n <= 0)
                break;
            hdr_len += (size_t) n;
            hdr_buf[hdr_len] = '\0';
            body_start = xr_http_find_header_end(hdr_buf, hdr_len);
        }

        if (!body_start) {
            result.error = XR_HTTP_ERR_PARSE;
            result.error_msg = xr_strdup("Invalid response headers");
            xr_free(hdr_buf);
            goto cleanup;
        }

        // Parse status
        result.status_code = xr_http_parse_status_code(hdr_buf);
        if (result.status_code < 200 || result.status_code >= 400) {
            result.error = XR_HTTP_ERR_RECV;
            char msg[64];
            snprintf(msg, sizeof(msg), "HTTP error: %d", result.status_code);
            result.error_msg = xr_strdup(msg);
            xr_free(hdr_buf);
            goto cleanup;
        }

        // Content-Length
        long long cl = xr_http_parse_content_length(hdr_buf, (size_t) (body_start - hdr_buf));
        result.total_size = cl > 0 ? (size_t) cl : 0;

        // Body bytes that arrived together with the headers
        size_t body_in_hdr = hdr_len - (size_t) (body_start - hdr_buf);
        if (body_in_hdr > 0) {
            if (out_fd >= 0)
                write_all(out_fd, body_start, body_in_hdr);
            if (config->on_data)
                config->on_data(body_start, body_in_hdr, config->user_data);
            result.downloaded = body_in_hdr;
            if (config->on_progress)
                config->on_progress(result.downloaded, result.total_size, config->user_data);
        }

        xr_free(hdr_buf);
    }

    // --- Stream body ---
    while (1) {
        int n = xr_pooled_conn_read(X, conn, buffer, buf_size);
        if (n <= 0)
            break;

        if (out_fd >= 0)
            write_all(out_fd, buffer, (size_t) n);
        if (config->on_data) {
            if (config->on_data(buffer, (size_t) n, config->user_data) != 0)
                break;  // Callback returning non-0 → cancel
        }

        result.downloaded += (size_t) n;

        if (config->on_progress)
            config->on_progress(result.downloaded, result.total_size, config->user_data);

        if (result.total_size > 0 && result.downloaded >= result.total_size)
            break;
    }

    result.completed = (result.total_size == 0) || (result.downloaded >= result.total_size);
    result.error = XR_HTTP_OK;

cleanup:
    // Connection: close — don't return to pool, just close.
    if (conn && pool)
        xr_conn_pool_close(X, pool, conn);
    if (out_fd >= 0)
        close(out_fd);
    xr_free(buffer);
    xr_http_url_free(&url);

    return result;
}

XrStreamResult xr_http_resume_download(struct XrayIsolate *X, const char *url,
                                       const char *output_path,
                                       XrHttpProgressCallback on_progress, void *user_data) {
    XrStreamConfig config;
    xr_stream_config_init(&config);
    config.url = url;
    config.output_path = output_path;
    config.on_progress = on_progress;
    config.user_data = user_data;

    // Check local file size for Range header
    struct stat st;
    if (stat(output_path, &st) == 0 && st.st_size > 0) {
        config.range_start = (size_t) st.st_size;
    }

    return xr_http_stream(X, &config);
}

long long xr_http_get_content_length(XrayIsolate *X, const char *url) {
    XrHttpRequestConfig config;
    xr_http_request_config_init(&config);
    config.url = url;
    config.method = XR_HTTP_METHOD_HEAD;

    XrHttpResult result = xr_http_request(X, &config);

    long long length = -1;
    for (int i = 0; i < result.header_count; i++) {
        if (result.headers[i].name_len == 14 &&
            strncasecmp(result.headers[i].name, "Content-Length", 14) == 0) {
            length = atoll(result.headers[i].value);
            break;
        }
    }

    xr_http_result_free(&result);
    return length;
}

void xr_stream_result_free(XrStreamResult *result) {
    if (!result)
        return;
    xr_free(result->error_msg);
    result->error_msg = NULL;
}
