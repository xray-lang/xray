/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchannel_ops.h — Single-source channel operation helpers
 *
 * All non-blocking channel operations (tryRecv, trySend) are defined
 * here exactly once, then called from VM instruction dispatch, VM cold
 * call dispatch, and JIT runtime helpers.  This eliminates the class of
 * bugs where one path diverges from another (missing deep copy, missing
 * ok flag, missing unbuffered rendezvous, etc.).
 *
 * Blocking send/recv cannot be fully unified because the yield/suspend
 * mechanism differs between VM (frame-save) and JIT (CPS suspend).
 * However, pre-yield send value preparation IS unified via
 * xr_chan_prepare_send.
 */

#ifndef XCHANNEL_OPS_H
#define XCHANNEL_OPS_H

#include "xchannel.h"
#include "xdeep_copy.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xisolate_api.h"

struct XrCoroutine;
struct XrayIsolate;

/* ========== Send-side deep copy ========== */

/* Deep copy a mutable value before it enters a channel buffer.
 * Copies into the isolate's shared GC so the value outlives the
 * sending coroutine. Returns the original value unchanged for
 * scalars and immutables. */
static inline XrValue xr_chan_prepare_send(struct XrayIsolate *isolate,
                                            XrValue value) {
    if (!XR_IS_PTR(value))
        return value;
    if (!xr_value_needs_copy(value))
        return value;
    return xr_deep_copy(isolate, value, xr_isolate_get_gc(isolate));
}

/* ========== Recv-side deep copy ========== */

/* Deep copy a mutable value received from a channel into the
 * receiver's coroutine-local GC.  Returns the original value
 * unchanged for scalars and immutables. */
static inline XrValue xr_chan_copy_recv(struct XrayIsolate *isolate,
                                         XrValue value,
                                         struct XrCoroutine *recv_coro) {
    if (!XR_IS_PTR(value))
        return value;
    if (!xr_value_needs_copy(value))
        return value;
    return xr_deep_copy_to_coro(isolate, value, recv_coro);
}

/* ========== tryRecv — non-blocking receive ========== */

/* Unified tryRecv: try buffer pop, then unbuffered rendezvous,
 * deep-copy on success, wake senders.
 *
 * out_value: receives the value on success (xr_null on failure)
 * recv_coro: target coroutine for deep copy (may be NULL for
 *            isolate-level GC fallback inside xr_deep_copy_to_coro)
 *
 * Returns true if a value was received, false otherwise. */
static inline bool xr_chan_try_recv(struct XrayIsolate *isolate,
                                     XrChannel *ch,
                                     XrValue *out_value,
                                     struct XrCoroutine *recv_coro) {
    XR_DCHECK(ch != NULL, "xr_chan_try_recv: NULL channel");
    XR_DCHECK(out_value != NULL, "xr_chan_try_recv: NULL out_value");

    bool ok;
    XrValue value = xr_channel_try_recv(ch, &ok);

    /* Unbuffered rendezvous: if buffer was empty, try to wake a blocked
     * sender from the runtime queue and take its value directly. */
    if (!ok) {
        XrCoroutine *sender = xr_runtime_wake_channel(isolate, ch, true);
        if (sender) {
            value = sender->send_value;
            ok = true;
        }
    }

    if (ok) {
        *out_value = xr_chan_copy_recv(isolate, value, recv_coro);
        /* Wake additional blocked senders now that a slot freed up */
        xr_runtime_wake_channel(isolate, ch, true);
        return true;
    }

    *out_value = xr_null();
    return false;
}

/* ========== trySend — non-blocking send ========== */

/* Unified trySend: deep-copy value, try buffer push, wake receivers
 * on success.  Returns true if the value was enqueued. */
static inline bool xr_chan_try_send(struct XrayIsolate *isolate,
                                     XrChannel *ch,
                                     XrValue value) {
    XR_DCHECK(ch != NULL, "xr_chan_try_send: NULL channel");

    value = xr_chan_prepare_send(isolate, value);
    bool success = xr_channel_try_send(ch, value);
    if (success) {
        xr_runtime_wake_channel(isolate, ch, false);
    }
    return success;
}

#endif  // XCHANNEL_OPS_H
