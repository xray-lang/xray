/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xio_runtime.c - Runtime-owned IO subsystem implementation
 *
 * KEY CONCEPT:
 *   This file is intentionally tiny. The IO runtime today wraps the
 *   DNS cache; per-subsystem state owns its own lifecycle helpers
 *   (xr_io_dns_cache_init / xr_io_dns_cache_destroy live in xdns.c
 *   so xio_runtime.c never needs to know cache layout).
 */

#include "xio_runtime.h"
#include "xdns.h"
#include "../coro/xworker.h"
#include "../base/xmalloc.h"
#include "../runtime/xisolate_internal.h"

#include <string.h>

XrIoRuntime *xr_io_runtime_new(void) {
    XrIoRuntime *io = (XrIoRuntime *) xr_calloc(1, sizeof(XrIoRuntime));
    if (!io)
        return NULL;
    xr_io_dns_cache_init(&io->dns);
    io->inited = true;
    return io;
}

void xr_io_runtime_free(XrIoRuntime *io) {
    if (!io)
        return;
    if (io->inited) {
        xr_io_dns_cache_destroy(&io->dns);
        io->inited = false;
    }
    xr_free(io);
}

XrIoRuntime *xr_io_runtime_from_isolate(struct XrayIsolate *X) {
    if (!X || !X->vm.runtime)
        return NULL;
    return ((XrRuntime *) X->vm.runtime)->io;
}

XrIoRuntime *xr_io_runtime_from_runtime(struct XrRuntime *rt) {
    if (!rt)
        return NULL;
    return rt->io;
}
