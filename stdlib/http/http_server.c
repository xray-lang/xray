/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_server.c - GMP model HTTP server implementation
 *
 * KEY CONCEPT:
 *   1. listen() creates Listener coroutine
 *   2. Listener coroutine loops accept(), creating Conn coroutine per connection
 *   3. Conn coroutine handles request/response, closes connection when done
 *   4. All I/O via xsocket API, netpoll handles blocked coroutines
 */

#include "../../src/base/xmalloc.h"
#include "http_server.h"
#include "http_router.h"
#include "../ws/ws.h"
#include "../../src/coro/xsocket.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/coro/xyieldable.h"  // Yieldable C function protocol
#include "../../src/vm/xvm_internal.h"

// Coroutine API
extern XrCoroutine *xr_coro_create(XrayIsolate *X, XrClosure *closure, XrValue *args, int arg_count,
                                   const char *name, const char *file, int line);
extern XrCoroutine *xr_coro_create_native(XrayIsolate *X, void (*func)(void *), void *arg,
                                          const char *name);
extern void xr_coro_spawn(XrayIsolate *X, XrCoroutine *coro);

// Per-Coroutine GC root registration
#include "../../src/runtime/gc/xcoro_gc.h"

#include "../../src/os/os_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Pre-generated Response Headers (global shared, zero allocation) ========== */

// 200 OK response headers (different Content-Types)
static const char HTTP_200_PLAIN[] = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: text/plain; charset=utf-8\r\n"
                                     "Connection: keep-alive\r\n"
                                     "Content-Length: ";

static const char HTTP_200_HTML[] = "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: text/html; charset=utf-8\r\n"
                                    "Connection: keep-alive\r\n"
                                    "Content-Length: ";

static const char HTTP_200_JSON[] = "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: application/json; charset=utf-8\r\n"
                                    "Connection: keep-alive\r\n"
                                    "Content-Length: ";

// 301/302 redirect template
static const char HTTP_REDIRECT_TEMPLATE[] = "HTTP/1.1 %d %s\r\n"
                                             "Location: %s\r\n"
                                             "Content-Length: 0\r\n"
                                             "Connection: keep-alive\r\n"
                                             "\r\n";

static const char CRLF_CRLF[] = "\r\n\r\n";

// 404 Not Found
static const char HTTP_404_RESPONSE[] = "HTTP/1.1 404 Not Found\r\n"
                                        "Content-Type: application/json\r\n"
                                        "Content-Length: 24\r\n"
                                        "Connection: keep-alive\r\n"
                                        "\r\n"
                                        "{\"error\": \"Not Found\"}";

/* ========== Internal Helper Functions ========== */

/*
 * Get HTTP status text
 */
static const char *http_status_text(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 500:
            return "Internal Server Error";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        default:
            return "Unknown";
    }
}

/*
 * Parse HTTP request line
 */
static int parse_request_line(char *line, XrHttpReq *req) {
    // Format: METHOD PATH HTTP/1.x
    char *method_end = strchr(line, ' ');
    if (!method_end)
        return -1;
    *method_end = '\0';

    // Parse method
    if (strcmp(line, "GET") == 0)
        req->method = XR_HTTP_METHOD_GET;
    else if (strcmp(line, "POST") == 0)
        req->method = XR_HTTP_METHOD_POST;
    else if (strcmp(line, "PUT") == 0)
        req->method = XR_HTTP_METHOD_PUT;
    else if (strcmp(line, "DELETE") == 0)
        req->method = XR_HTTP_METHOD_DELETE;
    else if (strcmp(line, "HEAD") == 0)
        req->method = XR_HTTP_METHOD_HEAD;
    else if (strcmp(line, "OPTIONS") == 0)
        req->method = XR_HTTP_METHOD_OPTIONS;
    else if (strcmp(line, "PATCH") == 0)
        req->method = XR_HTTP_METHOD_PATCH;
    else
        return -1;

    // Parse path
    char *path_start = method_end + 1;
    char *path_end = strchr(path_start, ' ');
    if (!path_end)
        return -1;
    *path_end = '\0';

    // Separate query string
    char *query = strchr(path_start, '?');
    if (query) {
        *query = '\0';
        req->query = query + 1;
    } else {
        req->query = NULL;
    }

    req->path = path_start;
    return 0;
}

/*
 * Check Keep-Alive
 */
static bool check_keep_alive(const char *headers) {
    // Simple check for Connection header
    const char *conn = strstr(headers, "Connection:");
    if (!conn)
        conn = strstr(headers, "connection:");
    if (!conn)
        return true;  // HTTP/1.1 defaults to Keep-Alive

    conn += 11;  // Skip "Connection:"
    while (*conn == ' ')
        conn++;

    if (strncasecmp(conn, "close", 5) == 0)
        return false;
    return true;
}

