/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_monitor_forward.h - Scheduler projection for monitor forwarding
 */

#ifndef XCORO_MONITOR_FORWARD_H
#define XCORO_MONITOR_FORWARD_H

#include "../base/xdefs.h"
#include "../runtime/value/xvalue.h"

#include <stdbool.h>

struct XrVMRuntime;

typedef void (*XrCoroMonitorForwardNotify)(void *context, XrValue reason);
typedef void (*XrCoroMonitorForwardDestroy)(void *context);

/* The provider consumes context on every return path. */
XR_FUNC bool xr_coro_monitor_forward(struct XrVMRuntime *isolate, const char *coroutine_name,
                                     XrCoroMonitorForwardNotify notify, void *context,
                                     XrCoroMonitorForwardDestroy destroy);

#endif  // XCORO_MONITOR_FORWARD_H
