/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xneterror.h - Unified network error codes
 *
 * KEY CONCEPT:
 *   Single error enum shared by all network modules (net, io, tls,
 *   http, ws, cluster). Eliminates redundant per-module error enums
 *   and enables consistent error propagation across layers.
 *
 * WHY THIS DESIGN:
 *   - One enum to rule them all: no more XrIOError / XrHttpError /
 *     XrWsError / XrTlsError with overlapping semantics
 *   - Errors propagate naturally: a TLS handshake failure from tls.c
 *     can be returned directly by http_client.c without translation
 *   - xr_net_error_string() provides human-readable messages for all codes
 */

#ifndef XR_STDLIB_NETERROR_H
#define XR_STDLIB_NETERROR_H

#include "../../src/base/xdefs.h"

typedef enum {
    // Success
    XR_NERR_OK = 0,

    // Connection lifecycle
    XR_NERR_CONNECT, // Connection failed
    XR_NERR_CLOSED, // Connection closed by peer
    XR_NERR_TIMEOUT, // Operation timed out
    XR_NERR_WOULDBLOCK, // Would block (internal, retry)

    // I/O
    XR_NERR_READ, // Read failed
    XR_NERR_WRITE, // Write / send failed

    // DNS
    XR_NERR_DNS, // DNS resolution failed

    // TLS
    XR_NERR_TLS, // General TLS error
    XR_NERR_TLS_INIT, // TLS library init failed
    XR_NERR_TLS_CERT, // Certificate error
    XR_NERR_TLS_HANDSHAKE, // TLS handshake failed
    XR_NERR_TLS_VERIFY, // Certificate verification failed

    // Protocol
    XR_NERR_URL_PARSE, // URL parse failed
    XR_NERR_PARSE, // Protocol parse error (HTTP, WS)
    XR_NERR_HANDSHAKE, // Application-level handshake failed (WS)
    XR_NERR_PROTOCOL, // Protocol violation
    XR_NERR_TOO_LARGE, // Payload / response too large

    // Server
    XR_NERR_BIND, // Bind failed
    XR_NERR_LISTEN, // Listen failed
    XR_NERR_ACCEPT, // Accept failed

    // Validation
    XR_NERR_INVALID, // Invalid argument

    // Resource
    XR_NERR_MEMORY, // Memory allocation failed

    XR_NERR__COUNT // Sentinel (not an error)
} XrNetError;

// Human-readable error string (never NULL).
XR_FUNC const char* xr_net_error_string(XrNetError err);

#endif // XR_STDLIB_NETERROR_H
