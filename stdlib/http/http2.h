/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2.h - HTTP/2 protocol support
 *
 * KEY CONCEPT:
 *   HTTP/2 frame parsing/generation, HPACK header compression,
 *   multiplexed stream management.
 */

#ifndef XR_STDLIB_HTTP2_H
#define XR_STDLIB_HTTP2_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ========== HTTP/2 Constants ========== */

// Connection preface
#define XR_HTTP2_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define XR_HTTP2_PREFACE_LEN 24

// Frame types
typedef enum {
    XR_H2_FRAME_DATA          = 0x0,
    XR_H2_FRAME_HEADERS       = 0x1,
    XR_H2_FRAME_PRIORITY      = 0x2,
    XR_H2_FRAME_RST_STREAM    = 0x3,
    XR_H2_FRAME_SETTINGS      = 0x4,
    XR_H2_FRAME_PUSH_PROMISE  = 0x5,
    XR_H2_FRAME_PING          = 0x6,
    XR_H2_FRAME_GOAWAY        = 0x7,
    XR_H2_FRAME_WINDOW_UPDATE = 0x8,
    XR_H2_FRAME_CONTINUATION  = 0x9
} XrH2FrameType;

// Frame flags
#define XR_H2_FLAG_END_STREAM   0x1
#define XR_H2_FLAG_END_HEADERS  0x4
#define XR_H2_FLAG_PADDED       0x8
#define XR_H2_FLAG_PRIORITY     0x20
#define XR_H2_FLAG_ACK          0x1

// SETTINGS parameter IDs
typedef enum {
    XR_H2_SETTINGS_HEADER_TABLE_SIZE      = 0x1,
    XR_H2_SETTINGS_ENABLE_PUSH            = 0x2,
    XR_H2_SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    XR_H2_SETTINGS_INITIAL_WINDOW_SIZE    = 0x4,
    XR_H2_SETTINGS_MAX_FRAME_SIZE         = 0x5,
    XR_H2_SETTINGS_MAX_HEADER_LIST_SIZE   = 0x6
} XrH2SettingsId;

// Default values
#define XR_H2_DEFAULT_HEADER_TABLE_SIZE      4096
#define XR_H2_DEFAULT_ENABLE_PUSH            1
#define XR_H2_DEFAULT_MAX_CONCURRENT_STREAMS 100
#define XR_H2_DEFAULT_INITIAL_WINDOW_SIZE    65535
#define XR_H2_DEFAULT_MAX_FRAME_SIZE         16384
#define XR_H2_DEFAULT_MAX_HEADER_LIST_SIZE   UINT32_MAX

// Error codes
typedef enum {
    XR_H2_NO_ERROR            = 0x0,
    XR_H2_PROTOCOL_ERROR      = 0x1,
    XR_H2_INTERNAL_ERROR      = 0x2,
    XR_H2_FLOW_CONTROL_ERROR  = 0x3,
    XR_H2_SETTINGS_TIMEOUT    = 0x4,
    XR_H2_STREAM_CLOSED       = 0x5,
    XR_H2_FRAME_SIZE_ERROR    = 0x6,
    XR_H2_REFUSED_STREAM      = 0x7,
    XR_H2_CANCEL              = 0x8,
    XR_H2_COMPRESSION_ERROR   = 0x9,
    XR_H2_CONNECT_ERROR       = 0xa,
    XR_H2_ENHANCE_YOUR_CALM   = 0xb,
    XR_H2_INADEQUATE_SECURITY = 0xc,
    XR_H2_HTTP_1_1_REQUIRED   = 0xd
} XrH2ErrorCode;

/* ========== Frame Header ========== */

typedef struct {
    uint32_t length;
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id;
} XrH2FrameHeader;

#define XR_H2_FRAME_HEADER_SIZE 9

/* ========== HPACK Dynamic Table ========== */

typedef struct XrHpackEntry {
    char *name;
    size_t name_len;
    char *value;
    size_t value_len;
    struct XrHpackEntry *next;
} XrHpackEntry;

typedef struct {
    XrHpackEntry *entries;
    size_t size;
    size_t max_size;
    int count;
} XrHpackTable;

/* ========== HTTP/2 Stream ========== */

typedef enum {
    XR_H2_STREAM_IDLE,
    XR_H2_STREAM_OPEN,
    XR_H2_STREAM_HALF_CLOSED_LOCAL,
    XR_H2_STREAM_HALF_CLOSED_REMOTE,
    XR_H2_STREAM_STATE_CLOSED
} XrH2StreamState;

typedef struct XrH2Priority {
    uint32_t dependency;
    uint8_t weight;
    bool exclusive;
} XrH2Priority;

typedef struct XrH2Stream {
    uint32_t id;
    XrH2StreamState state;
    int32_t window_size;
    int status;  // HTTP status code from :status pseudo-header (0 = not set)
    
    XrH2Priority priority;
    
    // Request/response data
    char *headers_buf;
    size_t headers_len;
    char *data_buf;
    size_t data_len;
    size_t data_cap;
    
    char *trailers_buf;
    size_t trailers_len;
    
    bool cancelled;
    
    struct XrH2Stream *next;
} XrH2Stream;

/* ========== HTTP/2 Stream Hash Table ========== */

#define XR_H2_STREAM_HASH_SIZE 64

