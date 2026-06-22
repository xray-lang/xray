/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate_params.c - Isolate parameter initialization
 *
 * WHY THIS DESIGN:
 *   Separate .o with zero heavy dependencies. Full/runtime variants use
 *   explicit constructors in their own translation units.
 */

#include "../runtime/xisolate_api.h"
#include "../base/xchecks.h"
#include "xray_isolate.h"
#include <string.h>

void xray_isolate_params_init(XrayIsolateParams *params) {
    if (params == NULL)
        return;
    memset(params, 0, sizeof(XrayIsolateParams));

    params->backend_type = XRAY_BACKEND_BYTECODE;
    params->initial_heap_size = 1024 * 1024;
    params->max_heap_size = 0;
}
