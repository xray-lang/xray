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
 *   Pure C WebSocket module with stackful coroutine server model.
 *   Client and server both implemented entirely in C, no script layer.
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
    WS_OPCODE_CONTINUATION = 0x0,   // Continuation frame
    WS_OPCODE_TEXT         = 0x1,   // Text frame
    WS_OPCODE_BINARY       = 0x2,   // Binary frame
    WS_OPCODE_CLOSE        = 0x8,   // Close frame
    WS_OPCODE_PING         = 0x9,   // Ping frame
    WS_OPCODE_PONG         = 0xA    // Pong frame
} XrWsOpcode;

/* ========== WebSocket States ========== */

typedef enum {
    WS_STATE_CONNECTING,    // Connecting
    WS_STATE_OPEN,          // Connected
    WS_STATE_CLOSING,       // Closing
    WS_STATE_CLOSED         // Closed
} XrWsState;

/* ========== I/O Buffer ========== */

#define WS_RBUF_INIT_CAP (65536 + 64)  // 64KB + headroom for max frame header (14B)

/* ========== WebSocket Close Codes ========== */

typedef enum {
    WS_CLOSE_NORMAL         = 1000,  // Normal closure
    WS_CLOSE_GOING_AWAY     = 1001,  // Endpoint going away
    WS_CLOSE_PROTOCOL_ERROR = 1002,  // Protocol error
    WS_CLOSE_UNSUPPORTED    = 1003,  // Unsupported data type
    WS_CLOSE_NO_STATUS      = 1005,  // No status code
    WS_CLOSE_ABNORMAL       = 1006,  // Abnormal closure
    WS_CLOSE_INVALID_DATA   = 1007,  // Invalid data
    WS_CLOSE_POLICY         = 1008,  // Policy violation
    WS_CLOSE_TOO_LARGE      = 1009,  // Message too large
    WS_CLOSE_EXTENSION      = 1010,  // Extension negotiation failed
    WS_CLOSE_SERVER_ERROR   = 1011   // Server error
} XrWsCloseCode;

// WebSocket error codes — aliases into unified XrNetError
typedef XrNetError XrWsError;
#define WS_OK               XR_NERR_OK
#define WS_ERR_URL           XR_NERR_URL_PARSE
#define WS_ERR_DNS           XR_NERR_DNS
#define WS_ERR_CONNECT       XR_NERR_CONNECT
#define WS_ERR_HANDSHAKE     XR_NERR_HANDSHAKE
#define WS_ERR_SEND          XR_NERR_WRITE
#define WS_ERR_RECV          XR_NERR_READ
#define WS_ERR_TIMEOUT       XR_NERR_TIMEOUT
#define WS_ERR_CLOSED        XR_NERR_CLOSED
#define WS_ERR_PROTOCOL      XR_NERR_PROTOCOL
#define WS_ERR_MEMORY        XR_NERR_MEMORY

/* ========== WebSocket Message ========== */

typedef struct XrWsMessage {
    XrWsOpcode opcode;      // Opcode
    char *data;             // Data
    size_t len;             // Length
    bool is_text;           // Is text message
    bool _no_free;          // If true, struct is embedded (don't free it)
    bool _data_inplace;     // If true, data points into rbuf (don't free data)
} XrWsMessage;

struct XrWebSocket;

/* ========== WebSocket Configuration ========== */

typedef struct XrWsConfig {
    const char *url;                // WebSocket URL (ws:// or wss://)
    const char **subprotocols;      // Subprotocol list
    int subprotocol_count;          // Subprotocol count
    const char **headers;           // Custom headers (key-value pairs)
    int header_count;               // Header count (number of key-value pairs)
    int connect_timeout_ms;         // Connection timeout (milliseconds)
    int ping_interval_ms;           // Ping interval (milliseconds, 0 to disable)
    int pong_timeout_ms;            // Pong timeout (milliseconds)
    size_t max_message_size;        // Maximum message size
} XrWsConfig;

/* ========== WebSocket Connection ========== */

// Forward declaration
struct XrayIsolate;