typedef struct {
    XrH2Stream *buckets[XR_H2_STREAM_HASH_SIZE];
    uint32_t count;
} XrH2StreamHash;

/* ========== HTTP/2 Connection ========== */

typedef struct {
    int fd;
    void *tls_conn;
    bool is_client;
    
    // Local/remote settings
    uint32_t local_settings[7];
    uint32_t remote_settings[7];
    
    // HPACK encoder/decoder
    XrHpackTable encoder_table;
    XrHpackTable decoder_table;
    
    // Stream management
    XrH2StreamHash stream_hash;
    uint32_t next_stream_id;
    int32_t connection_window;
    
    // Receive buffer
    char *recv_buf;
    size_t recv_len;
    size_t recv_cap;
    
    // State
    bool settings_sent;
    bool settings_acked;
    bool goaway_sent;
    bool goaway_received;
    uint32_t last_stream_id;
} XrH2Conn;

/* ========== HPACK API ========== */

void xr_hpack_init(XrHpackTable *table, size_t max_size);
void xr_hpack_free(XrHpackTable *table);

// HPACK encode header, returns encoded length or -1
int xr_hpack_encode(XrHpackTable *table, 
                    const char *name, size_t name_len,
                    const char *value, size_t value_len,
                    uint8_t *buf, size_t buf_len);

// HPACK decode header block
int xr_hpack_decode(XrHpackTable *table,
                    const uint8_t *buf, size_t buf_len,
                    void (*callback)(const char *name, size_t name_len,
                                    const char *value, size_t value_len,
                                    void *user_data),
                    void *user_data);

/* ========== HTTP/2 Connection API ========== */

struct XrHttpHeader;

XrH2Conn* xr_h2_conn_new(int fd, void *tls_conn, bool is_client);
XrH2Conn* xr_h2_conn_new_client(int fd, void *tls_conn);
void xr_h2_conn_free(XrH2Conn *conn);
int xr_h2_conn_init(XrH2Conn *conn);
XrH2Stream* xr_h2_stream_new(XrH2Conn *conn);

/* ========== Stream Hash Table API ========== */

void xr_h2_stream_hash_init(XrH2StreamHash *hash);
void xr_h2_stream_hash_add(XrH2StreamHash *hash, XrH2Stream *stream);
XrH2Stream* xr_h2_stream_hash_find(XrH2StreamHash *hash, uint32_t stream_id);
void xr_h2_stream_hash_remove(XrH2StreamHash *hash, uint32_t stream_id);
void xr_h2_stream_hash_free(XrH2StreamHash *hash);

// Send HEADERS frame
int xr_h2_send_headers(XrH2Conn *conn, XrH2Stream *stream,
                       const char **names, const size_t *name_lens,
                       const char **values, const size_t *value_lens,
                       int count, bool end_stream);

// Send DATA frame
int xr_h2_send_data(XrH2Conn *conn, XrH2Stream *stream,
                    const void *data, size_t len, bool end_stream);

// Receive stream data (blocking)
int xr_h2_recv_stream_data(XrH2Conn *conn, XrH2Stream *stream,
                            char **out_data, size_t *out_len);

// Receive and process frames
int xr_h2_recv(XrH2Conn *conn);

// Get stream response
XrH2Stream* xr_h2_get_stream(XrH2Conn *conn, uint32_t stream_id);

/* ========== Frame Parsing/Generation ========== */

int xr_h2_parse_frame_header(const uint8_t *buf, XrH2FrameHeader *header);
void xr_h2_write_frame_header(uint8_t *buf, const XrH2FrameHeader *header);
int xr_h2_send_settings(XrH2Conn *conn);
int xr_h2_send_settings_ack(XrH2Conn *conn);
int xr_h2_send_goaway(XrH2Conn *conn, uint32_t last_stream_id, XrH2ErrorCode error);
int xr_h2_send_window_update(XrH2Conn *conn, uint32_t stream_id, uint32_t increment);

/* ========== Stream Priority API ========== */

int xr_h2_set_priority(XrH2Conn *conn, XrH2Stream *stream, const XrH2Priority *priority);
int xr_h2_send_priority(XrH2Conn *conn, uint32_t stream_id, const XrH2Priority *priority);

/* ========== Stream Cancel API ========== */

int xr_h2_cancel_stream(XrH2Conn *conn, XrH2Stream *stream, XrH2ErrorCode error);
int xr_h2_send_rst_stream(XrH2Conn *conn, uint32_t stream_id, XrH2ErrorCode error);

/* ========== Trailers API ========== */

// Send Trailers (HEADERS after DATA)
int xr_h2_send_trailers(XrH2Conn *conn, XrH2Stream *stream,
                         const char **names, const size_t *name_lens,
                         const char **values, const size_t *value_lens,
                         int count);

/* ========== PING API ========== */

int xr_h2_send_ping(XrH2Conn *conn, const uint8_t data[8], bool ack);

/* ========== h2c (HTTP/2 Cleartext) ========== */

// Upgrade to HTTP/2 via HTTP/1.1 Upgrade
int xr_h2_upgrade_from_http1(XrH2Conn *conn, const char *settings_payload, size_t len);

// Start h2c connection directly (Prior Knowledge)
int xr_h2_start_h2c(XrH2Conn *conn);

#endif // XR_STDLIB_HTTP2_H
