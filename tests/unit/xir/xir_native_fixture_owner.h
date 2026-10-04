/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_native_fixture_owner.h - Finite graph resources for native callback proofs
 *
 * KEY CONCEPT:
 *   Descriptor construction and Program sealing charge the same finite ledger.
 *   Physical observation belongs to the consuming test translation unit.
 */
#ifndef XIR_NATIVE_FIXTURE_OWNER_H
#define XIR_NATIVE_FIXTURE_OWNER_H
#include "xir/xxir.h"
#define NATIVE_FIXTURE_BYTES UINT64_C(2097152)
#define NATIVE_FIXTURE_WORK UINT64_C(16000000)
typedef struct NativeFixtureOwner {
    XrXirCompileContext context;
    XrCompileResourceStats baseline;
} NativeFixtureOwner;
static uint64_t native_fixture_owners, native_fixture_max_allocated;
static uint64_t native_fixture_max_work, native_fixture_max_peak;
static inline XrCompileResourceStats native_fixture_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}
static inline XrXirStatus native_fixture_owner_new(NativeFixtureOwner *owner) {
    CHECK(!owner->context.resources);
    const XrCompileResourceLimits limits = {NATIVE_FIXTURE_BYTES, NATIVE_FIXTURE_BYTES, NATIVE_FIXTURE_WORK};
    XrCompileResourceStatus status = xr_compile_resources_new(&limits, &owner->context.resources);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY :
            status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    owner->context.limits = xr_xir_compile_default_limits();
    owner->baseline = native_fixture_stats(&owner->context);
    return XR_XIR_OK;
}
static inline void native_fixture_owner_free(NativeFixtureOwner *owner) {
    if (!owner->context.resources) return;
    XrCompileResourceStats stats = native_fixture_stats(&owner->context);
    CHECK(stats.live_bytes >= owner->baseline.live_bytes);
    CHECK(stats.allocated_bytes <= NATIVE_FIXTURE_BYTES && stats.peak_bytes <= NATIVE_FIXTURE_BYTES);
    CHECK(stats.work <= NATIVE_FIXTURE_WORK);
    ++native_fixture_owners;
    if (stats.allocated_bytes > native_fixture_max_allocated) native_fixture_max_allocated = stats.allocated_bytes;
    if (stats.work > native_fixture_max_work) native_fixture_max_work = stats.work;
    if (stats.peak_bytes > native_fixture_max_peak) native_fixture_max_peak = stats.peak_bytes;
    /* Programs and escaping values may still pin real allocations after this owner dies. */
    xr_compile_resources_release(owner->context.resources);
    *owner = (NativeFixtureOwner){0};
}
static inline void *native_fixture_one_byte_remaining(const XrXirCompileContext *context) {
    /* A real allocation consumes the same ledger, without changing its frozen limit. */
    XrCompileResourceStats before = native_fixture_stats(context);
    void *probe = NULL;
    CHECK(xr_compile_resources_alloc(context->resources, 1, &probe) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats after = native_fixture_stats(context);
    CHECK(after.allocated_bytes > before.allocated_bytes + 1);
    const uint64_t overhead = after.allocated_bytes - before.allocated_bytes - 1;
    xr_compile_resources_free(probe);
    CHECK(after.allocated_bytes + overhead + 1 < NATIVE_FIXTURE_BYTES);
    const uint64_t payload = NATIVE_FIXTURE_BYTES - after.allocated_bytes - overhead - 1;
    CHECK(payload && payload <= SIZE_MAX);
    void *reservation = NULL;
    CHECK(xr_compile_resources_alloc(context->resources, (size_t)payload, &reservation) == XR_COMPILE_RESOURCE_OK);
    after = native_fixture_stats(context);
    CHECK(NATIVE_FIXTURE_BYTES - after.allocated_bytes == 1);
    return reservation;
}
static inline void native_fixture_owner_report(void) {
    printf("native compiler owners=%llu max allocated/work/peak=%llu/%llu/%llu limits=%llu/%llu\n",
        (unsigned long long)native_fixture_owners, (unsigned long long)native_fixture_max_allocated,
        (unsigned long long)native_fixture_max_work, (unsigned long long)native_fixture_max_peak,
        (unsigned long long)NATIVE_FIXTURE_BYTES, (unsigned long long)NATIVE_FIXTURE_WORK);
}
#endif // XIR_NATIVE_FIXTURE_OWNER_H