/* ========== GC Root Marking Callback ========== */

/*
 * HTTP server GC root marking callback (Per-Coroutine GC)
 *
 * Marks all closures held by server to prevent GC collection.
 * Registered via xr_coro_gc_register_root(), called during coro GC mark phase.
 */
static void http_server_mark_roots(XrCoroGC *gc, void *userdata) {
    XrHttpServer *server = (XrHttpServer *) userdata;
    if (!server)
        return;

    // Mark route closure array
    for (int i = 0; i < server->route_closure_count; i++) {
        if (server->route_closures[i]) {
            xr_coro_gc_markobject(gc, (XrGCHeader *) server->route_closures[i]);
        }
    }

    // Mark connection handler closure
    if (server->conn_handler_closure) {
        xr_coro_gc_markobject(gc, (XrGCHeader *) server->conn_handler_closure);
    }
}

/* ========== Server API ========== */

/*
 * Create server
 */
XrHttpServer *xr_http_server_new(XrayIsolate *isolate) {
    XrHttpServer *server = (XrHttpServer *) xr_calloc(1, sizeof(XrHttpServer));
    if (!server)
        return NULL;

    server->isolate = isolate;
    server->listen_fd = -1;
    server->running = false;

    // Create router
    server->router = xr_router_new();

    // Register GC root with current coroutine's GC
    // Route closures are allocated on the coroutine that calls http.route()
    XrCoroutine *coro = xr_current_coro(isolate);
    if (coro && coro->coro_gc) {
        server->owner_gc = coro->coro_gc;
        xr_coro_gc_register_root(coro->coro_gc, http_server_mark_roots, server);
    }

    return server;
}

/*
 * Free server
 */
void xr_http_server_free(XrHttpServer *server) {
    if (!server)
        return;

    // Unregister from the coroutine GC we registered with
    if (server->owner_gc) {
        xr_coro_gc_unregister_root(server->owner_gc, http_server_mark_roots, server);
    }

    if (server->listen_fd >= 0) {
        xr_closesocket(server->listen_fd);
    }

    if (server->router) {
        xr_router_free(server->router);
    }

    // Free route closure array (closures themselves managed by GC)
    if (server->route_closures) {
        xr_free(server->route_closures);
    }

    xr_free(server);
}

/*
 * Add route
 */
void xr_http_server_route(XrHttpServer *server, XrHttpMethod method, const char *path,
                          XrClosure *handler) {
    if (!server || !server->router || !path || !handler)
        return;

    // Save closure to array (prevent GC collection)
    if (server->route_closure_count >= server->route_closure_capacity) {
        int new_cap = server->route_closure_capacity == 0 ? 16 : server->route_closure_capacity * 2;
        XrClosure **new_arr =
            (XrClosure **) xr_realloc(server->route_closures, new_cap * sizeof(XrClosure *));
        if (!new_arr)
            return;
        server->route_closures = new_arr;
        server->route_closure_capacity = new_cap;
    }
    server->route_closures[server->route_closure_count++] = handler;

    // Register to router (closure stored in user_data)
    xr_router_add(server->router, method, path, (XrRouteHandler) 1, (void *) handler);
}

/*
 * Add static response route
 */
void xr_http_server_static(XrHttpServer *server, XrHttpMethod method, const char *path,
                           const char *response, size_t response_len) {
    if (!server || !server->router || !path || !response)
        return;
    xr_router_add_static(server->router, method, path, response, response_len);
}

/*
 * Read and parse HTTP request
 */
