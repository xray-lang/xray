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
