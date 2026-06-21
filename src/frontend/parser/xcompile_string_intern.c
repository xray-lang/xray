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

XrString *xr_compile_time_intern(XrayIsolate *iso, const char *chars, size_t len) {
    XrGlobalStringPool *pool = xr_isolate_get_string_pool(iso);
    if (!pool) {
        xr_log_warning("string", "compile_time_intern: isolate or global pool is NULL");
        return NULL;
    }

    uint32_t hash = xr_string_hash(chars, len);
    XrString *found = xr_global_pool_lookup(pool, chars, len, hash);
    if (found)
        return found;

    return xr_global_pool_insert(pool, iso, chars, len, hash);
}
