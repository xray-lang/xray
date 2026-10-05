/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_interface_context_owner.h - Shared finite ledgers for interface witnesses
 */
#ifndef XIR_INTERFACE_CONTEXT_OWNER_H
#define XIR_INTERFACE_CONTEXT_OWNER_H
#include "xir_stage_context_owner.h"

/* Ledger setup is outside the operation's injected allocator failure matrix.
 * Its physical allocation remains metered until the final owner release. */
static XrXirCompileContext interface_context_default(void) {
    size_t previous_attempts = attempts, previous_failure = fail_at;
    fail_at = SIZE_MAX;
    stage_context = stage_context_default();
    attempts = previous_attempts; fail_at = previous_failure;
    return stage_context;
}
static const XrXirCompileContext *interface_context_pointer_default(void) {
    (void)interface_context_default();
    return &stage_owners[stage_owner_count - 1].context;
}
static XrXirCompileContext interface_context_limited(uint64_t allocated, uint64_t peak, uint64_t work) {
    size_t previous_attempts = attempts, previous_failure = fail_at;
    fail_at = SIZE_MAX;
    XrXirCompileContext context = stage_context_limited(allocated, peak, work);
    attempts = previous_attempts; fail_at = previous_failure;
    return context;
}
static uint64_t interface_work(const XrXirCompileContext *context) {
    return stage_stats(context).work - stage_owner_baseline.work;
}
static void interface_temporary_clean(const XrXirCompileContext *context) {
    CHECK(stage_stats(context).live_bytes == stage_owner_baseline.live_bytes);
    CHECK(interface_live == stage_owner_count);
}
static XrXirCompileContext interface_exact_context(XrCompileResourceStats stats, unsigned boundary) {
    CHECK(stats.allocated_bytes > stage_owner_baseline.allocated_bytes);
    CHECK(stats.peak_bytes > stage_owner_baseline.live_bytes && stats.work > stage_owner_baseline.work);
    uint64_t allocated = stats.allocated_bytes - stage_owner_baseline.allocated_bytes;
    uint64_t peak = stats.peak_bytes - stage_owner_baseline.live_bytes;
    uint64_t work = stats.work - stage_owner_baseline.work;
    if (boundary == 1) --allocated;
    if (boundary == 2) --peak;
    if (boundary == 3) --work;
    return interface_context_limited(allocated, peak, work);
}
static void interface_exhaust_work(const XrXirCompileContext *context) {
    uint64_t used = stage_stats(context).work;
    CHECK(used <= STAGE_WORK);
    CHECK(xr_compile_resources_work(context->resources, STAGE_WORK - used) == XR_COMPILE_RESOURCE_OK);
}
#endif // XIR_INTERFACE_CONTEXT_OWNER_H
