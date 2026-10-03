/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_fixture_owner.h - Bounded ownership for source execution fixtures
 *
 * KEY CONCEPT:
 *   Parser, artifact, packet and Program stages share one finite ledger.
 */
#ifndef XIR_SOURCE_FIXTURE_OWNER_H
#define XIR_SOURCE_FIXTURE_OWNER_H
#include "xir/xxir_compile_context.h"

typedef struct SourceFixtureOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
} SourceFixtureOwner;

static inline void source_fixture_owner_new(SourceFixtureOwner *owner) {
    CHECK(!owner->context.resources);
    /* The mixed Array consumer has the largest cumulative charge: each VM
     * binding verifies the artifact. Bounds cover both execution directions. */
    const XrCompileResourceLimits limits = {
        UINT64_C(64) * 1024 * 1024, UINT64_C(8) * 1024 * 1024, UINT64_C(128000000)};
    CHECK(xr_compile_resources_new(&limits, &owner->context.resources) == XR_COMPILE_RESOURCE_OK);
    owner->context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(owner->context.resources, &owner->baseline) == XR_COMPILE_RESOURCE_OK);
}

static inline void source_fixture_owner_free(SourceFixtureOwner *owner) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(owner->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == owner->baseline.live_bytes);
    CHECK(stats.allocated_bytes > owner->baseline.allocated_bytes && stats.work > owner->baseline.work);
    fprintf(stderr, "source fixture compiler ledger: work=%llu allocated=%llu peak=%llu live=%llu\n",
        (unsigned long long) stats.work, (unsigned long long) stats.allocated_bytes,
        (unsigned long long) stats.peak_bytes, (unsigned long long) stats.live_bytes);
    xr_compile_resources_release(owner->context.resources);
    *owner = (SourceFixtureOwner) {0};
}
#endif // XIR_SOURCE_FIXTURE_OWNER_H
