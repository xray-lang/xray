/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2_server.c - High-performance HTTP/2 server implementation
 *
 * KEY CONCEPT:
 *   Event-driven architecture with cross-platform support:
 *   - macOS: kqueue
 *   - Linux: epoll
 *   - Windows: IOCP
 */

#include "../../src/base/xmalloc.h"
#include "http2_server.h"
#include "http2.h"
#include "../../include/xray_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#define xr_close_socket closesocket
#else
#define xr_close_socket close
#endif

#if defined(__APPLE__)
#include <sys/event.h>
#define XR_USE_KQUEUE 1
#elif defined(__linux__)
#include <sys/epoll.h>
#define XR_USE_EPOLL 1
#elif defined(_WIN32)
#define XR_USE_IOCP 1
#endif

/* ========== Constants ========== */

#define H2_MAX_EVENTS 256
#define H2_MAX_CONNS 1024
#define H2_RECV_BUF_SIZE 16384
#define H2_BACKLOG 1024

/* ========== Connection State ========== */

typedef enum {
    H2_CONN_FREE = 0,
    H2_CONN_TLS_HANDSHAKE,
    H2_CONN_PREFACE,
    H2_CONN_ACTIVE,
    H2_CONN_CLOSING
} H2ConnState;

/* ========== High-Performance Connection Structure ========== */

typedef struct XrH2FastConn {
    xr_socket_t fd;
    H2ConnState state;
    XrTlsConn *tls;
    XrH2Conn *h2;

    // Receive buffer
    uint8_t recv_buf[H2_RECV_BUF_SIZE];
    size_t recv_len;

    // Preface state
    bool preface_received;
    bool settings_sent;
} XrH2FastConn;

/* ========== Helper Functions ========== */

/* ========== Connection Management (Per-Server) ========== */

static void h2_server_conn_pool_init(XrH2Server *server, int max_conns) {
    server->conn_pool.size = max_conns;
    server->conn_pool.conns = (XrH2FastConn *) xr_calloc(max_conns, sizeof(XrH2FastConn));
    for (int i = 0; i < max_conns; i++) {
        server->conn_pool.conns[i].fd = XR_INVALID_SOCKET;
        server->conn_pool.conns[i].state = H2_CONN_FREE;
    }
}

static void h2_server_conn_pool_destroy(XrH2Server *server) {
    if (server->conn_pool.conns) {
        for (int i = 0; i < server->conn_pool.size; i++) {
            if (server->conn_pool.conns[i].fd != XR_INVALID_SOCKET) {
                xr_close_socket(server->conn_pool.conns[i].fd);
            }
            if (server->conn_pool.conns[i].tls) {
                xr_tls_conn_close(server->conn_pool.conns[i].tls);
                xr_tls_conn_free(server->conn_pool.conns[i].tls);
            }
            if (server->conn_pool.conns[i].h2) {
                xr_h2_conn_free(server->conn_pool.conns[i].h2);
            }
        }
        xr_free(server->conn_pool.conns);
        server->conn_pool.conns = NULL;
    }
}

static __attribute__((unused)) XrH2FastConn *h2_conn_get(XrH2Server *server, int fd) {
    if (fd < 0 || fd >= server->conn_pool.size)
        return NULL;
    return &server->conn_pool.conns[fd];
}

static void h2_conn_reset(XrH2FastConn *conn) {
    conn->recv_len = 0;
    conn->preface_received = false;
    conn->settings_sent = false;
}

