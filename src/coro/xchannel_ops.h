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
#include "../base/xchecks.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/value/xtransfer_mode.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xshared.h"
#include "../runtime/mem/xalloc_unified.h"
#include "../runtime/mem/xsystem_heap.h"

struct XrCoroutine;
struct XrVMRuntime;

/* ========== Send-side ownership ========== */

static inline bool xr_chan_value_is_owned_message(XrValue value) {
    return XR_IS_PTR(value) && XR_VALUE_GCPTR(value) && XR_OBJ_IS_OWNED(XR_VALUE_GCPTR(value));
}

/* Send CONSUMES the caller's reference (XI_CHAN_SEND classifies its args
 * as consuming uses; the compiler dups beforehand when the caller still
 * needs the value afterwards). What enters the channel:
 *
 *   - scalars: the value itself, no RC involved.
 *   - shared refs (channel, atomic, ...): the caller's +1 transfers to
 *     the buffer and is handed to the receiver on delivery.
 *   - deep values (array, map, instance, ...): a coroutine-independent owned
 *     message root replaces the original; the caller's reference is
 *     released HERE — the single send-side consumption point.
 *
 * Channel transport never accepts TRANSIT roots. A TRANSIT value reaching this
 * helper means a caller bypassed the fixed inline/owned/shared transport
 * contract and must be fixed at the producer, not papered over here. */
static inline XrValue xr_chan_prepare_send_core(XrRuntimeCore *core, XrValue value) {
    if (!XR_IS_PTR(value))
        return value;
    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj)
        return value;
    XR_CHECK(!XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT),
             "channel send: TRANSIT payload is not a valid channel transport");
    if (XR_OBJ_IS_SHARED(obj))
        return value; /* shared frozen/sync identity: transfer the caller's reference */
    if (XR_OBJ_IS_OWNED(obj))
        return value; /* owned-message root: move the owner token, not the graph */
    if (!xr_value_needs_copy(value))
        return value;
    /* drop_is_last is mutating, so evaluate it exactly once on each path. */
    bool coro_local = obj && !XR_OBJ_IS_SHARED(obj);
    if (coro_local && xr_obj_drop_is_last((XrObjHeader *) obj)) {
        XrValue copied = xr_deep_copy_explicit_to_storage_core(core, value, XR_OBJ_STORAGE_OWNED);
        xr_coro_heap_destroy_obj(xr_current_coro_heap(), obj);
        return copied;
    }
    if (coro_local) {
        /* Not the last reference (already dropped above): copy, no destroy. */
        return xr_deep_copy_explicit_to_storage_core(core, value, XR_OBJ_STORAGE_OWNED);
    }
    XrValue copied = xr_deep_copy_explicit_to_storage_core(core, value, XR_OBJ_STORAGE_OWNED);
    if (obj && xr_obj_drop_is_last((XrObjHeader *) obj))
        xr_owned_destroy_core(core, obj);
    return copied;
}

static inline XrValue xr_chan_prepare_send_transfer_core(XrRuntimeCore *core, XrValue value,
                                                         uint8_t mode) {
    if (mode == XR_TRANSFER_MOVE)
        return xr_chan_prepare_send_core(core, value);
    if (!XR_IS_PTR(value))
        return value;

    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj)
        return value;
    XR_CHECK(!XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT),
             "channel send transfer: TRANSIT payload is not a valid channel transport");
    if (XR_OBJ_IS_OWNED(obj))
        return mode == XR_TRANSFER_COPY
                   ? xr_deep_copy_explicit_to_storage_core(core, value, XR_OBJ_STORAGE_OWNED)
                   : value;
    if (!xr_value_needs_copy(value)) {
        if (XR_OBJ_IS_SHARED(obj))
            xr_shared_retain(obj);
        return value;
    }
    if (mode == XR_TRANSFER_COPY)
        return xr_deep_copy_explicit_to_storage_core(core, value, XR_OBJ_STORAGE_OWNED);
    if (XR_OBJ_IS_SHARED(obj)) {
        xr_shared_retain(obj);
        return value;
    }
    return xr_deep_copy_explicit_to_storage_core(core, value, XR_OBJ_STORAGE_OWNED);
}

static inline XrValue xr_chan_prepare_send(struct XrVMRuntime *isolate, XrValue value) {
    return xr_chan_prepare_send_core(isolate ? xr_isolate_get_runtime_core(isolate) : NULL, value);
}

static inline XrValue xr_chan_prepare_send_transfer(struct XrVMRuntime *isolate, XrValue value,
                                                    uint8_t mode) {
    return xr_chan_prepare_send_transfer_core(isolate ? xr_isolate_get_runtime_core(isolate) : NULL,
                                              value, mode);
}

