/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_emission_arena_probe.c - Actual receiving arena with isolated cache disposal
 */
#include "../../../src/base/xarena.c"

/* The test counts cache ownership as well as plan ownership. Dispose the
 * isolated cache between injections so retained segments cannot hide leaks. */
XR_FUNC void xr_test_emission_arena_flush_cache(void) {
    XrArenaSegmentCache *cache = arena_cache_current(false);
    if (!cache)
        return;
    arena_cache_flush(cache);
#if defined(XR_OS_WINDOWS)
    XR_CHECK(FlsSetValue(arena_cache_fls_index, NULL), "test arena cache detach failed");
    xr_free(cache);
#else
    XR_CHECK(pthread_setspecific(arena_cache_key, NULL) == 0, "test arena cache detach failed");
    xr_free(cache);
#endif
}