static void h2_conn_close(XrH2Server *server, XrH2FastConn *conn) {
    if (conn->fd == XR_INVALID_SOCKET)
        return;

#ifdef XR_USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, conn->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(server->event_fd, &ev, 1, NULL, 0, NULL);
    EV_SET(&ev, conn->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(server->event_fd, &ev, 1, NULL, 0, NULL);
#elif defined(XR_USE_EPOLL)
    epoll_ctl(server->event_fd, EPOLL_CTL_DEL, conn->fd, NULL);
#endif

    if (conn->h2) {
        xr_h2_send_goaway(conn->h2, conn->h2->last_stream_id, XR_H2_NO_ERROR);
        xr_h2_conn_free(conn->h2);
        conn->h2 = NULL;
    }

    if (conn->tls) {
        xr_tls_conn_close(conn->tls);
        xr_tls_conn_free(conn->tls);
        conn->tls = NULL;
    }

    xr_close_socket(conn->fd);
    conn->fd = XR_INVALID_SOCKET;
    conn->state = H2_CONN_FREE;
}

/* ========== Request Processing ========== */

static void process_streams(XrH2Server *server, XrH2FastConn *conn) {
    if (!conn->h2 || !server->on_request)
        return;

    // Traverse stream hash table
    for (uint32_t i = 0; i < conn->h2->stream_hash.nbuckets; i++) {
        XrH2Stream *stream = conn->h2->stream_hash.buckets[i];
        while (stream) {
            if (stream->state == XR_H2_STREAM_HALF_CLOSED_REMOTE) {
                // Create request context
                XrH2Context ctx = {0};
                ctx.conn = conn->h2;
                ctx.stream = stream;

                // Parse method and path from headers
                ctx.method = "GET";
                ctx.path = "/";

                // Call request handler
                server->on_request(&ctx, server->user_data);

                // Mark stream as processed
                stream->state = XR_H2_STREAM_STATE_CLOSED;
            }
            stream = stream->next;
        }
    }
}

/* ========== TLS Handshake Handling ========== */

static int handle_tls_handshake(XrH2Server *server, XrH2FastConn *conn) {
    (void) server;

    if (!conn->tls) {
        conn->state = H2_CONN_PREFACE;
        return 0;
    }

    int result = xr_tls_conn_handshake_server(conn->tls);
    if (result == XR_TLS_OK) {
        // Check ALPN
        const char *alpn = xr_tls_conn_get_alpn(conn->tls);
        if (!alpn || strcmp(alpn, "h2") != 0) {
            return -1;
        }
        conn->state = H2_CONN_PREFACE;
        return 0;
    }
    // TLS handshake failed
    return -1;
}

/* ========== Preface Handling ========== */

static int handle_preface(XrH2Server *server, XrH2FastConn *conn) {
    (void) server;

    // Read data
    ssize_t n;
    if (conn->tls) {
        n = xr_tls_conn_read(conn->tls, conn->recv_buf + conn->recv_len,
                             H2_RECV_BUF_SIZE - conn->recv_len);
    } else {
        n = recv(conn->fd, conn->recv_buf + conn->recv_len, H2_RECV_BUF_SIZE - conn->recv_len, 0);
    }

    if (n < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK)
            return 0;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
#endif
        return -1;
    }
    if (n == 0)
        return -1;

    conn->recv_len += n;

    // Check preface
    if (!conn->preface_received) {
        if (conn->recv_len < XR_HTTP2_PREFACE_LEN) {
            return 0;  // Need more data
        }

        if (memcmp(conn->recv_buf, XR_HTTP2_PREFACE, XR_HTTP2_PREFACE_LEN) != 0) {
            return -1;  // Invalid preface
        }

        conn->preface_received = true;

        // Remove processed data
        conn->recv_len -= XR_HTTP2_PREFACE_LEN;
        if (conn->recv_len > 0) {
            memmove(conn->recv_buf, conn->recv_buf + XR_HTTP2_PREFACE_LEN, conn->recv_len);
        }
    }

    // Send server SETTINGS
    if (!conn->settings_sent) {
        // Create HTTP/2 connection
        conn->h2 = xr_h2_conn_new(conn->fd, conn->tls, false);
        if (!conn->h2)
            return -1;

        if (xr_h2_send_settings(conn->h2) < 0) {
            return -1;
        }

        conn->settings_sent = true;
        conn->state = H2_CONN_ACTIVE;
    }

    return 0;
}

/* ========== Active Connection Read ========== */

static int handle_read(XrH2Server *server, XrH2FastConn *conn) {
    switch (conn->state) {
        case H2_CONN_TLS_HANDSHAKE:
            return handle_tls_handshake(server, conn);

        case H2_CONN_PREFACE:
            return handle_preface(server, conn);

        case H2_CONN_ACTIVE:
            if (!conn->h2)
                return -1;

            // Receive HTTP/2 frames
            if (xr_h2_recv(conn->h2) < 0) {
                if (conn->h2->goaway_received) {
                    return -1;
                }
            }

            // Process requests
            process_streams(server, conn);
            return 0;

        default:
            return -1;
    }
}

