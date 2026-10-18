/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_time_internal.h - Exact host clock configuration inheritance
 */
#ifndef XXIR_CALL_TIME_INTERNAL_H
#define XXIR_CALL_TIME_INTERNAL_H
#include "xxir_call_internal.h"
/* Copies only immutable configuration. The host still owns callback context
 * until the receiving Call retires; no callback or clock read occurs here. */
XR_FUNC bool xr_xir_call_time_provider(const XrXirCall *call,XrXirTimeProvider *output);
/* The exact executor binding observes only its current Channel deadline.
 * READY includes still pending; callers separately query owned readiness. */
XR_FUNC XrXirCallStatus xr_xir_call_channel_deadline_poll(XrXirCall *call,
    const XrXirExecutorBinding *binding);
#endif // XXIR_CALL_TIME_INTERNAL_H
