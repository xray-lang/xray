/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmonitor_registry.h - Synchronized channel registry for named monitors
 */

#ifndef XMONITOR_REGISTRY_H
#define XMONITOR_REGISTRY_H

#include "../base/xdefs.h"

#include <stdbool.h>
#include <stdint.h>

struct XrChannel;
struct XrVMRuntime;
typedef struct XrMonitorRegistry XrMonitorRegistry;

XR_FUNC XrMonitorRegistry *xr_monitor_registry_new(void);
XR_FUNC void xr_monitor_registry_destroy(XrMonitorRegistry *registry);

XR_FUNC bool xr_monitor_registry_add_node(XrMonitorRegistry *registry, const char *node_name,
                                          struct XrChannel *channel);
XR_FUNC bool xr_monitor_registry_add_remote(XrMonitorRegistry *registry, const char *node_name,
                                            const char *coroutine_name, struct XrChannel *channel);
XR_FUNC bool xr_monitor_registry_remove_remote(XrMonitorRegistry *registry, const char *node_name,
                                               const char *coroutine_name,
                                               struct XrChannel *channel);

XR_FUNC void xr_monitor_registry_notify_node(XrMonitorRegistry *registry,
                                             struct XrVMRuntime *isolate, const char *node_name);
XR_FUNC void xr_monitor_registry_notify_remote(XrMonitorRegistry *registry,
                                               struct XrVMRuntime *isolate, const char *node_name,
                                               const char *coroutine_name, const char *reason);
XR_FUNC int64_t xr_monitor_registry_node_count(XrMonitorRegistry *registry);

#endif  // XMONITOR_REGISTRY_H
