/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_native_fixture_owner.h - Held-memory graph resources for native callback proofs
 *
 * KEY CONCEPT:
 *   Descriptor construction and Program sealing retain the same controlled memory owner.
 *   Physical observation belongs to the consuming test translation unit.
 */
#ifndef XIR_NATIVE_FIXTURE_OWNER_H
#define XIR_NATIVE_FIXTURE_OWNER_H
#include "xir/xxir.h"
#define NATIVE_FIXTURE_BYTES UINT64_C(2097152)
typedef struct NativeFixtureOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
} NativeFixtureOwner;
static uint64_t native_fixture_owners, native_fixture_max_allocated;
static uint64_t native_fixture_max_peak;
static inline XrCompileResourceStats native_fixture_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}
static inline XrXirStatus native_fixture_owner_new(NativeFixtureOwner *owner) {
    CHECK(!owner->context.resources);
    const XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, NATIVE_FIXTURE_BYTES, true};
    XrCompileResourceStatus status = xr_compile_resources_new(&limits, &owner->context.resources);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            status == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_XIR_MEMORY_LIMIT :
            status == XR_COMPILE_RESOURCE_OVERFLOW ? XR_XIR_OVERFLOW : XR_XIR_BAD_STRUCTURE;
    owner->context.limits = xr_xir_compile_default_limits();
    owner->baseline = native_fixture_stats(&owner->context);
    return XR_XIR_OK;
}
static inline void native_fixture_owner_free(NativeFixtureOwner *owner) {
    if (!owner->context.resources) return;
    XrCompileResourceStats stats = native_fixture_stats(&owner->context);
    CHECK(stats.held_bytes >= owner->baseline.held_bytes);
    CHECK(stats.peak_bytes <= NATIVE_FIXTURE_BYTES);
    ++native_fixture_owners;
    if (stats.allocated_bytes > native_fixture_max_allocated) native_fixture_max_allocated = stats.allocated_bytes;
    if (stats.peak_bytes > native_fixture_max_peak) native_fixture_max_peak = stats.peak_bytes;
    /* Programs and escaping values may still pin real allocations after this owner dies. */
    xr_compile_resources_release(owner->context.resources);
    *owner = (NativeFixtureOwner){0};
}
static inline void *native_fixture_one_byte_remaining(const XrXirCompileContext *context) {
    /* Measure the real held footprint without depending on allocator layout. */
    XrCompileResourceStats before = native_fixture_stats(context);
    void *probe = NULL;
    CHECK(xr_compile_resources_alloc(context->resources, 1, &probe) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats after = native_fixture_stats(context);
    CHECK(after.held_bytes > before.held_bytes + 1);
    const uint64_t overhead = after.held_bytes - before.held_bytes - 1;
    xr_compile_resources_free(probe);
    CHECK(native_fixture_stats(context).held_bytes == before.held_bytes);
    CHECK(before.held_bytes < NATIVE_FIXTURE_BYTES && overhead < NATIVE_FIXTURE_BYTES - before.held_bytes - 1);
    const uint64_t payload = NATIVE_FIXTURE_BYTES - before.held_bytes - overhead - 1;
    CHECK(payload && payload <= SIZE_MAX);
    void *reservation = NULL;
    CHECK(xr_compile_resources_alloc(context->resources, (size_t)payload, &reservation) == XR_COMPILE_RESOURCE_OK);
    after = native_fixture_stats(context);
    CHECK(NATIVE_FIXTURE_BYTES - after.held_bytes == 1);
    return reservation;
}
static inline void native_fixture_owner_report(void) {
    printf("native compiler owners=%llu observed allocated=%llu peak=%llu held-limit=%llu\n",
        (unsigned long long)native_fixture_owners, (unsigned long long)native_fixture_max_allocated,
        (unsigned long long)native_fixture_max_peak, (unsigned long long)NATIVE_FIXTURE_BYTES);
}
#endif // XIR_NATIVE_FIXTURE_OWNER_H
