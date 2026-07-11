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
#include "http_client_internal.h"
#include "http_parser_internal.h"
#include "http_conn_pool.h"
#include "../../src/runtime/xisolate_internal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

    char *data_buf;
    size_t data_len;

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
    XrVMRuntime *isolate;
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
} XrH2Conn;

void http2_hpack_init(XrHpackTable *table, size_t max_size);
void http2_hpack_free(XrHpackTable *table);
int http2_hpack_encode(XrHpackTable *table, const char *name, size_t name_len, const char *value,
                       size_t value_len, uint8_t *buf, size_t buf_len);
int http2_hpack_decode(XrHpackTable *table, const uint8_t *buf, size_t buf_len,
                       void (*callback)(const char *name, size_t name_len, const char *value,
                                        size_t value_len, void *user_data),
                       void *user_data);

XrH2Conn *http2_conn_new(XrVMRuntime *X, int fd, void *tls_conn, bool is_client);
void http2_conn_free(XrH2Conn *conn);
int http2_conn_init(XrH2Conn *conn);
XrH2ErrorCode http2_validate_inbound_frame_header(const XrH2FrameHeader *header);
XrH2ErrorCode http2_apply_settings_payload(XrH2Conn *conn, const uint8_t *payload, uint32_t length);
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
    size_t method_len;
    XrHttpHeader *headers;
    int header_count;
    const char *body;
    size_t body_len;
} XrH2Request;

typedef struct XrH2Response {
    int status;
    char *body;
    size_t body_len;
} XrH2Response;

XrH2Pool *http2_client_pool_create(void);
void http2_client_pool_destroy(XrH2Pool *pool);
XrH2Response *http2_client_request(XrVMRuntime *X, XrH2Pool *pool, const char *url,
                                   const XrH2Request *req);
void http2_client_response_free(XrH2Response *resp);

// Per-Isolate HTTP module context, stored in module's native_handle.
typedef struct XrHttpContext {
    /* === Connection Pools (per-isolate) === */
    XrHttpConnPool *http_conn_pool;
    XrH2Pool *h2_client_pool;

} XrHttpContext;

XrHttpContext *http_get_context(XrVMRuntime *X);

#endif
