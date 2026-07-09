/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http.h - HTTP module public interface
 *
 * KEY CONCEPT:
 *   Public C entrypoint for registering the HTTP module. Native HTTP
 *   client/server internals live in http_internal.h and the narrower
 *   subsystem headers.
 */

#ifndef XR_STDLIB_HTTP_H
#define XR_STDLIB_HTTP_H

#include "../../src/base/xdefs.h"

struct XrVMRuntime;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_http(struct XrVMRuntime *isolate);

#endif  // XR_STDLIB_HTTP_H
