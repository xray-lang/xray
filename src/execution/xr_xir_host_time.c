/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_host_time.c - System clock provider and monotonic timer readiness
 *
 * KEY CONCEPT:
 *   Elapsed time is always recomputed from the monotonic clock after any sleep,
 *   so scheduler wakeups can only delay readiness, never grant it early.
 */
#include "xr_xir_host_time.h"
#include "../os/os_time.h"
#include "../shared/xr_time_offset.h"

static XrXirTimeStatus host_clock(void *context, XrXirClockKind clock, int64_t *nanoseconds) {
    (void) context;
    uint64_t reading = 0;
    switch (clock) {
    case XR_XIR_CLOCK_REALTIME: reading = xr_time_realtime_ns(); if (!reading) return XR_XIR_TIME_FAILED; break;
    case XR_XIR_CLOCK_MONOTONIC: reading = xr_time_monotonic_ns(); if (!reading) return XR_XIR_TIME_FAILED; break;
    case XR_XIR_CLOCK_CPU: reading = xr_time_process_cpu_ns(); break;
    default: return XR_XIR_TIME_FAILED;
    }
    if (reading > (uint64_t) INT64_MAX) return XR_XIR_TIME_FAILED;
    *nanoseconds = (int64_t) reading;
    return XR_XIR_TIME_OK;
}
static XrXirTimeStatus host_utc_offset(void *context, int64_t seconds, int64_t *minutes) {
    (void) context;
    return xr_time_utc_offset_at(seconds, minutes) ? XR_XIR_TIME_OK : XR_XIR_TIME_RANGE;
}
XR_FUNC void xr_xir_host_time_provider(XrXirTimeProvider *provider) {
    if (!provider) return;
    *provider = (XrXirTimeProvider) {XR_XIR_CALL_ABI_VERSION, 0, host_clock, host_utc_offset, NULL};
}

XR_FUNC XrXirHostWaitStatus xr_xir_host_wait_begin(XrXirHostWait *wait, const XrXirWaitRequest *request) {
    if (!wait || !request || request->reserved) return XR_XIR_HOST_WAIT_BAD_ARGUMENT;
    XrXirHostWait next = {0};
    if (request->kind == XR_XIR_WAIT_TIMER_MS) {
        if (request->after_ms < 1 || request->after_ms > XR_XIR_TIMER_MAX_MS) return XR_XIR_HOST_WAIT_BAD_ARGUMENT;
        next.duration_ns = request->after_ms * UINT64_C(1000000);
        next.start_ns = xr_time_monotonic_ns();
        if (!next.start_ns) return XR_XIR_HOST_WAIT_FAILED;
    } else if (request->kind != XR_XIR_WAIT_YIELD || request->after_ms) return XR_XIR_HOST_WAIT_BAD_ARGUMENT;
    next.active = true;
    *wait = next;
    return XR_XIR_HOST_WAIT_PENDING;
}
XR_FUNC XrXirHostWaitStatus xr_xir_host_wait_poll(XrXirHostWait *wait, uint64_t max_sleep_ns) {
    if (!wait || !wait->active) return XR_XIR_HOST_WAIT_BAD_ARGUMENT;
    if (!wait->duration_ns) return XR_XIR_HOST_WAIT_DUE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint64_t now = xr_time_monotonic_ns();
        if (!now || now < wait->start_ns) return XR_XIR_HOST_WAIT_FAILED;
        uint64_t elapsed = now - wait->start_ns;
        if (elapsed >= wait->duration_ns) return XR_XIR_HOST_WAIT_DUE;
        if (attempt || !max_sleep_ns) return XR_XIR_HOST_WAIT_PENDING;
        uint64_t remaining = wait->duration_ns - elapsed;
        xr_time_sleep_ns(remaining < max_sleep_ns ? remaining : max_sleep_ns);
    }
    return XR_XIR_HOST_WAIT_PENDING;
}
