/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws.h - WebSocket module public interface
 *
 * KEY CONCEPT:
 *   Native WebSocket connection I/O with pure-Xray protocol helpers layered
 *   above it for public handshake/frame control-plane APIs.
 *   Implements RFC 6455 WebSocket protocol.
 */

#ifndef XR_STDLIB_WS_H
#define XR_STDLIB_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../src/base/xdefs.h"
#include "../net/xneterror.h"

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif

/* ========== WebSocket Opcodes ========== */

typedef enum {
    WS_OPCODE_CONTINUATION = 0x0,  // Continuation frame
    WS_OPCODE_TEXT = 0x1,          // Text frame
    WS_OPCODE_BINARY = 0x2,        // Binary frame
    WS_OPCODE_CLOSE = 0x8,         // Close frame
    WS_OPCODE_PING = 0x9,          // Ping frame
    WS_OPCODE_PONG = 0xA           // Pong frame
} XrWsOpcode;

/* ========== WebSocket States ========== */

typedef enum {
    WS_STATE_CONNECTING,  // Connecting
    WS_STATE_OPEN,        // Connected
    WS_STATE_CLOSING,     // Closing
    WS_STATE_CLOSED       // Closed
} XrWsState;

/* ========== I/O Buffer ========== */

#define WS_RBUF_INIT_CAP (65536 + 64)  // 64KB + headroom for max frame header (14B)

/* ========== WebSocket Close Codes ========== */

typedef enum {
    WS_CLOSE_NORMAL = 1000,          // Normal closure
    WS_CLOSE_GOING_AWAY = 1001,      // Endpoint going away
    WS_CLOSE_PROTOCOL_ERROR = 1002,  // Protocol error
    WS_CLOSE_UNSUPPORTED = 1003,     // Unsupported data type
    WS_CLOSE_NO_STATUS = 1005,       // No status code
    WS_CLOSE_ABNORMAL = 1006,        // Abnormal closure
    WS_CLOSE_INVALID_DATA = 1007,    // Invalid data
    WS_CLOSE_POLICY = 1008,          // Policy violation
    WS_CLOSE_TOO_LARGE = 1009,       // Message too large
    WS_CLOSE_EXTENSION = 1010,       // Extension negotiation failed
    WS_CLOSE_SERVER_ERROR = 1011     // Server error
} XrWsCloseCode;

// WebSocket error codes — aliases into unified XrNetError
typedef XrNetError XrWsError;
#define WS_OK XR_NERR_OK
#define WS_ERR_URL XR_NERR_URL_PARSE
#define WS_ERR_DNS XR_NERR_DNS
#define WS_ERR_CONNECT XR_NERR_CONNECT
#define WS_ERR_HANDSHAKE XR_NERR_HANDSHAKE
#define WS_ERR_SEND XR_NERR_WRITE
#define WS_ERR_RECV XR_NERR_READ
#define WS_ERR_TIMEOUT XR_NERR_TIMEOUT
#define WS_ERR_CLOSED XR_NERR_CLOSED
#define WS_ERR_PROTOCOL XR_NERR_PROTOCOL
#define WS_ERR_MEMORY XR_NERR_MEMORY

/* ========== WebSocket Message ========== */

/*
 * Internal ownership flags — packed into a single byte so the public
 * message struct does not expose individual booleans that callers might
 * accidentally flip. Values are powers of two for bitwise testing.
 */
#define XR_WS_MSG_NO_FREE 0x01      /* struct is embedded, do not xr_free it */
#define XR_WS_MSG_DATA_INPLACE 0x02 /* data points into rbuf, do not xr_free data */

typedef struct XrWsMessage {
    XrWsOpcode opcode;  // Opcode
    char *data;         // Data (caller reads; freed by ws_message_free)
    size_t len;         // Length
    bool is_text;       // Is text message
    uint8_t _flags;     // Reserved — do not touch (internal ownership bits)
} XrWsMessage;

struct XrWebSocket;

/* ========== WebSocket Configuration ========== */

typedef struct XrWsConfig {
    const char *url;            // WebSocket URL (ws:// or wss://)
    const char **subprotocols;  // Subprotocol list
    int subprotocol_count;      // Subprotocol count
    const char **headers;       // Custom headers (key-value pairs)
    int header_count;           // Header count (number of key-value pairs)
    int connect_timeout_ms;     // Connection timeout (milliseconds)
    int ping_interval_ms;       // Ping interval (milliseconds, 0 to disable)
    int pong_timeout_ms;        // Pong timeout (milliseconds)
    size_t max_message_size;    // Maximum message size
} XrWsConfig;

/* ========== WebSocket Connection ========== */

// Forward declaration
struct XrVMRuntime;

