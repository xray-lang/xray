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
 * call dispatch, and runtime helpers.  This eliminates the class of
 * bugs where one path diverges from another (missing deep copy, missing
 * ok flag, missing unbuffered rendezvous, etc.).
 *
 * Blocking send/recv use the backend-neutral helpers in xblock.h.
 * This header stays focused on value movement and non-blocking fast paths.
 */

#ifndef XCHANNEL_OPS_H
#define XCHANNEL_OPS_H

#include "xchannel.h"
#include "xdeep_copy.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xshared.h"
#include "../runtime/gc/xalloc_unified.h"

struct XrCoroutine;
struct XrayIsolate;

/* ========== Send-side ownership ========== */

/* Send CONSUMES the caller's reference (XI_CHAN_SEND classifies its args
 * as consuming uses; the compiler dups beforehand when the caller still
 * needs the value afterwards). What enters the channel:
 *
 *   - scalars: the value itself, no RC involved.
 *   - shared refs (channel, atomic, ...): the caller's +1 transfers to
 *     the buffer and is handed to the receiver on delivery.
 *   - deep values (array, map, instance, ...): a coroutine-independent
 *     TRANSIT graph replaces the original; the caller's reference is
 *     released HERE — the single send-side consumption point.
 *
 * Re-entrant: a value that is already a TRANSIT root (a blocked send
 * being retried after suspension) passes through unchanged. */
static inline XrValue xr_chan_prepare_send(struct XrayIsolate *isolate, XrValue value) {
    if (!XR_IS_PTR(value))
        return value;
    XrGCHeader *obj = XR_VALUE_GCPTR(value);
    if (obj && XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT))
        return value; /* already prepared (blocked-send retry path) */
    if (!xr_value_needs_copy(value))
        return value;
    /* Zero-copy move fast path applies ONLY to coroutine-local (non-shared)
     * sources we uniquely own. Shared-by-pointer values must keep the original
     * copy-then-drop order: deep_copy increfs them and hands them out by
     * pointer, and the drop rebalances — dropping first then rc_destroy would
     * free a shared const value the receiver still aliases. drop_is_last is
     * mutating, so it is evaluated exactly once on each path. */
    bool coro_local = obj && !XR_GC_IS_SHARED(obj);
    if (coro_local && xr_obj_drop_is_last((XrObjHeader *) obj)) {
        /* Unique owner (move semantics): steal the buffer for self-contained
         * scalar arrays, else deep-copy. Either way free the source struct. */
        XrValue moved;
        if (xr_chan_try_move_array_to_transit(isolate, value, &moved)) {
            xr_coro_gc_rc_destroy(xr_current_coro_gc(), obj); /* free emptied source struct */
            return moved;
        }
        XrValue copied = xr_deep_copy_to_transit(isolate, value);
        xr_coro_gc_rc_destroy(xr_current_coro_gc(), obj);
        return copied;
    }
    if (coro_local) {
        /* Not the last reference (already dropped above): copy, no destroy. */
        return xr_deep_copy_to_transit(isolate, value);
    }
    /* Shared / pointer-shared values: original copy-then-drop order. */
    XrValue copied = xr_deep_copy_to_transit(isolate, value);
    if (obj && xr_obj_drop_is_last((XrObjHeader *) obj))
        xr_coro_gc_rc_destroy(xr_current_coro_gc(), obj);
    return copied;
}

/* A prepared send value that never entered the channel — try-send
 * failure, send on a closed channel, timeout/cancel of a blocked send —
 * still carries the reference that delivery would have handed to the
 * receiver. Release it exactly once here.
 *
 * Coro-heap values that pass through prepare untouched (e.g. strings)
 * are left alone: their reference balance is owner-thread business. */
static inline void xr_chan_abandon_send(XrValue prepared) {
    if (!XR_IS_PTR(prepared))
        return;
    XrGCHeader *obj = XR_VALUE_GCPTR(prepared);
    if (!obj)
        return;
    if (XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT)) {
        xr_chan_transit_release(prepared);
        return;
    }
    if (XR_GC_IS_SHARED(obj)) {
        /* Atomic refcount: safe from any thread. Managed objects
         * (channels) make drop_is_last a no-op by design. */
        if (xr_obj_drop_is_last((XrObjHeader *) obj))
            xr_shared_destroy(obj);
    }
}

/* ========== Recv-side deep copy ========== */

/* Deep copy a mutable value received from a channel into the
 * receiver's coroutine-local GC, then release the channel buffer's
 * transit reference (frees the transit graph).  Returns the original
 * value unchanged for scalars and immutables. */
static inline XrValue xr_chan_copy_recv(struct XrayIsolate *isolate, XrValue value,
                                        struct XrCoroutine *recv_coro) {
    if (!XR_IS_PTR(value))
        return value;
    if (!xr_value_needs_copy(value))
        return value;
    /* Zero-copy adopt: if the transit value is a uniquely-owned self-contained
     * scalar array, steal its buffer into the receiver heap instead of copying;
     * the helper releases the emptied transit struct on success. */
    XrValue adopted;
    if (xr_chan_try_adopt_array_from_transit(isolate, value, recv_coro, &adopted))
        return adopted;
    XrValue copied = xr_deep_copy_to_coro(isolate, value, recv_coro);
    xr_chan_transit_release(value);
    return copied;
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
static inline bool xr_chan_try_recv(struct XrayIsolate *isolate, XrChannel *ch, XrValue *out_value,
                                    struct XrCoroutine *recv_coro) {
    XR_DCHECK(ch != NULL, "xr_chan_try_recv: NULL channel");
    XR_DCHECK(out_value != NULL, "xr_chan_try_recv: NULL out_value");

    bool ok;
    XrValue value = xr_channel_try_recv(ch, &ok);

    if (ok) {
        *out_value = xr_chan_copy_recv(isolate, value, recv_coro);
        return true;
    }

    *out_value = xr_null();
    return false;
}

/* ========== trySend — non-blocking send ========== */

/* Unified trySend: deep-copy value, try buffer push, wake receivers
 * on success.  Returns true if the value was enqueued. The argument is
 * consumed either way (single-shot semantics): on failure the prepared
 * value is released. */
static inline bool xr_chan_try_send(struct XrayIsolate *isolate, XrChannel *ch, XrValue value) {
    XR_DCHECK(ch != NULL, "xr_chan_try_send: NULL channel");

    value = xr_chan_prepare_send(isolate, value);
    if (xr_channel_try_send(ch, value))
        return true;
    xr_chan_abandon_send(value);
    return false;
}

#endif  // XCHANNEL_OPS_H
