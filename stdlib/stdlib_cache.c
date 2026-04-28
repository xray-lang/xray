/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * stdlib_cache.c - Per-isolate stdlib cache: create / get / free
 *
 * The cache pointer lives as an opaque `void *` on XrayIsolate so that
 * the core header never needs to include stdlib types. This translation
 * unit is the only place that casts between the opaque pointer and the
 * concrete XrStdlibCache struct, keeping the coupling local.
 */

#include "stdlib_cache.h"

#include <string.h>

#include "../src/base/xmalloc.h"
#include "../src/runtime/xisolate_internal.h"

XR_FUNC XrStdlibCache *xr_stdlib_cache_get(XrayIsolate *isolate) {
    XrStdlibCache *c = (XrStdlibCache *) isolate->stdlib_cache;
    if (c)
        return c;
    c = (XrStdlibCache *) xr_malloc(sizeof(XrStdlibCache));
    if (!c) {
        /* Match xmalloc OOM policy used by the rest of stdlib. */
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    isolate->stdlib_cache = c;
    return c;
}

XR_FUNC void xr_stdlib_cache_free(XrayIsolate *isolate) {
    if (!isolate || !isolate->stdlib_cache)
        return;
    XrStdlibCache *c = (XrStdlibCache *) isolate->stdlib_cache;

    /* Tear down per-isolate log state (async thread, mutex, logger). */
    if (c->log_state_cleanup && c->log_state) {
        c->log_state_cleanup(c->log_state);
    }

    /* Shapes and interned strings are GC-managed; freeing the
     * container is sufficient. */
    xr_free(c);
    isolate->stdlib_cache = NULL;
}
