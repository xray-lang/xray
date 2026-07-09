/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_internal.h - Private HTTP native module boundary
 */

#ifndef XR_STDLIB_HTTP_INTERNAL_H
#define XR_STDLIB_HTTP_INTERNAL_H

#include "http.h"
#include "http_client.h"
#include "http_parser.h"
#include "../net/conn_pool.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/runtime/xisolate_internal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct XrClosure;

#define XR_ROUTER_MAX_PARAMS 16

/* ========== HTTP/2 Data Plane ========== */

#define XR_HTTP2_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define XR_HTTP2_PREFACE_LEN 24

typedef enum {
    XR_H2_FRAME_DATA = 0x0,
    XR_H2_FRAME_HEADERS = 0x1,
    XR_H2_FRAME_PRIORITY = 0x2,
    XR_H2_FRAME_RST_STREAM = 0x3,
    XR_H2_FRAME_SETTINGS = 0x4,
    XR_H2_FRAME_PUSH_PROMISE = 0x5,
    XR_H2_FRAME_PING = 0x6,
    XR_H2_FRAME_GOAWAY = 0x7,
    XR_H2_FRAME_WINDOW_UPDATE = 0x8,
    XR_H2_FRAME_CONTINUATION = 0x9
} XrH2FrameType;

#define XR_H2_FLAG_END_STREAM 0x1
#define XR_H2_FLAG_END_HEADERS 0x4
#define XR_H2_FLAG_PADDED 0x8
#define XR_H2_FLAG_PRIORITY 0x20
#define XR_H2_FLAG_ACK 0x1

typedef enum {
    XR_H2_SETTINGS_HEADER_TABLE_SIZE = 0x1,
    XR_H2_SETTINGS_ENABLE_PUSH = 0x2,
    XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    XR_H2_SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
    XR_H2_SETTINGS_MAX_FRAME_SIZE = 0x5,
    XR_H2_SETTINGS_MAX_HEADER_LIST_SIZE = 0x6
} XrH2SettingsId;

#define XR_H2_DEFAULT_HEADER_TABLE_SIZE 4096
#define XR_H2_DEFAULT_ENABLE_PUSH 1
#define XR_H2_DEFAULT_MAX_CONCURRENT_STREAMS 100
#define XR_H2_DEFAULT_INITIAL_WINDOW_SIZE 65535
#define XR_H2_DEFAULT_MAX_FRAME_SIZE 16384
#define XR_H2_DEFAULT_MAX_HEADER_LIST_SIZE UINT32_MAX

typedef enum {
    XR_H2_NO_ERROR = 0x0,
    XR_H2_PROTOCOL_ERROR = 0x1,
    XR_H2_INTERNAL_ERROR = 0x2,
    XR_H2_FLOW_CONTROL_ERROR = 0x3,
    XR_H2_SETTINGS_TIMEOUT = 0x4,
    XR_H2_STREAM_CLOSED = 0x5,
    XR_H2_FRAME_SIZE_ERROR = 0x6,
    XR_H2_REFUSED_STREAM = 0x7,
    XR_H2_CANCEL = 0x8,
    XR_H2_COMPRESSION_ERROR = 0x9,
    XR_H2_CONNECT_ERROR = 0xa,
    XR_H2_ENHANCE_YOUR_CALM = 0xb,
    XR_H2_INADEQUATE_SECURITY = 0xc,
    XR_H2_HTTP_1_1_REQUIRED = 0xd
} XrH2ErrorCode;

typedef struct {
    uint32_t length;
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id;
} XrH2FrameHeader;

#define XR_H2_FRAME_HEADER_SIZE 9

typedef struct XrHpackEntry {
    char *name;
    size_t name_len;
    char *value;
    size_t value_len;
    struct XrHpackEntry *next;
    struct XrHpackEntry *prev;
} XrHpackEntry;

typedef struct {
    XrHpackEntry *entries;
    XrHpackEntry *tail;
    size_t size;
    size_t max_size;
    int count;
} XrHpackTable;

typedef enum {
    XR_H2_STREAM_IDLE,
    XR_H2_STREAM_OPEN,
    XR_H2_STREAM_HALF_CLOSED_LOCAL,
    XR_H2_STREAM_HALF_CLOSED_REMOTE,
    XR_H2_STREAM_STATE_CLOSED
} XrH2StreamState;

typedef struct XrH2Stream {
    uint32_t id;
    XrH2StreamState state;
    int32_t window_size;
    int status;

    char *headers_buf;
    size_t headers_len;
    char *data_buf;
    size_t data_len;
    size_t data_cap;

    struct XrH2Stream *next;
} XrH2Stream;

#define XR_H2_STREAM_HASH_INIT_CAP 16
#define XR_H2_STREAM_HASH_LOAD_NUM 3
#define XR_H2_STREAM_HASH_LOAD_DEN 4

typedef struct {
    XrH2Stream **buckets;
    uint32_t nbuckets;
    uint32_t count;
} XrH2StreamHash;

