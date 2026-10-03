/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_scalar_compile_owner.h - Observe the scalar fixture compiler ledger
 *
 * KEY CONCEPT:
 *   Every stage shares one ledger and returns its live allocations on cleanup.
 */
#ifndef XIR_SCALAR_COMPILE_OWNER_H
#define XIR_SCALAR_COMPILE_OWNER_H
#include "xir/xxir_compile_context.h"
#include <stdio.h>

typedef struct ScalarCompileOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
} ScalarCompileOwner;

static ScalarCompileOwner scalar_owner;

static inline void scalar_compile_owner_new(ScalarCompileOwner *owner,
    XrCompileResourceLimits limits, XrXirCompileLimits structural) {
    CHECK(!owner->context.resources);
    CHECK(xr_compile_resources_new(&limits, &owner->context.resources) == XR_COMPILE_RESOURCE_OK);
    owner->context.limits = structural;
    CHECK(xr_compile_resources_stats(owner->context.resources, &owner->baseline) == XR_COMPILE_RESOURCE_OK);
}

static inline void scalar_compile_owner_free(ScalarCompileOwner *owner) {
    XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(owner->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == owner->baseline.live_bytes);
    CHECK(stats.allocated_bytes >= owner->baseline.allocated_bytes);
    CHECK(stats.work >= owner->baseline.work);
    if (owner == &scalar_owner)
        fprintf(stderr, "scalar compiler ledger: work=%llu allocated=%llu peak=%llu live=%llu\n",
            (unsigned long long) stats.work, (unsigned long long) stats.allocated_bytes,
            (unsigned long long) stats.peak_bytes, (unsigned long long) stats.live_bytes);
    xr_compile_resources_release(owner->context.resources);
    *owner = (ScalarCompileOwner) {0};
}

static inline void scalar_compile_begin(void) {
    scalar_compile_owner_new(&scalar_owner,
        /* Fewer than 10,000 runs each recheck the complete scalar artifact.
         * Bound cumulative charges separately from the 16 MiB live limit. */
        (XrCompileResourceLimits) {UINT64_C(8589934592), UINT64_C(16777216), UINT64_C(34359738368)},
        xr_xir_compile_default_limits());
}

#endif // XIR_SCALAR_COMPILE_OWNER_H
