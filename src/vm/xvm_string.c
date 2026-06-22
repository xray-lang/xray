/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_string.c - VM isolate string helpers
 */

#include "xvm_string.h"
#include "../base/xchecks.h"
#include "../coro/xworker.h"
#include "../runtime/xisolate_api.h"
#include <stdio.h>

static inline XrStrBuf **vm_tmp_strbuf_slot(XrVMRuntime *X) {
    XrWorker *worker = xr_current_worker();
    if (worker && worker->m)
        return &worker->m->tmp_strbuf;
    return xr_isolate_tmp_strbuf_slot(X);
}

XrStrBuf *xr_strbuf_tmp(XrVMRuntime *X) {
    XR_DCHECK(X != NULL, "strbuf_tmp: NULL isolate");
    XrStrBuf **slot = vm_tmp_strbuf_slot(X);
    if (!slot)
        return NULL;

    if (!*slot) {
        *slot = xr_strbuf_new(X, XR_STRBUF_MIN_CAP);
        if (!*slot) {
            fprintf(stderr, "[ERROR] tmp_strbuf allocation failed\n");
            return NULL;
        }
    }

    xr_strbuf_reset(*slot);
    return *slot;
}