typedef struct {
    int fd;
    void *tls_conn;
    bool is_client;

    uint32_t local_settings[7];
    uint32_t remote_settings[7];

    XrHpackTable encoder_table;
    XrHpackTable decoder_table;

    XrH2StreamHash stream_hash;
    uint32_t next_stream_id;
    int32_t connection_window;

    char *recv_buf;
    size_t recv_len;
    size_t recv_cap;
} XrH2Conn;

void http2_hpack_init(XrHpackTable *table, size_t max_size);
void http2_hpack_free(XrHpackTable *table);
int http2_hpack_encode(XrHpackTable *table, const char *name, size_t name_len, const char *value,
                       size_t value_len, uint8_t *buf, size_t buf_len);
int http2_hpack_decode(XrHpackTable *table, const uint8_t *buf, size_t buf_len,
                       void (*callback)(const char *name, size_t name_len, const char *value,
                                        size_t value_len, void *user_data),
                       void *user_data);

XrH2Conn *http2_conn_new(int fd, void *tls_conn, bool is_client);
void http2_conn_free(XrH2Conn *conn);
int http2_conn_init(XrH2Conn *conn);
XrH2Stream *http2_stream_new(XrH2Conn *conn);
int http2_send_headers(XrH2Conn *conn, XrH2Stream *stream, const char **names,
                       const size_t *name_lens, const char **values, const size_t *value_lens,
                       int count, bool end_stream);
int http2_send_data(XrH2Conn *conn, XrH2Stream *stream, const void *data, size_t len,
                    bool end_stream);
int http2_recv_stream_data(XrH2Conn *conn, XrH2Stream *stream, char **out_data, size_t *out_len);

/* ========== HTTP/2 Client ========== */

typedef struct XrH2Pool XrH2Pool;

typedef struct XrH2Request {
    const char *method;
    const char *path;
    const char *authority;
    const char *scheme;
    XrHttpHeader *headers;
    int header_count;
    const char *body;
    size_t body_len;
} XrH2Request;

typedef struct XrH2Response {
    int status;
    XrHttpHeader *headers;
    int header_count;
    char *body;
    size_t body_len;
    XrH2ErrorCode error;
    char *error_msg;
} XrH2Response;

XrH2Pool *http2_client_pool_create(void);
void http2_client_pool_destroy(XrH2Pool *pool);
XrH2Response *http2_client_request(XrH2Pool *pool, const char *url, const XrH2Request *req);
void http2_client_response_free(XrH2Response *resp);

typedef struct {
    const char *key;
    size_t key_len;
    const char *value;
    size_t value_len;
} XrRouteParam;

typedef struct {
    XrRouteParam params[XR_ROUTER_MAX_PARAMS];
    int count;
} XrRouteParams;

typedef enum {
    XR_ROUTE_NONE = 0,
    XR_ROUTE_DYNAMIC,
    XR_ROUTE_STATIC,
    XR_ROUTE_WEBSOCKET,
} XrRouteKind;

typedef struct XrRouterNode {
    char *path;
    size_t path_len;

    XrRouteKind kind;
    void *user_data;

    char *static_response;
    size_t static_response_len;

    struct XrRouterNode **children;
    int child_count;
    int child_cap;

    struct XrRouterNode *param_child;
    struct XrRouterNode *wildcard_child;

    char *param_name;
    size_t param_name_len;
} XrRouterNode;

typedef struct {
    XrRouterNode *trees[XR_HTTP_METHOD_UNKNOWN + 1];
} XrRouter;

XrRouter *http_router_new(void);
void http_router_free(XrRouter *router);
bool http_router_add(XrRouter *router, XrHttpMethod method, const char *path, void *user_data);
bool http_router_add_static(XrRouter *router, XrHttpMethod method, const char *path,
                            const char *response, size_t response_len);
XrRouteKind http_router_find(XrRouter *router, XrHttpMethod method, const char *path,
                             size_t path_len, XrRouteParams *params, void **user_data,
                             const char **static_response, size_t *static_response_len);
bool http_router_add_websocket(XrRouter *router, const char *path, void *user_data);

typedef struct XrHttpServer {
    int listen_fd;
    volatile bool running;

    XrRouter *router;

    struct XrClosure **route_closures;
    int route_closure_count;
    int route_closure_capacity;
} XrHttpServer;

// Per-Isolate HTTP module context, stored in module's native_handle.
typedef struct XrHttpContext {
    /* === Server === */
    struct XrHttpServer *server;
    XrVMRuntime *server_isolate;

    /* === Server Runtime State === */
    _Atomic int current_conns;

    /* === Connection Pools (per-isolate) === */
    XrConnPool *conn_pool;
    XrH2Pool *h2_client_pool;

} XrHttpContext;

XrHttpContext *http_get_context(XrVMRuntime *X);

XrHttpServer *http_server_new(void);
void http_server_free(XrHttpServer *server);
void http_server_route(XrHttpServer *server, XrHttpMethod method, const char *path,
                       struct XrClosure *handler);
void http_server_stop(XrHttpServer *server);

XrCFuncResult http_listen_impl(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result);

#endif