typedef struct XrWebSocket {
    // Connection state
    XrWsState state;
    int fd;                         // Socket file descriptor
    bool is_server;                 // true if server-side connection (no masking on send)
    struct XrayIsolate *isolate;    // Isolate for coroutine-aware I/O

    // URL info
    char *host;
    int port;
    char *path;
    bool is_secure;                 // wss://

    // Protocol
    char *protocol;                 // Negotiated subprotocol
    char *sec_key;                  // Sec-WebSocket-Key

    // Configuration
    XrWsConfig config;

    // Flat read buffer (inline, no indirection)
    char *rbuf;                   // heap-allocated read buffer
    int rbuf_off;                 // consumed offset (data at rbuf+rbuf_off)
    int rbuf_len;                 // valid data bytes from rbuf_off
    int rbuf_cap;                 // allocated capacity

    // Message buffer (dynamic, allocated per-frame based on payload size)
    char *msg_buf;                // dynamically allocated for large payloads
    size_t msg_buf_size;          // allocated capacity
    size_t msg_buf_len;           // bytes filled
    size_t msg_remaining;         // remaining bytes to read for current frame

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

    // Partial send tracking (for non-blocking writev resume)
    size_t send_offset;             // offset into payload for partial send resume
    bool send_header_sent;          // true if frame header already sent

    // Cork write buffer (batch multiple frames into single send)
    char *wbuf;                     // heap-allocated write buffer
    int wbuf_len;                   // valid data bytes
    int wbuf_cap;                   // allocated capacity
    bool corked;                    // true if corked (writes go to wbuf)

    // Embedded message (avoids calloc per recv)
    XrWsMessage last_msg;

    // Ping/Pong auto-management (monotonic ms)
    uint64_t last_ping_sent_ms;     // 0 = no ping in flight
    uint64_t last_pong_recv_ms;     // Last pong received
    bool ping_in_flight;            // true if waiting for pong

    // permessage-deflate (RFC 7692)
    bool deflate_enabled;           // Extension negotiated successfully
    bool deflate_no_context;        // no_context_takeover (per-message independent)

    // Cached PollDesc (avoids fdmap lookup on every yield)
    void *cached_pd;

    // Close info
    int close_code;
    char *close_reason;
} XrWebSocket;

/* ========== Core API ========== */

// Initialize configuration with defaults
XR_FUNC void xr_ws_config_init(XrWsConfig *config);

// Create WebSocket connection
XR_FUNC XrWebSocket* xr_ws_new(const XrWsConfig *config);

// Free WebSocket
XR_FUNC void xr_ws_free(XrWebSocket *ws);

/*
 * Bind the WebSocket to a XrayIsolate for coroutine-aware I/O.
 * When set, blocking send/recv paths yield via netpoll instead of
 * falling back to poll(5s). Callers (typically ws_binding) must invoke
 * this right after xr_ws_new so the subsequent xr_ws_connect / xr_ws_send
 * paths can cooperate with the scheduler.
 *
 * Passing NULL restores the legacy blocking fallback behaviour.
 * Server-side connections are bound automatically in xr_ws_upgrade.
 */
XR_FUNC void xr_ws_set_isolate(XrWebSocket *ws, struct XrayIsolate *X);

// Connect to server
XR_FUNC XrWsError xr_ws_connect(XrWebSocket *ws);

// Close connection
XR_FUNC XrWsError xr_ws_close(XrWebSocket *ws, int code, const char *reason);

// Send text message
XR_FUNC XrWsError xr_ws_send_text(XrWebSocket *ws, const char *text, size_t len);

// Send binary message
XR_FUNC XrWsError xr_ws_send_binary(XrWebSocket *ws, const void *data, size_t len);

// Send Ping
XR_FUNC XrWsError xr_ws_ping(XrWebSocket *ws);

// Send Pong
XR_FUNC XrWsError xr_ws_pong(XrWebSocket *ws, const void *data, size_t len);

// Receive message (blocking)
// Returns: received message (caller must free), NULL on error or close
XR_FUNC XrWsMessage* xr_ws_recv(XrWebSocket *ws);

// Receive message (non-blocking, for yieldable integration)
// Returns: received message, or NULL if:
//   - error/close: ws->state != WS_STATE_OPEN
//   - need more data: *need_more = true
// This function never blocks - returns immediately
XR_FUNC XrWsMessage* xr_ws_recv_try(XrWebSocket *ws, bool *need_more);

// Send frame (non-blocking, for yieldable integration)
// Returns: 0 = complete, -1 = error, -2 = would block (need to wait for write)
XR_FUNC int xr_ws_send_frame_try(XrWebSocket *ws, XrWsOpcode opcode,
                          const void *data, size_t len);

// Cork: buffer subsequent send_frame_try calls into wbuf (no syscall)
XR_FUNC void xr_ws_cork(XrWebSocket *ws);

// Uncork: flush wbuf with single send, return to direct-send mode
// Returns: 0 = complete, -1 = error, -2 = would block
XR_FUNC int xr_ws_uncork(XrWebSocket *ws);

