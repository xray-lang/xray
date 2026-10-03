/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_host_time.h - Operating-system clocks and timer readiness for hosts
 *
 * KEY CONCEPT:
 *   A timer request is only a duration. The host owns the monotonic start, the
 *   elapsed comparison and every sleep; an early wake never grants readiness.
 */
#ifndef XR_XIR_HOST_TIME_H
#define XR_XIR_HOST_TIME_H
#include "../xir/xxir_call.h"

/* Fills the system provider: realtime, process CPU and monotonic nanoseconds
 * and the local UTC offset. A zero realtime or monotonic reading, or a value
 * beyond the signed range, is a failed read; an offset the platform cannot
 * represent is a range rejection. The provider has no context and no state. */
XR_FUNC void xr_xir_host_time_provider(XrXirTimeProvider *provider);

typedef enum XrXirHostWaitStatus {
    XR_XIR_HOST_WAIT_DUE, XR_XIR_HOST_WAIT_PENDING, XR_XIR_HOST_WAIT_FAILED,
    XR_XIR_HOST_WAIT_BAD_ARGUMENT
} XrXirHostWaitStatus;
/* One wait per suspension. The start is read once by begin; repeated polls never
 * restart it. A yield request is due immediately. */
typedef struct XrXirHostWait {
    uint64_t start_ns, duration_ns;
    bool active;
} XrXirHostWait;
XR_FUNC XrXirHostWaitStatus xr_xir_host_wait_begin(XrXirHostWait *wait, const XrXirWaitRequest *request);
/* Reads the monotonic clock and reports DUE once at least the full duration has
 * elapsed. While pending it first sleeps for at most max_sleep_ns (0 does not
 * sleep) and then compares again. A failed or backward clock is FAILED, never DUE. */
XR_FUNC XrXirHostWaitStatus xr_xir_host_wait_poll(XrXirHostWait *wait, uint64_t max_sleep_ns);
#endif // XR_XIR_HOST_TIME_H
