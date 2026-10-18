/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel.h - Shared owned channel storage and bounded FIFO transfer
 *
 * KEY CONCEPT:
 *   Handle copies share one identity. Queue publication transfers an admitted
 *   owner only after all fallible preparation has completed.
 */
#ifndef XXIR_CHANNEL_H
#define XXIR_CHANNEL_H
#include "xxir_value.h"

typedef enum XrXirChannelState {
    XR_XIR_CHANNEL_RETRY, XR_XIR_CHANNEL_SENT, XR_XIR_CHANNEL_VALUE,
    XR_XIR_CHANNEL_FULL, XR_XIR_CHANNEL_EMPTY, XR_XIR_CHANNEL_CLOSED
} XrXirChannelState;

/* The type must be closed and its definition-context Sendable element
 * obligation previously discharged. Construction grants no execution proof. */
XR_FUNC XrXirValueStatus xr_xir_channel_new(XrXirType type, int64_t capacity,
    XrXirValueAdmission *admission, XrXirValue *output);
/* SENT alone consumes owned. FULL, CLOSED, RETRY and failures preserve it.
 * RETRY is an internal scheduling outcome, never a public Full/Empty result. */
XR_FUNC XrXirValueStatus xr_xir_channel_send_owned(const XrXirValue *channel,
    XrXirValue *owned, XrXirValueAdmission *admission, XrXirChannelState *state);
/* All possible SendResult values are prepared before enqueue can consume owned.
 * SENT, FULL and CLOSED publish the matching enum; RETRY publishes nothing. */
XR_FUNC XrXirValueStatus xr_xir_channel_send_result(const XrXirValue *channel,
    XrXirValue *owned, XrXirType result_type, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirChannelState *state);
/* The ordinary argument keeps its logical copy in every outcome. The queue
 * receives a separate owner only on SENT, after all fallible preparation. */
XR_FUNC XrXirValueStatus xr_xir_channel_try_send(const XrXirValue *channel,
    const XrXirValue *message, XrXirType result_type, XrXirValueAdmission *admission,
    XrXirValue *output, XrXirChannelState *state);
/* Output must be canonical Unit. VALUE transfers the queued owner; failure
 * leaves the queue and output intact. Close still permits queued values. */
XR_FUNC XrXirValueStatus xr_xir_channel_receive(const XrXirValue *channel,
    XrXirValueAdmission *admission, XrXirValue *output, XrXirChannelState *state);
/* A governed Recv<T> is fully prepared before any queued owner is removed.
 * VALUE, EMPTY and CLOSED publish the matching enum; RETRY publishes nothing. */
XR_FUNC XrXirValueStatus xr_xir_channel_receive_result(const XrXirValue *channel,
    XrXirType result_type, XrXirValueAdmission *admission, XrXirValue *output, XrXirChannelState *state);
XR_FUNC XrXirValueStatus xr_xir_channel_close(const XrXirValue *channel,
    XrXirValueAdmission *admission, XrXirChannelState *state);
XR_FUNC XrXirValueStatus xr_xir_channel_info(const XrXirValue *channel,
    XrXirValueAdmission *admission, int64_t *capacity, bool *closed);
#endif // XXIR_CHANNEL_H