/* A prepared send value that never entered the channel — try-send
 * failure, send on a closed channel, timeout/cancel of a blocked send —
 * still carries the reference that delivery would have handed to the
 * receiver. Release it exactly once here.
 *
 * Coro-heap values that pass through prepare untouched (e.g. strings)
 * are left alone: their reference balance is owner-thread business. */
static inline void xr_chan_abandon_send_core(XrRuntimeCore *core, XrValue prepared) {
    if (!XR_IS_PTR(prepared))
        return;
    XrObjHeader *obj = XR_VALUE_GCPTR(prepared);
    if (!obj)
        return;
    XR_CHECK(!XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT),
             "channel abandon: TRANSIT payload reached channel transport");
    if (XR_OBJ_IS_OWNED(obj)) {
        if (xr_obj_drop_is_last(obj))
            xr_owned_destroy_core(core, obj);
        return;
    }
    if (XR_OBJ_IS_SHARED(obj)) {
        /* Atomic refcount: safe from any thread. Managed objects
         * (channels) make drop_is_last a no-op by design. */
        if (xr_obj_drop_is_last((XrObjHeader *) obj))
            xr_shared_destroy_core(core, obj);
    }
}

/* ========== Recv-side deep copy ========== */

/* Deliver a channel value to the receiver. Owned message roots move through
 * unchanged. Scalars and immutable/shared handles are returned unchanged.
 * Channel queues must not contain TRANSIT roots. */
static inline XrValue xr_chan_copy_recv_core(XrRuntimeCore *core, XrValue value,
                                             struct XrCoroutine *recv_coro) {
    if (!XR_IS_PTR(value))
        return value;
    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj)
        return value;
    XR_CHECK(!XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT),
             "channel recv: TRANSIT payload reached channel transport");
    if (xr_chan_value_is_owned_message(value))
        return value;
    if (!xr_value_needs_copy(value))
        return value;
    return xr_deep_copy_to_coro_core(core, value, recv_coro);
}

static inline XrValue xr_chan_copy_recv(struct XrVMRuntime *isolate, XrValue value,
                                        struct XrCoroutine *recv_coro) {
    return xr_chan_copy_recv_core(isolate ? xr_isolate_get_runtime_core(isolate) : NULL, value,
                                  recv_coro);
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
static inline bool xr_chan_try_recv_core(XrRuntimeCore *core, XrChannel *ch, XrValue *out_value,
                                         struct XrCoroutine *recv_coro) {
    XR_DCHECK(ch != NULL, "xr_chan_try_recv: NULL channel");
    XR_DCHECK(out_value != NULL, "xr_chan_try_recv: NULL out_value");

    bool ok;
    XrValue value = xr_channel_try_recv(ch, &ok);

    if (ok) {
        *out_value = xr_chan_copy_recv_core(core, value, recv_coro);
        return true;
    }

    *out_value = xr_null();
    return false;
}

static inline bool xr_chan_try_recv(struct XrVMRuntime *isolate, XrChannel *ch, XrValue *out_value,
                                    struct XrCoroutine *recv_coro) {
    return xr_chan_try_recv_core(isolate ? xr_isolate_get_runtime_core(isolate) : NULL, ch,
                                 out_value, recv_coro);
}

/* ========== trySend — non-blocking send ========== */

/* Unified trySend: deep-copy value, try buffer push, wake receivers
 * on success.  Returns true if the value was enqueued. The argument is
 * consumed either way (single-shot semantics): on failure the prepared
 * value is released. */
static inline bool xr_chan_try_send_core(XrRuntimeCore *core, XrChannel *ch, XrValue value) {
    XR_DCHECK(ch != NULL, "xr_chan_try_send: NULL channel");

    value = xr_chan_prepare_send_core(core, value);
    if (xr_channel_try_send(ch, value))
        return true;
    xr_chan_abandon_send_core(core, value);
    return false;
}

static inline bool xr_chan_try_send_transfer_core(XrRuntimeCore *core, XrChannel *ch, XrValue value,
                                                  uint8_t mode) {
    XR_DCHECK(ch != NULL, "xr_chan_try_send_transfer: NULL channel");

    value = xr_chan_prepare_send_transfer_core(core, value, mode);
    if (xr_channel_try_send(ch, value))
        return true;
    xr_chan_abandon_send_core(core, value);
    return false;
}

static inline bool xr_chan_try_send(struct XrVMRuntime *isolate, XrChannel *ch, XrValue value) {
    return xr_chan_try_send_core(isolate ? xr_isolate_get_runtime_core(isolate) : NULL, ch, value);
}

static inline bool xr_chan_try_send_transfer(struct XrVMRuntime *isolate, XrChannel *ch,
                                             XrValue value, uint8_t mode) {
    return xr_chan_try_send_transfer_core(isolate ? xr_isolate_get_runtime_core(isolate) : NULL, ch,
                                          value, mode);
}

#endif  // XCHANNEL_OPS_H
