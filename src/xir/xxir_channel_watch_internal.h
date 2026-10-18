/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_watch_internal.h - Owned, callback-free Channel readiness registration
 */
#ifndef XXIR_CHANNEL_WATCH_INTERNAL_H
#define XXIR_CHANNEL_WATCH_INTERNAL_H
#include "xxir_channel.h"
typedef struct XrXirChannelWatch XrXirChannelWatch;
typedef struct XrXirChannelObservation {
    uint64_t sequence;
    uint32_t capacity,count;
    bool closed;
} XrXirChannelObservation;
typedef enum XrXirChannelWatchState {
    XR_XIR_CHANNEL_WATCH_RETRY,XR_XIR_CHANNEL_WATCH_PENDING,XR_XIR_CHANNEL_WATCH_READY
} XrXirChannelWatchState;
/* RETRY preserves the observation. VALUE publishes one synchronized snapshot. */
XR_FUNC XrXirValueStatus xr_xir_channel_observe(const XrXirValue *channel,
    XrXirValueAdmission *admission,XrXirChannelObservation *output,XrXirChannelState *state);
/* Only PENDING publishes an owned watch. READY means the observed frontier
 * already changed or closed; RETRY means registration did not acquire its lock. */
XR_FUNC XrXirValueStatus xr_xir_channel_watch_begin(const XrXirValue *channel,uint64_t sequence,
    XrXirValueAdmission *admission,XrXirChannelWatch **output,XrXirChannelWatchState *state);
XR_FUNC bool xr_xir_channel_watch_ready(const XrXirChannelWatch *watch);
/* The unique owner may withdraw a pending notification without releasing it.
 * A prior notification wins; neither result takes the Channel queue lock. */
XR_FUNC bool xr_xir_channel_watch_cancel_pending(XrXirChannelWatch *watch);
/* Drops the unique external owner. Cancellation never waits for a queue lock;
 * the queue owns any deferred unlink storage, without borrowing a Call frame. */
XR_FUNC void xr_xir_channel_watch_drop(XrXirChannelWatch *watch);
#endif // XXIR_CHANNEL_WATCH_INTERNAL_H
