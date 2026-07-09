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

#endif  // XR_STDLIB_WS_INTERNAL_H
