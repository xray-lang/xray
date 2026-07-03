/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_string_intern.c - compiler-only XrString interning
 */

#include "../../runtime/object/xstring.h"
#include "../../runtime/xisolate_api.h"
#include "../../base/xlog.h"

XrString *xr_compile_time_intern(XrVMRuntime *iso, const char *chars, size_t len) {
    XrGlobalStringPool *pool = xr_isolate_get_string_pool(iso);
    if (!pool) {
        xr_log_warning("string", "compile_time_intern: isolate or global pool is NULL");
        return NULL;
    }

    uint32_t hash = xr_string_hash(chars, len);

    // Compile-time interning writes the SAME isolate-global pool as the runtime
    // interner (xr_string_intern_core). When compilation overlaps runtime string
    // interning — REPL, LSP eval, or runtime `eval` while go coroutines run — an
    // unlocked read/write corrupts the pool's open-addressed table (P1-4).
    // Mirror the runtime path: rdlock lookup, then double-checked wrlock insert.
    xr_rwlock_rdlock(&pool->lock);
    XrString *found = xr_global_pool_lookup(pool, chars, len, hash);
    xr_rwlock_rdunlock(&pool->lock);
    if (found)
        return found;

    xr_rwlock_wrlock(&pool->lock);
    found = xr_global_pool_lookup(pool, chars, len, hash);
    if (found) {
        xr_rwlock_wrunlock(&pool->lock);
        return found;
    }
    // xr_global_pool_insert additionally marks the string permanent (immune to
    // pool sweeps) — that bookkeeping mutates the pool and must stay under lock.
    XrString *str = xr_global_pool_insert(pool, iso, chars, len, hash);
    xr_rwlock_wrunlock(&pool->lock);
    return str;
}
