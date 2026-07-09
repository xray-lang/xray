/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws_internal.h - Private helpers shared by the ws native binding units
 */

#ifndef XR_STDLIB_WS_INTERNAL_H
#define XR_STDLIB_WS_INTERNAL_H

#include "ws.h"

// Validate WebSocket URL syntax without touching DNS or sockets.
XrWsError ws_url_validate(const char *url);

// Initialize configuration with defaults.
void ws_config_init(XrWsConfig *config);

// Create/free native WebSocket connections.
XrWebSocket *ws_new(const XrWsConfig *config);
void ws_free(XrWebSocket *ws);

/*
 * Bind the WebSocket to a XrVMRuntime for coroutine-aware I/O.
 * MUST be called before ws_connect_start / xr_ws_send_frame_try / xr_ws_recv_try.
 * Server-side connections are bound automatically by the upgrade path.
 * Passing NULL is a programming error; all WS I/O requires an isolate.
 */
void ws_set_isolate(XrWebSocket *ws, struct XrVMRuntime *X);

// Yieldable client connect, split into a non-blocking phase machine so the
// connect cfunc can suspend the coroutine instead of blocking a worker thread.
//   ws_connect_start: DNS + socket + non-blocking connect(). Returns a poll
//     event to wait for (XR_WAIT_WRITE) on success, or a negative -XrWsError.
//   ws_connect_pump:  advance the handshake. Returns 0 when the connection is
//     OPEN, a negative -XrWsError on failure, or a poll event (XR_WAIT_READ /
//     XR_WAIT_WRITE) to wait for before calling pump again.
int ws_connect_start(XrWebSocket *ws);
int ws_connect_pump(XrWebSocket *ws);

// Get error description for native WS results.
const char *ws_error_string(XrWsError err);

/*
 * Optional policy knobs for ws_upgrade_ex.
 *
 * allowed_origins:
 *   NULL-terminated array of Origin strings. When set, the upgrade is
 *   rejected with HTTP 403 unless the client's Origin header exactly
 *   matches one of the entries. NULL intentionally allows any Origin.
 *   A single "*" entry matches any non-empty origin.
 *
 * server_protocols:
 *   NULL-terminated, ordered list of subprotocols the server supports.
 *   When set, ws_upgrade_ex walks the client's Sec-WebSocket-Protocol
 *   offer and picks the first name shared with this list. That name is
 *   echoed back in the 101 response and stored in `ws->protocol`. If no
 *   overlap exists, the upgrade completes without a subprotocol (same
 *   behaviour as today). Leaving this NULL disables negotiation.
 *
 * All string arrays are borrowed; the caller must keep them alive for
 * the duration of the ws_upgrade_ex call.
 */
typedef struct XrWsUpgradeOptions {
    const char **allowed_origins;
    const char **server_protocols;
} XrWsUpgradeOptions;

/*
 * Upgrade from HTTP request to WebSocket (server side), with optional Origin
 * policy and subprotocol picker. Passing `opts == NULL` applies no policy.
 *
 * On Origin rejection this sends an HTTP 403 response and returns NULL;
 * on any other failure it also returns NULL but no response is sent
 * (the socket is left untouched for the caller to close).
 */
XrWebSocket *ws_upgrade_ex(struct XrVMRuntime *isolate, int fd, const char *request_headers,
                           const XrWsUpgradeOptions *opts);

// Check if HTTP request is a WebSocket upgrade request.
bool ws_is_upgrade_request(const char *request_headers);

#endif  // XR_STDLIB_WS_INTERNAL_H
