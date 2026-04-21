/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2_server.h - HTTP/2 server implementation
 *
 * KEY CONCEPT:
 *   HTTP/2 server with Server Push and multiplexing support.
 */

#ifndef XR_STDLIB_HTTP2_SERVER_H
#define XR_STDLIB_HTTP2_SERVER_H

#include "../../src/base/xdefs.h"
#include "http2.h"
#include "../net/tls.h"
#include <stdbool.h>
#include <pthread.h>

/* ========== HTTP/2 Server Config ========== */

typedef struct XrH2ServerConfig {
    const char *host;               // Listen address
    int port;                       // Listen port
    const char *cert_file;          // TLS certificate file
    const char *key_file;           // TLS private key file
    int max_connections;            // Max connections
    int max_streams_per_conn;       // Max streams per connection
    size_t max_header_list_size;    // Max header list size
    size_t max_frame_size;          // Max frame size
} XrH2ServerConfig;

/* ========== HTTP/2 Request Context ========== */

typedef struct XrH2Context {
    XrH2Conn *conn;                 // HTTP/2 connection
    XrH2Stream *stream;             // Current stream
    
    // Request info
    const char *method;
    const char *path;
    const char *authority;
    const char *scheme;
    char **header_names;
    char **header_values;
    int header_count;
    char *body;
    size_t body_len;
    
    // Response state
    bool headers_sent;
    bool response_ended;
} XrH2Context;

/* ========== HTTP/2 Server Connection Pool ========== */

// Forward declaration
struct XrH2FastConn;

typedef struct XrH2ServerConnPool {
    struct XrH2FastConn *conns;     // Connection array (indexed by fd)
    int size;                        // Array size
} XrH2ServerConnPool;

/* ========== HTTP/2 Server ========== */

typedef struct XrH2Server {
    int listen_fd;                  // Listen socket
    int event_fd;                   // kqueue/epoll fd
    XrTlsContext *tls_ctx;          // TLS context
    XrH2ServerConfig config;        // Configuration
    
    // Connection management (per-server pool)
    XrH2ServerConnPool conn_pool;   // Per-server connection pool
    XrH2Conn **connections;
    int connection_count;
    int max_connections;
    pthread_mutex_t conn_lock;
    
    // State
    volatile bool running;
    
    // Callbacks
    void (*on_request)(XrH2Context *ctx, void *user_data);
    void *user_data;
} XrH2Server;

/* ========== API ========== */

/*
 * Initialize server config
 */
XR_FUNC void xr_h2_server_config_init(XrH2ServerConfig *config);

/*
 * Create HTTP/2 server
 */
XR_FUNC XrH2Server* xr_h2_server_new(const XrH2ServerConfig *config);

/*
 * Free server
 */
XR_FUNC void xr_h2_server_free(XrH2Server *server);

/*
 * Set request handler callback
 */
XR_FUNC void xr_h2_server_on_request(XrH2Server *server,
                              void (*handler)(XrH2Context *ctx, void *user_data),
                              void *user_data);

/*
 * Start server (blocking)
 */
XR_FUNC int xr_h2_server_listen(XrH2Server *server);

/*
 * Stop server
 */
XR_FUNC void xr_h2_server_stop(XrH2Server *server);

/* ========== Response API ========== */

/*
 * Send response headers
 */
XR_FUNC int xr_h2_ctx_respond(XrH2Context *ctx, int status,
                       const char **names, const char **values, int count);

/*
 * Send response body
 */
XR_FUNC int xr_h2_ctx_write(XrH2Context *ctx, const void *data, size_t len);

/*
 * End response
 */
XR_FUNC int xr_h2_ctx_end(XrH2Context *ctx);

/*
 * Send complete response (headers + body)
 */
XR_FUNC int xr_h2_ctx_send(XrH2Context *ctx, int status,
                    const char *content_type,
                    const void *body, size_t body_len);

/* ========== Server Push API ========== */

/*
 * Push resource
 * 
 * ctx: Current request context
 * path: Push resource path
 * content_type: Content type
 * data: Resource data
 * len: Data length
 */
XR_FUNC int xr_h2_ctx_push(XrH2Context *ctx, const char *path,
                    const char *content_type,
                    const void *data, size_t len);

#endif
