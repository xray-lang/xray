/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * ws.h - WebSocket standard library module loader
 *
 * KEY CONCEPT:
 *   ws's public API is implemented in stdlib/ws/ws.xr. This header only
 *   exposes the native loader that anchors `import ws` in the stdlib registry.
 */

#ifndef XR_STDLIB_WS_H
#define XR_STDLIB_WS_H

#include "../../src/base/xdefs.h"

struct XrVMRuntime;
struct XrModule;


#endif  // XR_STDLIB_WS_H