int xr_http_read_request(XrayIsolate *X, int fd, XrHttpReq *req, char *buf, size_t buf_size) {
    if (!req || !buf || buf_size == 0)
        return -1;

    memset(req, 0, sizeof(XrHttpReq));
    req->fd = fd;
    req->keep_alive = true;

    // Batch read request headers (performance optimization: read multiple bytes at once)
    size_t total = 0;
    bool found_end = false;

    while (total < buf_size - 1) {
        // Batch read (up to buffer full)
        size_t to_read = buf_size - 1 - total;
        if (to_read > 4096)
            to_read = 4096;  // Limit single read size

        int n = xr_socket_read(X, fd, buf + total, to_read);
        if (n <= 0) {
            if (total == 0)
                return -1;  // Connection closed
            break;
        }
        total += n;

        // Search for \r\n\r\n in read data
        for (size_t i = ((size_t) total > (size_t) n + 3) ? (size_t) total - (size_t) n - 3 : 0;
             i + 3 < (size_t) total; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                found_end = true;
                total = i + 4;  // Truncate to header end position
                break;
            }
        }
        if (found_end)
            break;
    }

    if (!found_end)
        return -1;
    buf[total] = '\0';

    // Parse request line
    char *line_end = strstr(buf, "\r\n");
    if (!line_end)
        return -1;
    *line_end = '\0';

    if (parse_request_line(buf, req) < 0)
        return -1;

    // Check Keep-Alive
    req->keep_alive = check_keep_alive(line_end + 2);

    // Parse Content-Length and read request body
    const char *headers = line_end + 2;
    long long content_length = xr_http_parse_content_length(headers, total - (headers - buf));

    if (content_length > 0) {
        // Prevent memory exhaustion attack, limit max body size
        if (content_length > 10 * 1024 * 1024) {  // 10MB
            return -1;
        }

        // Allocate body buffer
        req->body = (char *) xr_malloc(content_length + 1);
        if (!req->body)
            return -1;

        // Read body
        size_t body_read = 0;
        while (body_read < (size_t) content_length) {
            ssize_t n = xr_socket_read(X, fd, req->body + body_read, content_length - body_read);
            if (n <= 0) {
                xr_free(req->body);
                req->body = NULL;
                return -1;
            }
            body_read += n;
        }
        req->body[content_length] = '\0';
        req->body_len = content_length;
    }

    return 0;
}

/*
 * Auto-detect Content-Type, choose pre-defined header
 */
static void detect_content_type(const char *body, size_t body_len, const char **header,
                                size_t *header_len) {
    if (body_len > 0 && body) {
        char first = body[0];
        // HTML detection: starts with <
        if (first == '<') {
            *header = HTTP_200_HTML;
            *header_len = sizeof(HTTP_200_HTML) - 1;
            return;
        }
        // JSON detection: starts with { or [
        if (first == '{' || first == '[') {
            *header = HTTP_200_JSON;
            *header_len = sizeof(HTTP_200_JSON) - 1;
            return;
        }
    }
    // Default text/plain
    *header = HTTP_200_PLAIN;
    *header_len = sizeof(HTTP_200_PLAIN) - 1;
}

/*
 * writev zero-copy send response
 * Uses pre-generated header + Content-Length + CRLF + body
 */
static int xr_http_writev_response(XrayIsolate *X, int fd, const char *body, size_t body_len) {
    // Auto-detect Content-Type
    const char *header;
    size_t header_len;
    detect_content_type(body, body_len, &header, &header_len);

    // Content-Length number
    char cl_buf[32];
    int cl_len = snprintf(cl_buf, sizeof(cl_buf), "%zu", body_len);

    // Build iovec
    struct iovec iov[4];
    iov[0].iov_base = (void *) header;
    iov[0].iov_len = header_len;
    iov[1].iov_base = cl_buf;
    iov[1].iov_len = cl_len;
    iov[2].iov_base = (void *) CRLF_CRLF;
    iov[2].iov_len = 4;
    iov[3].iov_base = (void *) body;
    iov[3].iov_len = body_len;

    int iov_count = (body_len > 0) ? 4 : 3;

    // writev send (single syscall)
    ssize_t total = 0;
    for (int i = 0; i < iov_count; i++) {
        total += iov[i].iov_len;
    }

    ssize_t sent = 0;
    while (sent < total) {
        // Recalculate current iovec
        struct iovec cur_iov[4];
        int cur_cnt = 0;
        size_t skip = sent;

        for (int i = 0; i < iov_count; i++) {
            if (skip >= iov[i].iov_len) {
                skip -= iov[i].iov_len;
                continue;
            }
            cur_iov[cur_cnt].iov_base = (char *) iov[i].iov_base + skip;
            cur_iov[cur_cnt].iov_len = iov[i].iov_len - skip;
            cur_cnt++;
            skip = 0;
        }

        ssize_t n = writev(fd, cur_iov, cur_cnt);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // writev blocked, fallback to xr_socket_write (coroutine-safe)
                for (int i = 0; i < cur_cnt; i++) {
                    int wn = xr_socket_write(X, fd, cur_iov[i].iov_base, cur_iov[i].iov_len);
                    if (wn < 0)
                        return -1;
                    sent += wn;
                }
                continue;
            }
            return -1;
        }
        sent += n;
    }

    return 0;
}

/*
 * Send HTTP response (compatible with old interface)
 */
