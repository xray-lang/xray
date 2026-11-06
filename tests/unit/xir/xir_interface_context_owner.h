/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_interface_context_owner.h - Shared owned memory for interface witnesses
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
static XrXirCompileContext interface_context_limited(uint64_t held) {
    size_t previous_attempts = attempts, previous_failure = fail_at;
    fail_at = SIZE_MAX;
    XrXirCompileContext context = stage_context_limited(held);
    attempts = previous_attempts; fail_at = previous_failure;
    return context;
}
static void interface_temporary_clean(const XrXirCompileContext *context) {
    CHECK(stage_stats(context).held_bytes == stage_owner_baseline.held_bytes);
    CHECK(interface_live == stage_owner_count);
}
static XrXirCompileContext interface_exact_context(XrCompileResourceStats stats, unsigned boundary) {
    CHECK(boundary < 3 && stats.peak_bytes > stage_owner_baseline.held_bytes && stats.peak_bytes < UINT64_MAX);
    uint64_t peak = stats.peak_bytes - stage_owner_baseline.held_bytes;
    if (boundary == 1) --peak;
    if (boundary == 2) ++peak;
    return interface_context_limited(peak);
}
#endif // XIR_INTERFACE_CONTEXT_OWNER_H
