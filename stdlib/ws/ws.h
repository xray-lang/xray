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

struct XrVMRuntime;
struct XrModule;

/* ========== Module API ========== */

// Load WebSocket module
XR_FUNC struct XrModule *xr_load_module_ws(struct XrVMRuntime *isolate);

#endif
