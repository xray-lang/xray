/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_process_shutdown.c - process-wide registry teardown (task 218 line 4).
 */

#include "xr_process_shutdown.h"

#include "value/xtype.h"

#define XR_PROCESS_SHUTDOWN_CALLBACK_CAP 16

static XrProcessShutdownCallback g_shutdown_callbacks[XR_PROCESS_SHUTDOWN_CALLBACK_CAP];
static size_t g_shutdown_callback_count = 0;

bool xr_process_shutdown_register(XrProcessShutdownCallback callback) {
    if (!callback)
        return false;
    for (size_t i = 0; i < g_shutdown_callback_count; i++) {
        if (g_shutdown_callbacks[i] == callback)
            return true;
    }
    if (g_shutdown_callback_count == XR_PROCESS_SHUTDOWN_CALLBACK_CAP)
        return false;
    g_shutdown_callbacks[g_shutdown_callback_count++] = callback;
    return true;
}

void xr_process_shutdown(void) {
    /*
     * Release process-wide (non-isolate) runtime registries. Keep this the
     * single ordered teardown point for defense line 4. Each release must be
     * idempotent so repeated calls (e.g. atexit + explicit) stay safe.
     *
     * Type system: clears the borrowed per-thread current type pool pointer
     * so no stale cross-lifetime borrow survives shutdown.
     */
    for (size_t i = g_shutdown_callback_count; i > 0; i--)
        g_shutdown_callbacks[i - 1]();
    xr_type_global_shutdown();
}