int xr_http_write_response(XrayIsolate *X, int fd, XrHttpResp *resp) {
    if (!resp)
        return -1;

    // If content_type specified, use full build
    if (resp->content_type) {
        char header[512];
        int header_len = snprintf(header, sizeof(header),
                                  "HTTP/1.1 %d %s\r\n"
                                  "Content-Type: %s\r\n"
                                  "Content-Length: %zu\r\n"
                                  "Connection: keep-alive\r\n"
                                  "\r\n",
                                  resp->status, http_status_text(resp->status), resp->content_type,
                                  resp->body_len);

        int n = xr_socket_write(X, fd, header, header_len);
        if (n < 0)
            return -1;

        if (resp->body && resp->body_len > 0) {
            n = xr_socket_write(X, fd, resp->body, resp->body_len);
            if (n < 0)
                return -1;
        }
        return 0;
    }

    // Otherwise use writev zero-copy + auto-detect
    return xr_http_writev_response(X, fd, resp->body, resp->body_len);
}

/*
 * Send simple text response (writev zero-copy + auto-detect Content-Type)
 */
int xr_http_send_text(XrayIsolate *X, int fd, int status, const char *body) {
    (void) status;  // Currently only 200 pre-generated header supported
    size_t body_len = body ? strlen(body) : 0;
    return xr_http_writev_response(X, fd, body, body_len);
}

/*
 * Send error response
 */
int xr_http_send_error(XrayIsolate *X, int fd, int status, const char *message) {
    // 404 uses pre-defined response (single send)
    if (status == 404 && (message == NULL || strcmp(message, "Not Found") == 0)) {
        return xr_socket_write(X, fd, HTTP_404_RESPONSE, sizeof(HTTP_404_RESPONSE) - 1);
    }

    // Other errors dynamically built
    char body[256];
    snprintf(body, sizeof(body), "{\"error\": \"%s\"}",
             message ? message : http_status_text(status));

    XrHttpResp resp = {.status = status,
                       .content_type = "application/json",
                       .body = body,
                       .body_len = strlen(body),
                       .body_owned = false};
    return xr_http_write_response(X, fd, &resp);
}

/*
 * Send redirect response
 */
int xr_http_send_redirect(XrayIsolate *X, int fd, int status, const char *location) {
    if (!location)
        return -1;

    // Use pre-defined redirect template
    char response[512];
    const char *status_text = (status == 301) ? "Moved Permanently" : "Found";
    int len =
        snprintf(response, sizeof(response), HTTP_REDIRECT_TEMPLATE, status, status_text, location);

    return xr_socket_write(X, fd, response, len);
}

/*
 * Try to find a prebuilt response for raw HTTP data.
 * Quick-parses method+path, does route lookup, returns prebuilt pointer.
 * Does NOT perform I/O - caller writes via coroutine-safe path.
 */
bool xr_http_try_prebuilt(XrRouter *router, const char *raw_data, size_t data_len,
                          const char **out_resp, size_t *out_len) {
    if (!router || !raw_data || data_len < 14)
        return false;

    // Quick parse method (only GET for prebuilt fast path)
    const char *p = raw_data;
    const char *end = raw_data + data_len;
    XrHttpMethod method;

    if (p[0] == 'G' && p[1] == 'E' && p[2] == 'T' && p[3] == ' ') {
        method = XR_HTTP_METHOD_GET;
        p += 4;
    } else {
        return false;
    }

    // Quick parse path (stop at space or '?')
    const char *path = p;
    while (p < end && *p != ' ' && *p != '?')
        p++;
    size_t path_len = p - path;
    if (path_len == 0)
        return false;

    // Route lookup (stack-only, zero GC allocation)
    XrRouteParams params;
    params.count = 0;
    void *user_data = NULL;
    const char *static_resp = NULL, *prebuilt_resp = NULL;
    size_t static_len = 0, prebuilt_len = 0;

    xr_router_find(router, method, path, path_len, &params, &user_data, &static_resp, &static_len,
                   &prebuilt_resp, &prebuilt_len);

    if (!prebuilt_resp || prebuilt_len == 0 || params.count > 0)
        return false;

    *out_resp = prebuilt_resp;
    *out_len = prebuilt_len;
    return true;
}

/*
 * Stop server
 */
void xr_http_server_stop(XrHttpServer *server) {
    if (!server)
        return;

    server->running = false;

    // Close listen socket, wake up accept
    if (server->listen_fd >= 0) {
        xr_closesocket(server->listen_fd);
        server->listen_fd = -1;
    }
}

/*
 * Set WebSocket handler
 */
void xr_http_server_set_ws_handler(XrHttpServer *server, XrWsConnectionHandler handler,
                                   void *user_data) {
    if (!server)
        return;
    server->ws_handler = handler;
    server->ws_user_data = user_data;
}