/* ========== Accept New Connection ========== */

static void handle_accept(XrH2Server *server) {
    while (1) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);

        xr_socket_t fd = accept(server->listen_fd, (struct sockaddr *) &addr, &len);
        if (fd == XR_INVALID_SOCKET) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK)
                break;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
#endif
            break;
        }

        if ((int) fd >= server->conn_pool.size) {
            xr_close_socket(fd);
            continue;
        }

        xr_socket_set_nonblocking(fd);
        xr_socket_set_nodelay(fd, true);

        XrH2FastConn *conn = &server->conn_pool.conns[(int) fd];
        conn->fd = fd;
        h2_conn_reset(conn);

        // Create TLS connection
        if (server->tls_ctx) {
            conn->tls = xr_tls_conn_new(server->tls_ctx, fd);
            if (!conn->tls) {
                xr_close_socket(fd);
                conn->fd = XR_INVALID_SOCKET;
                continue;
            }
            conn->state = H2_CONN_TLS_HANDSHAKE;
        } else {
            conn->state = H2_CONN_PREFACE;
        }

        server->connection_count++;

#ifdef XR_USE_KQUEUE
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, conn);
        kevent(server->event_fd, &ev, 1, NULL, 0, NULL);
#elif defined(XR_USE_EPOLL)
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = conn;
        epoll_ctl(server->event_fd, EPOLL_CTL_ADD, fd, &ev);
#endif
    }
}

/* ========== Event Loop - kqueue ========== */

#ifdef XR_USE_KQUEUE
static void event_loop_kqueue(XrH2Server *server) {
    struct kevent events[H2_MAX_EVENTS];

    while (server->running) {
        int n = kevent(server->event_fd, NULL, 0, events, H2_MAX_EVENTS, NULL);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            struct kevent *ev = &events[i];

            if ((int) ev->ident == server->listen_fd) {
                handle_accept(server);
                continue;
            }

            XrH2FastConn *conn = (XrH2FastConn *) ev->udata;
            if (!conn)
                continue;

            if (ev->flags & EV_EOF) {
                h2_conn_close(server, conn);
                continue;
            }

            if (ev->filter == EVFILT_READ) {
                if (handle_read(server, conn) < 0) {
                    h2_conn_close(server, conn);
                }
            }
        }
    }
}
#endif

/* ========== Event Loop - epoll ========== */

#ifdef XR_USE_EPOLL
static void event_loop_epoll(XrH2Server *server) {
    struct epoll_event events[H2_MAX_EVENTS];

    while (server->running) {
        int n = epoll_wait(server->event_fd, events, H2_MAX_EVENTS, -1);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            struct epoll_event *ev = &events[i];

            if (ev->data.fd == server->listen_fd) {
                handle_accept(server);
                continue;
            }

            XrH2FastConn *conn = (XrH2FastConn *) ev->data.ptr;
            if (!conn)
                continue;

            if (ev->events & (EPOLLERR | EPOLLHUP)) {
                h2_conn_close(server, conn);
                continue;
            }

            if (ev->events & EPOLLIN) {
                if (handle_read(server, conn) < 0) {
                    h2_conn_close(server, conn);
                }
            }
        }
    }
}
#endif

/* ========== Public API ========== */

void xr_h2_server_config_init(XrH2ServerConfig *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(XrH2ServerConfig));
    config->host = "0.0.0.0";
    config->port = 8443;
    config->max_connections = H2_MAX_CONNS;
    config->max_streams_per_conn = 100;
    config->max_header_list_size = 16384;
    config->max_frame_size = 16384;
}

