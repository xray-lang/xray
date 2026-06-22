/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_monitor.h - CLI coroutine monitor entry points
 */

#ifndef XCORO_MONITOR_H
#define XCORO_MONITOR_H

#include "../base/xdefs.h"
#include "../base/xforward_decl.h"

XR_FUNC void xray_vm_coro_monitor_start(XrVMRuntime *X, int watch_interval_ms, int http_port);

#endif  // XCORO_MONITOR_H
