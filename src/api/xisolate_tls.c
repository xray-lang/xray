/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_tls.c - Thread-local isolate storage and enter/exit
 *
 * WHY THIS DESIGN:
 *   Separate compilation unit so that any isolate variant (full/lite)
 *   can set the current isolate without pulling in the heavy lifecycle
 *   code from xray_isolate.c.
 */

#include "../runtime/xisolate_internal.h"
#include "../base/xchecks.h"
#include <stdio.h>
#include "../base/xlog.h"

XR_THREAD_LOCAL XrayIsolate *g_current_isolate = NULL;

void xray_isolate_enter(XrayIsolate *isolate) {
    if (isolate == NULL) {
        xr_log_warning("isolate", "isolate_enter: isolate is NULL");
        return;
    }
    g_current_isolate = isolate;
}

void xray_isolate_exit(void) {
    g_current_isolate = NULL;
}
