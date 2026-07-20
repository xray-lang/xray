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

void xr_process_shutdown(void) {
    /*
     * Release process-wide (non-isolate) runtime registries. Keep this the
     * single ordered teardown point for defense line 4. Each release must be
     * idempotent so repeated calls (e.g. atexit + explicit) stay safe.
     *
     * Type system: clears the borrowed per-thread current type pool pointer
     * so no stale cross-lifetime borrow survives shutdown.
     */
    xr_type_global_shutdown();

    /*
     * NOTE (task 218 P1 / follow-up): additional process-exit leaks reported
     * by the lsan_strict lane originate outside the runtime's editable surface
     * (the frontend XRD module cache and the analyzer's thread-local symbol
     * registry). As those layers adopt this hook, add their releases here so
     * the LSan suppression list only ever shrinks.
     */
}
