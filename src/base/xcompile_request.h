/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_request.h - Single-request cooperative cancellation and deadline
 *
 * KEY CONCEPT:
 *   The caller owns this short-lived control and polls at explicit safe points.
 *   No allocator lock, memory quota or published artifact owns its callbacks.
 */
#ifndef XCOMPILE_REQUEST_H
#define XCOMPILE_REQUEST_H

#include "xcompile_resources.h"

typedef bool (*XrCompileCancelPoll)(const void *context);
typedef enum XrCompileDeadlineMode {
    XR_COMPILE_DEADLINE_NONE,
    XR_COMPILE_DEADLINE_AFTER
} XrCompileDeadlineMode;
typedef struct XrCompileRequestOptions {
    XrCompileCancelPoll cancel_poll;
    const void *cancel_context;
    XrCompileDeadlineMode deadline_mode;
    uint64_t timeout_ns;
} XrCompileRequestOptions;
typedef struct XrCompileRequestControl {
    XrCompileCancelPoll cancel_poll;
    const void *cancel_context;
    uint64_t deadline_ns;
    XrCompileResourceStatus status;
    bool initialized, has_deadline, polling;
} XrCompileRequestControl;

/* Initialize an all-zero, caller-owned control exactly once. Borrow the
 * callback/context until request destruction; never copy it into an artifact.
 * AFTER converts a duration to one monotonic absolute deadline without reset;
 * zero expires at the first poll. NONE requires timeout_ns == 0. Clock failure
 * and addition overflow preserve the empty output. No callback runs here.
 * Callers serialize this control, its polling and destruction. Child compiler
 * operations share this control instead of creating a fresh deadline. */
XR_FUNC XrCompileResourceStatus xr_compile_request_control_init(
    const XrCompileRequestOptions *options, XrCompileRequestControl *output);
/* Poll outside allocator locks. Deadline precedes callback; the first failure
 * remains local to this control. Callback may inspect memory stats, but cannot
 * recursively poll/reinitialize this control. No implicit allocator polling. */
XR_FUNC XrCompileResourceStatus xr_compile_request_control_poll(XrCompileRequestControl *control);

#endif // XCOMPILE_REQUEST_H