XrH2Server *xr_h2_server_new(const XrH2ServerConfig *config) {
    if (!config)
        return NULL;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    XrH2Server *server = (XrH2Server *) xr_calloc(1, sizeof(XrH2Server));
    if (!server)
        return NULL;

    server->config = *config;
    server->max_connections = config->max_connections;
    server->listen_fd = -1;
    server->event_fd = -1;

    pthread_mutex_init(&server->conn_lock, NULL);

    // Create TLS context
    if (config->cert_file && config->key_file) {
        server->tls_ctx = xr_tls_context_new_server(config->cert_file, config->key_file);
        if (!server->tls_ctx) {
            xr_free(server);
            return NULL;
        }
        xr_tls_context_set_alpn_callback(server->tls_ctx, NULL, NULL);
    }

    // Initialize connection pool
    h2_server_conn_pool_init(server, server->max_connections);

    return server;
}

void xr_h2_server_free(XrH2Server *server) {
    if (!server)
        return;

    xr_h2_server_stop(server);

    pthread_mutex_destroy(&server->conn_lock);

    h2_server_conn_pool_destroy(server);

    if (server->tls_ctx)
        xr_tls_context_free(server->tls_ctx);

#ifdef XR_USE_KQUEUE
    if (server->event_fd >= 0)
        close(server->event_fd);
#elif defined(XR_USE_EPOLL)
    if (server->event_fd >= 0)
        close(server->event_fd);
#endif

    if (server->listen_fd >= 0)
        xr_close_socket(server->listen_fd);

    xr_free(server);

#ifdef _WIN32
    WSACleanup();
#endif
}

void xr_h2_server_on_request(XrH2Server *server, void (*handler)(XrH2Context *ctx, void *user_data),
                             void *user_data) {
    if (!server)
        return;
    server->on_request = handler;
    server->user_data = user_data;
}

