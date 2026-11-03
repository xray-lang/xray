/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_select_internal.h - Owned buffered send and receive selection
 *
 * KEY CONCEPT:
 *   Every candidate is observed together, but only one prepared operation can
 *   commit. A parked owner retains values and notifications, never a frame.
 */
#ifndef XXIR_CHANNEL_SELECT_INTERNAL_H
#define XXIR_CHANNEL_SELECT_INTERNAL_H
#include "xxir_channel.h"

typedef struct XrXirChannelSelect XrXirChannelSelect;
typedef struct XrXirChannelSelectRequest {
    const XrXirValue *channels;
    uint32_t count;
    bool has_default,has_timeout;
    int64_t timeout_ms;
    uint64_t now_ns;
} XrXirChannelSelectRequest;
typedef enum XrXirChannelSelectState {
    XR_XIR_CHANNEL_SELECT_RETRY,
    XR_XIR_CHANNEL_SELECT_PENDING,
    XR_XIR_CHANNEL_SELECT_CANDIDATE,
    XR_XIR_CHANNEL_SELECT_COMMITTED,
    XR_XIR_CHANNEL_SELECT_CANCELLED
} XrXirChannelSelectState;
typedef enum XrXirChannelSelectKind {
    XR_XIR_CHANNEL_SELECT_RECEIVE,
    XR_XIR_CHANNEL_SELECT_DEFAULT,
    XR_XIR_CHANNEL_SELECT_TIMEOUT,
    XR_XIR_CHANNEL_SELECT_SEND,
    XR_XIR_CHANNEL_SELECT_SEND_CLOSED
} XrXirChannelSelectKind;
typedef struct XrXirChannelSelectDeadline {
    uint64_t start_ns,last_ns,duration_ns;
    bool enabled;
} XrXirChannelSelectDeadline;

/* Copies all receive handles and owned (Channel, payload) send pairs.
 * Closed empty Channels are not receive-ready; closed sends are ready panics.
 * Default plus timeout and open zero-capacity rendezvous are not admitted here.
 * Positive deadlines use an already validated, positive monotonic sample;
 * the caller owns the clock provider and reports its failures separately. */
XR_FUNC XrXirValueStatus xr_xir_channel_select_begin(const XrXirChannelSelectRequest *request,
    XrXirValueAdmission *admission,XrXirChannelSelect **output);
/* Reuses the original handles and deadline. Entropy is one fresh uniform
 * random word from the execution provider, never a source-language selector.
 * Rejection sampling may request RETRY without consuming a queue value.
 * CANDIDATE owns a typed payload snapshot but has not consumed any message. */
XR_FUNC XrXirValueStatus xr_xir_channel_select_probe(XrXirChannelSelect *select,
    uint64_t now_ns,uint64_t entropy,XrXirValueAdmission *admission,XrXirChannelSelectState *state);
/* The borrow expires on probe, commit or drop. A bridge can fully prepare its
 * typed arm result before commit; that preparation must not consume this value. */
XR_FUNC bool xr_xir_channel_select_candidate(const XrXirChannelSelect *select,
    XrXirChannelSelectKind *kind,uint32_t *arm,const XrXirValue **value);
/* Rechecks the complete observed frontier under the same lock set. RETRY or
 * failure leaves all queues unchanged. Success cannot require another fallible
 * result allocation, retain or host callback before publication by the bridge.
 * A changed timer readiness set also returns RETRY without consuming a queue. */
XR_FUNC XrXirValueStatus xr_xir_channel_select_commit(XrXirChannelSelect *select,
    uint64_t now_ns,XrXirValueAdmission *admission,XrXirChannelSelectState *state);
/* Copies the actual timer owner. Subtraction after monotonic validation avoids
 * absolute-deadline overflow; the caller must not keep a pointer into owner. */
XR_FUNC bool xr_xir_channel_select_deadline(const XrXirChannelSelect *select,
    XrXirChannelSelectDeadline *output);
/* Notification is not readiness or permission to consume. The driver probes
 * again after any event and supplies a fresh monotonic sample and entropy. */
XR_FUNC bool xr_xir_channel_select_notified(const XrXirChannelSelect *select);
XR_FUNC bool xr_xir_channel_select_take(XrXirChannelSelect *select,
    XrXirChannelSelectKind *kind,uint32_t *arm,XrXirValue *output);
/* A winning commit is never undone by a later cancellation request. Drop is
 * the unique owner's final retirement and consumes every unclaimed payload. */
XR_FUNC bool xr_xir_channel_select_cancel(XrXirChannelSelect *select);
XR_FUNC void xr_xir_channel_select_drop(XrXirChannelSelect *select);
#endif // XXIR_CHANNEL_SELECT_INTERNAL_H