// Poll events (non-blocking)
// timeout_ms: timeout in milliseconds, 0 returns immediately, -1 waits forever
// Returns: 0 no event, 1 has event, -1 error
XR_FUNC int xr_ws_poll(XrWebSocket *ws, int timeout_ms);

// Free message
XR_FUNC void xr_ws_message_free(XrWsMessage *msg);

// Recycle message: return heap-allocated data back to ws->msg_buf for reuse
// Avoids malloc/free per frame in echo-style hot loops
XR_FUNC void xr_ws_message_recycle(XrWebSocket *ws, XrWsMessage *msg);

// Get state
XR_FUNC XrWsState xr_ws_get_state(XrWebSocket *ws);

// Get error description
XR_FUNC const char* xr_ws_error_string(XrWsError err);

/* ========== WebSocket Server API ========== */

// Upgrade from HTTP request to WebSocket (server side)
// fd: client socket
// request_headers: HTTP request headers (containing Upgrade, Sec-WebSocket-Key, etc.)
// isolate: XrayIsolate for coroutine-aware I/O (can be NULL for non-coroutine use)
// Returns: upgraded WebSocket connection, NULL on failure
XR_FUNC XrWebSocket* xr_ws_upgrade(struct XrayIsolate *isolate, int fd, const char *request_headers);

/*
 * Optional policy knobs for xr_ws_upgrade_ex.
 *
 * allowed_origins:
 *   NULL-terminated array of Origin strings. When set, the upgrade is
 *   rejected with HTTP 403 unless the client's Origin header exactly
 *   matches one of the entries. NULL disables the check (legacy
 *   behaviour). A single "*" entry matches any non-empty origin.
 *
 * server_protocols:
 *   NULL-terminated, ordered list of subprotocols the server supports.
 *   When set, xr_ws_upgrade_ex walks the client's Sec-WebSocket-Protocol
 *   offer and picks the first name shared with this list. That name is
 *   echoed back in the 101 response and stored in `ws->protocol`. If no
 *   overlap exists, the upgrade completes without a subprotocol (same
 *   behaviour as today). Leaving this NULL disables negotiation.
 *
 * All string arrays are borrowed — the caller must keep them alive for
 * the duration of the xr_ws_upgrade_ex call.
 */
typedef struct XrWsUpgradeOptions {
    const char **allowed_origins;
    const char **server_protocols;
} XrWsUpgradeOptions;

/*
 * Extended upgrade with Origin policy and subprotocol picker.
 * See XrWsUpgradeOptions. Passing `opts == NULL` is equivalent to
 * xr_ws_upgrade (no policy).
 *
 * On Origin rejection this sends an HTTP 403 response and returns NULL;
 * on any other failure it also returns NULL but no response is sent
 * (the socket is left untouched for the caller to close).
 */
XR_FUNC XrWebSocket* xr_ws_upgrade_ex(struct XrayIsolate *isolate, int fd,
                                       const char *request_headers,
                                       const XrWsUpgradeOptions *opts);

// Check if HTTP request is a WebSocket upgrade request
// Returns: true if it is a WebSocket upgrade request
XR_FUNC bool xr_ws_is_upgrade_request(const char *request_headers);

// Get Sec-WebSocket-Key (extract from request headers)
// Returns: key string (caller must free), NULL if not found
XR_FUNC char* xr_ws_get_sec_key(const char *request_headers);

/*
 * Pick the first subprotocol the client offered that the server also
 * supports. `server_protocols` is a NULL-terminated array ordered by
 * server preference; the client offer is parsed from the request's
 * Sec-WebSocket-Protocol header as a comma-separated list.
 *
 * Returns an xr_strdup'd name on match (caller frees) or NULL if no
 * overlap exists or the client sent no offer.
 */
XR_FUNC char* xr_ws_pick_subprotocol(const char *request_headers,
                                      const char **server_protocols);

// Send WebSocket upgrade response
// Returns: 0 on success, -1 on failure
XR_FUNC int xr_ws_send_upgrade_response(int fd, const char *sec_key,
                                const char *protocol, bool deflate_ok);

/*
 * Upgrade HTTP connection to WebSocket and wrap as script-visible Json object.
 * Used by HTTP server to upgrade in-place when a WS route matches.
 * Returns xr_null() on failure.
 */
XR_FUNC XrValue xr_ws_upgrade_and_wrap(struct XrayIsolate *X, int fd,
                                const char *request_headers);

/* ========== Module API ========== */

struct XrayIsolate;
struct XrModule;

// Load WebSocket module
XR_FUNC struct XrModule* xr_load_module_ws(struct XrayIsolate *isolate);

#endif