int xr_h2_server_listen(XrH2Server *server) {
    if (!server)
        return -1;

    // Create socket
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0)
        return -1;

    xr_socket_set_reuseaddr(server->listen_fd, true);
    xr_socket_set_nonblocking(server->listen_fd);

    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server->config.port);
    addr.sin_addr.s_addr = inet_addr(server->config.host);

    if (bind(server->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        xr_close_socket(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    if (listen(server->listen_fd, H2_BACKLOG) < 0) {
        xr_close_socket(server->listen_fd);
        server->listen_fd = -1;
        return -1;
    }

    // Create event fd
#ifdef XR_USE_KQUEUE
    server->event_fd = kqueue();
    if (server->event_fd < 0) {
        xr_close_socket(server->listen_fd);
        return -1;
    }
    struct kevent ev;
    EV_SET(&ev, server->listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(server->event_fd, &ev, 1, NULL, 0, NULL);
#elif defined(XR_USE_EPOLL)
    server->event_fd = epoll_create1(0);
    if (server->event_fd < 0) {
        xr_close_socket(server->listen_fd);
        return -1;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server->listen_fd;
    epoll_ctl(server->event_fd, EPOLL_CTL_ADD, server->listen_fd, &ev);
#endif

    printf("HTTP/2 server listening on %s:%d\n", server->config.host, server->config.port);
    server->running = true;

#ifdef XR_USE_KQUEUE
    event_loop_kqueue(server);
#elif defined(XR_USE_EPOLL)
    event_loop_epoll(server);
#else
    fprintf(stderr, "No event mechanism available\n");
    return -1;
#endif

    return 0;
}

void xr_h2_server_stop(XrH2Server *server) {
    if (!server)
        return;
    server->running = false;

    if (server->listen_fd >= 0) {
        shutdown(server->listen_fd, SHUT_RDWR);
        xr_close_socket(server->listen_fd);
        server->listen_fd = -1;
    }
}

/* ========== Response API ========== */

int xr_h2_ctx_respond(XrH2Context *ctx, int status, const char **names, const char **values,
                      int count) {
    if (!ctx || !ctx->conn || !ctx->stream || ctx->headers_sent)
        return -1;

    const char *all_names[33];
    size_t all_name_lens[33];
    const char *all_values[33];
    size_t all_value_lens[33];
    int total = 0;

    char status_str[4];
    snprintf(status_str, sizeof(status_str), "%d", status);
    all_names[total] = ":status";
    all_name_lens[total] = 7;
    all_values[total] = status_str;
    all_value_lens[total] = strlen(status_str);
    total++;

    for (int i = 0; i < count && total < 32; i++) {
        all_names[total] = names[i];
        all_name_lens[total] = strlen(names[i]);
        all_values[total] = values[i];
        all_value_lens[total] = strlen(values[i]);
        total++;
    }

    if (xr_h2_send_headers(ctx->conn, ctx->stream, all_names, all_name_lens, all_values,
                           all_value_lens, total, false) < 0) {
        return -1;
    }

    ctx->headers_sent = true;
    return 0;
}

int xr_h2_ctx_write(XrH2Context *ctx, const void *data, size_t len) {
    if (!ctx || !ctx->conn || !ctx->stream)
        return -1;

    if (!ctx->headers_sent) {
        if (xr_h2_ctx_respond(ctx, 200, NULL, NULL, 0) < 0) {
            return -1;
        }
    }

    return xr_h2_send_data(ctx->conn, ctx->stream, data, len, false);
}

int xr_h2_ctx_end(XrH2Context *ctx) {
    if (!ctx || !ctx->conn || !ctx->stream || ctx->response_ended)
        return -1;

    if (!ctx->headers_sent) {
        const char *names[] = {":status"};
        size_t name_lens[] = {7};
        const char *values[] = {"200"};
        size_t value_lens[] = {3};

        if (xr_h2_send_headers(ctx->conn, ctx->stream, names, name_lens, values, value_lens, 1,
                               true) < 0) {
            return -1;
        }
    } else {
        if (xr_h2_send_data(ctx->conn, ctx->stream, NULL, 0, true) < 0) {
            return -1;
        }
    }

    ctx->response_ended = true;
    return 0;
}

int xr_h2_ctx_send(XrH2Context *ctx, int status, const char *content_type, const void *body,
                   size_t body_len) {
    if (!ctx)
        return -1;

    const char *names[2];
    const char *values[2];
    int count = 0;

    if (content_type) {
        names[count] = "content-type";
        values[count] = content_type;
        count++;
    }

    char len_str[32];
    snprintf(len_str, sizeof(len_str), "%zu", body_len);
    names[count] = "content-length";
    values[count] = len_str;
    count++;

    if (xr_h2_ctx_respond(ctx, status, names, values, count) < 0) {
        return -1;
    }

    if (body && body_len > 0) {
        if (xr_h2_ctx_write(ctx, body, body_len) < 0) {
            return -1;
        }
    }

    return xr_h2_ctx_end(ctx);
}

int xr_h2_ctx_push(XrH2Context *ctx, const char *path, const char *content_type, const void *data,
                   size_t len) {
    if (!ctx || !ctx->conn || !ctx->stream || !path)
        return -1;

    XrH2Stream *push_stream = xr_h2_stream_new(ctx->conn);
    if (!push_stream)
        return -1;

    uint8_t frame[1024];
    uint8_t *p = frame + XR_H2_FRAME_HEADER_SIZE;

    *p++ = 0;
    *p++ = 0;
    *p++ = (push_stream->id >> 8) & 0xFF;
    *p++ = push_stream->id & 0xFF;

    *p++ = 0x82;
    *p++ = 0x04;
    *p++ = (uint8_t) strlen(path);
    memcpy(p, path, strlen(path));
    p += strlen(path);
    *p++ = 0x87;
    *p++ = 0x01;
    *p++ = (uint8_t) strlen(ctx->authority ? ctx->authority : "localhost");
    memcpy(p, ctx->authority ? ctx->authority : "localhost",
           strlen(ctx->authority ? ctx->authority : "localhost"));
    p += strlen(ctx->authority ? ctx->authority : "localhost");

    size_t payload_len = p - (frame + XR_H2_FRAME_HEADER_SIZE);

    XrH2FrameHeader header = {.length = (uint32_t) payload_len,
                              .type = XR_H2_FRAME_PUSH_PROMISE,
                              .flags = XR_H2_FLAG_END_HEADERS,
                              .stream_id = ctx->stream->id};
    xr_h2_write_frame_header(frame, &header);

    if (write(ctx->conn->fd, frame, XR_H2_FRAME_HEADER_SIZE + payload_len) < 0) {
        return -1;
    }

    XrH2Context push_ctx = {0};
    push_ctx.conn = ctx->conn;
    push_ctx.stream = push_stream;

    return xr_h2_ctx_send(&push_ctx, 200, content_type, data, len);
}
