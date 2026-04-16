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
 *   Streaming downloads with progress callbacks and resume support
 */

#include "http_stream.h"
#include "http_parser.h"
#include "../../include/xray_platform.h"
#include "../net/dns.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#define DEFAULT_BUFFER_SIZE (64 * 1024)  // 64KB

// Use common parsing functions from http_common.h

/* ========== API Implementation ========== */

void xr_stream_config_init(XrStreamConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(XrStreamConfig));
    config->buffer_size = DEFAULT_BUFFER_SIZE;
    config->timeout_ms = 30000;
    config->follow_redirects = true;
}

XrStreamResult xr_http_download(const char *url,
                                 const char *output_path,
                                 XrHttpProgressCallback on_progress,
                                 void *user_data) {
    XrStreamConfig config;
    xr_stream_config_init(&config);
    config.url = url;
    config.output_path = output_path;
    config.on_progress = on_progress;
    config.user_data = user_data;
    
    return xr_http_stream(&config);
}

XrStreamResult xr_http_stream(const XrStreamConfig *config) {
    XrStreamResult result;
    memset(&result, 0, sizeof(result));
    
    if (!config || !config->url) {
        result.error = XR_HTTP_ERR_URL_PARSE;
        result.error_msg = strdup("Invalid URL");
        return result;
    }
    
    // Parse URL
    XrHttpUrl url;
    if (xr_http_url_parse(config->url, &url) < 0) {
        result.error = XR_HTTP_ERR_URL_PARSE;
        result.error_msg = strdup("URL parse failed");
        return result;
    }
    
    int fd = -1;
    FILE *output_file = NULL;
    char *buffer = NULL;
    
    // Open output file
    if (config->output_path) {
        output_file = fopen(config->output_path, "wb");
        if (!output_file) {
            result.error = XR_HTTP_ERR_SEND;
            result.error_msg = strdup("Cannot open output file");
            goto cleanup;
        }
    }
    
    // DNS resolution (with cache, IPv4/IPv6 dual-stack)
    XrSockAddr resolved_addr;
    if (!xr_dns_resolve(url.host, &resolved_addr, XR_AF_UNSPEC)) {
        result.error = XR_HTTP_ERR_DNS;
        result.error_msg = strdup("DNS resolution failed");
        goto cleanup;
    }
    
    // Create socket
    fd = socket(resolved_addr.family, SOCK_STREAM, 0);
    if (fd < 0) {
        result.error = XR_HTTP_ERR_CONNECT;
        result.error_msg = strdup("Socket creation failed");
        goto cleanup;
    }
    
    // Connect
    struct sockaddr *sa;
    socklen_t sa_len;
    if (resolved_addr.family == AF_INET) {
        resolved_addr.addr.v4.sin_port = htons(url.port);
        sa = (struct sockaddr*)&resolved_addr.addr.v4;
        sa_len = sizeof(struct sockaddr_in);
    } else {
        resolved_addr.addr.v6.sin6_port = htons(url.port);
        sa = (struct sockaddr*)&resolved_addr.addr.v6;
        sa_len = sizeof(struct sockaddr_in6);
    }
    
    if (connect(fd, sa, sa_len) < 0) {
        result.error = XR_HTTP_ERR_CONNECT;
        result.error_msg = strdup("Connection failed");
        goto cleanup;
    }
    
    // Build request
    char request[1024];
    char *p = request;
    p += sprintf(p, "GET %s HTTP/1.1\r\n", url.path ? url.path : "/");
    p += sprintf(p, "Host: %s\r\n", url.host);
    p += sprintf(p, "User-Agent: xray-http/1.0\r\n");
    p += sprintf(p, "Accept: */*\r\n");
    p += sprintf(p, "Connection: close\r\n");
    
    // Range header (resume download)
    if (config->range_start > 0 || config->range_end > 0) {
        if (config->range_end > 0) {
            p += sprintf(p, "Range: bytes=%zu-%zu\r\n", 
                        config->range_start, config->range_end);
        } else {
            p += sprintf(p, "Range: bytes=%zu-\r\n", config->range_start);
        }
    }
    
    p += sprintf(p, "\r\n");
    
    // Send request
    if (send(fd, request, p - request, 0) < 0) {
        result.error = XR_HTTP_ERR_SEND;
        result.error_msg = strdup("Send failed");
        goto cleanup;
    }
    
    // Allocate buffer
    size_t buf_size = config->buffer_size > 0 ? config->buffer_size : DEFAULT_BUFFER_SIZE;
    buffer = (char*)malloc(buf_size);
    if (!buffer) {
        result.error = XR_HTTP_ERR_MEMORY;
        result.error_msg = strdup("Memory allocation failed");
        goto cleanup;
    }
    
    // Receive response headers
    size_t header_buf_size = 8192;
    char *header_buf = (char*)malloc(header_buf_size);
    size_t header_len = 0;
    const char *body_start = NULL;
    
    while (!body_start && header_len < header_buf_size - 1) {
        ssize_t n = xr_recv_timeout(fd, header_buf + header_len, 
                                  header_buf_size - header_len - 1, 
                                  config->timeout_ms);
        if (n <= 0) break;
        header_len += n;
        header_buf[header_len] = '\0';
        body_start = xr_http_find_header_end(header_buf, header_len);
    }
    
    if (!body_start) {
        result.error = XR_HTTP_ERR_PARSE;
        result.error_msg = strdup("Invalid response headers");
        free(header_buf);
        goto cleanup;
    }
    
    // Parse status code
    result.status_code = xr_http_parse_status_code(header_buf);
    if (result.status_code < 200 || result.status_code >= 400) {
        result.error = XR_HTTP_ERR_RECV;
        char msg[64];
        snprintf(msg, sizeof(msg), "HTTP error: %d", result.status_code);
        result.error_msg = strdup(msg);
        free(header_buf);
        goto cleanup;
    }
    
    // Parse Content-Length
    long long content_length = xr_http_parse_content_length(header_buf, body_start - header_buf);
    result.total_size = content_length > 0 ? (size_t)content_length : 0;
    
    // Handle already received body part
    size_t body_in_header = header_len - (body_start - header_buf);
    if (body_in_header > 0) {
        if (output_file) {
            fwrite(body_start, 1, body_in_header, output_file);
        }
        if (config->on_data) {
            config->on_data(body_start, body_in_header, config->user_data);
        }
        result.downloaded = body_in_header;
        
        if (config->on_progress) {
            config->on_progress(result.downloaded, result.total_size, config->user_data);
        }
    }
    
    free(header_buf);
    
    // Receive body
    while (1) {
        ssize_t n = xr_recv_timeout(fd, buffer, buf_size, config->timeout_ms);
        if (n <= 0) break;
        
        if (output_file) {
            fwrite(buffer, 1, n, output_file);
        }
        if (config->on_data) {
            if (config->on_data(buffer, n, config->user_data) != 0) {
                break;  // Callback returning non-0 means cancel
            }
        }
        
        result.downloaded += n;
        
        if (config->on_progress) {
            config->on_progress(result.downloaded, result.total_size, config->user_data);
        }
        
        // Check if download complete
        if (result.total_size > 0 && result.downloaded >= result.total_size) {
            break;
        }
    }
    
    result.completed = (result.total_size == 0) || 
                       (result.downloaded >= result.total_size);
    result.error = XR_HTTP_OK;

cleanup:
    if (fd >= 0) close(fd);
    if (output_file) fclose(output_file);
    if (buffer) free(buffer);
    xr_http_url_free(&url);
    
    return result;
}

XrStreamResult xr_http_resume_download(const char *url,
                                        const char *output_path,
                                        XrHttpProgressCallback on_progress,
                                        void *user_data) {
    XrStreamConfig config;
    xr_stream_config_init(&config);
    config.url = url;
    config.output_path = output_path;
    config.on_progress = on_progress;
    config.user_data = user_data;
    
    // Check local file size
    struct stat st;
    if (stat(output_path, &st) == 0 && st.st_size > 0) {
        config.range_start = st.st_size;
        
        // Open in append mode
        FILE *f = fopen(output_path, "ab");
        if (f) {
            fclose(f);
        }
    }
    
    return xr_http_stream(&config);
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
    if (!result) return;
    free(result->error_msg);
    result->error_msg = NULL;
}