typedef struct XrWebSocket {
    // Connection state
    XrWsState state;
    int fd;                       // Socket file descriptor
    bool is_server;               // true if server-side connection (no masking on send)
    struct XrVMRuntime *isolate;  // Isolate for coroutine-aware I/O

    // URL info
    char *host;
    int port;
    char *path;
    bool is_secure;  // wss://

    // Protocol
    char *protocol;  // Negotiated subprotocol
    char *sec_key;   // Sec-WebSocket-Key

    // Configuration
    XrWsConfig config;

    // Flat read buffer (inline, no indirection)
    char *rbuf;    // heap-allocated read buffer
    int rbuf_off;  // consumed offset (data at rbuf+rbuf_off)
    int rbuf_len;  // valid data bytes from rbuf_off
    int rbuf_cap;  // allocated capacity

    // Message buffer (dynamic, allocated per-frame based on payload size)
    char *msg_buf;         // dynamically allocated for large payloads
    size_t msg_buf_size;   // allocated capacity
    size_t msg_buf_len;    // bytes filled
    size_t msg_remaining;  // remaining bytes to read for current frame

    // Current frame state (for multi-read frames)
    bool frame_in_progress;       // true if reading a frame payload
    bool frame_fin;               // FIN bit of current frame
    XrWsOpcode frame_opcode;      // opcode of current frame
    bool frame_masked;            // masked flag
    bool frame_rsv1;              // RSV1 bit (permessage-deflate compressed flag)
    unsigned char frame_mask[4];  // mask key

    // Fragment message buffer (for fragmented messages across frames)
    char *frag_buf;
    size_t frag_buf_size;
    size_t frag_buf_len;
    XrWsOpcode frag_opcode;

    // TLS (if enabled)
    void *tls_conn;
    void *tls_ctx;

    // Yieldable send: a fully-built frame (header + masked payload) is staged
    // in send_frame_buf and drained non-blockingly across coroutine yields.
    // A stable buffer is required because (a) a client frame's mask cannot be
    // re-derived on retry and (b) TLS SSL_write must retry the same bytes.
    // The buffer is reused across sends (grown on demand, freed on close).
    char *send_frame_buf;     // staged frame bytes (owned)
    size_t send_frame_cap;    // allocated capacity
    size_t send_frame_len;    // staged frame length
    size_t send_frame_off;    // bytes already drained
    bool send_frame_pending;  // a staged send is mid-drain (resume continues it)
    int send_wait_event;      // XR_WAIT_WRITE, or XR_WAIT_READ for a TLS WANT_READ
    int recv_wait_event;      // XR_WAIT_READ, or XR_WAIT_WRITE for a TLS WANT_WRITE

    // Yieldable client handshake progress. ws_connect_start sets up
    // DNS+socket+connect; ws_connect_pump advances a CONNECT->TLS->SEND->RECV
    // phase machine that never blocks the worker: it returns 0 (open), <0
    // (-XrWsError) or a poll event (XR_WAIT_READ/WRITE) to wait for before the
    // next pump. The handshake request is built once and drained, then the
    // response is read, both across coroutine yields, so the connect cfunc holds
    // no OS thread (no P handoff, no shared timer-wheel contention).
    int connect_phase;       // WsConnectPhase
    char *connect_buf;       // handshake request (SEND) then response (RECV); owned
    size_t connect_buf_cap;  // allocated capacity of connect_buf
    size_t connect_len;      // request bytes to send / response bytes received
    size_t connect_off;      // request bytes already sent

    // Embedded message (avoids calloc per recv)
    XrWsMessage last_msg;

    // Ping/Pong auto-management (monotonic ms)
    uint64_t last_ping_sent_ms;  // 0 = no ping in flight
    uint64_t last_pong_recv_ms;  // Last pong received
    bool ping_in_flight;         // true if waiting for pong

    // permessage-deflate (RFC 7692)
    bool deflate_enabled;     // Extension negotiated successfully
    bool deflate_no_context;  // no_context_takeover (per-message independent)

    // Cached PollDesc (avoids fdmap lookup on every yield)
    void *cached_pd;

    // Close info
    int close_code;
    char *close_reason;
} XrWebSocket;

/* ========== Core API ========== */

// Close connection
XR_FUNC XrWsError xr_ws_close(XrWebSocket *ws, int code, const char *reason);

// Send Ping
XR_FUNC XrWsError xr_ws_ping(XrWebSocket *ws);

// Receive message (non-blocking, for yieldable integration)
// Returns: received message, or NULL if:
//   - error/close: ws->state != WS_STATE_OPEN
//   - need more data: *need_more = true
// This function never blocks - returns immediately
XR_FUNC XrWsMessage *xr_ws_recv_try(XrWebSocket *ws, bool *need_more);

// Send frame (non-blocking, for yieldable integration)
// Returns: 0 = complete, -1 = error, -2 = would block (need to wait for write)
XR_FUNC int xr_ws_send_frame_try(XrWebSocket *ws, XrWsOpcode opcode, const void *data, size_t len);

// Free message
XR_FUNC void xr_ws_message_free(XrWsMessage *msg);

/* ========== WebSocket Server API ========== */

/*
 * Upgrade HTTP connection to WebSocket and wrap as script-visible Json object.
 * Used by HTTP server to upgrade in-place when a WS route matches.
 * Returns xr_null() on failure.
 */
XR_FUNC XrValue xr_ws_upgrade_and_wrap(struct XrVMRuntime *X, int fd, const char *request_headers);

/* ========== Module API ========== */

struct XrVMRuntime;
struct XrModule;

// Load WebSocket module
XR_FUNC struct XrModule *xr_load_module_ws(struct XrVMRuntime *isolate);

#endif
