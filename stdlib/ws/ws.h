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

#include "../../src/base/xdefs.h"

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif

struct XrVMRuntime;
struct XrModule;

/* ========== WebSocket Server API ========== */

/*
 * Upgrade HTTP connection to WebSocket and wrap as script-visible Json object.
 * Used by HTTP server to upgrade in-place when a WS route matches.
 * Returns xr_null() on failure.
 */
XrValue ws_upgrade_and_wrap(struct XrVMRuntime *X, int fd, const char *request_headers);

/* ========== Module API ========== */

// Load WebSocket module
XR_FUNC struct XrModule *xr_load_module_ws(struct XrVMRuntime *isolate);

#endif
