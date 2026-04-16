/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_debug_hooks.c - Debug hook registration
 */

#include "xray_debug_hooks.h"
#include "xisolate_api.h"
#include <stddef.h>

void xr_debug_register_hooks(XrayIsolate *isolate, XrDebugHooks *hooks) {
    if (!isolate) return;
    xr_isolate_set_debug_hooks(isolate, hooks);
}

XrDebugHooks *xr_debug_get_hooks(XrayIsolate *isolate) {
    if (!isolate) return NULL;
    return (XrDebugHooks *)xr_isolate_get_debug_hooks(isolate);
}
