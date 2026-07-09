/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws_http_bridge.h - Private HTTP/WS native integration point
 */

#ifndef XR_STDLIB_WS_HTTP_BRIDGE_H
#define XR_STDLIB_WS_HTTP_BRIDGE_H

#include "../../src/base/xdefs.h"

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif

struct XrVMRuntime;

/*
 * Upgrade an already accepted HTTP connection to WebSocket and wrap it as the
 * script-visible connection Json used by ws.send/ws.recv.
 */
XrValue ws_upgrade_and_wrap(struct XrVMRuntime *X, int fd, const char *request_headers);

#endif
