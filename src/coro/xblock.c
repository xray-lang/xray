/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xblock.c - Backend-neutral coroutine blocking helpers
 */

#include "xblock.h"

#include <stdatomic.h>
#include <stdint.h>

#include "../base/xchecks.h"
#include "xchannel_ops.h"
#include "xcoroutine.h"
#include "xtask.h"
#include "xworker.h"
#include "xyieldable.h"

static inline XrCoroBlockResult block_result(XrCoroBlockKind kind, XrValue value, bool ok) {
    XrCoroBlockResult result = {kind, value, ok};
    return result;
}

XrValue *xr_slot_value_address(XrSlotRef slot) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return NULL;
        case XR_SLOT_XVALUE_PTR:
            return (XrValue *) slot.base;
        case XR_SLOT_AOT_FRAME_OFFSET:
        case XR_SLOT_JIT_SUSPEND:
            if (!slot.base)
                return NULL;
            return (XrValue *) ((uint8_t *) slot.base + slot.offset);
        default:
            return NULL;
    }
}

bool xr_slot_store_value(XrSlotRef slot, XrValue value) {
    XrValue *addr = xr_slot_value_address(slot);
    if (!addr)
        return slot.kind == XR_SLOT_NONE;
    *addr = value;
    return true;
}

static void coro_finish_resume(XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "coro_finish_resume: NULL coro");
    xr_coro_resume_store(coro, XR_RESUME_OK);
    atomic_store_explicit(&coro->wait_channel, NULL, memory_order_relaxed);
    coro->channel_deadline = 0;
}

static void coro_arm_timeout(XrCoroutine *coro, int64_t timeout_ms) {
    XR_DCHECK(coro != NULL, "coro_arm_timeout: NULL coro");
    XR_DCHECK(timeout_ms > 0, "coro_arm_timeout: non-positive timeout");

    coro->channel_deadline = xr_monotonic_ticks() + timeout_ms;
    XrWorker *worker = xr_current_worker();
    if (!worker)
        return;

    xr_worker_add_sleep_timer(worker, coro, timeout_ms);
    XrRuntime *runtime = worker->p.runtime;
    if (runtime) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.timeout_event_block_count);
    }
}

XrCoroBlockResult xr_coro_chan_send_resume(XrCoroutine *coro, XrSlotRef result_slot) {
    if (!coro)
        return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);

    int resume_status = xr_coro_resume_load(coro);
    if (resume_status == XR_RESUME_CHANNEL) {
        coro_finish_resume(coro);
        xr_slot_store_value(result_slot, xr_bool(true));
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }
    if (resume_status == XR_RESUME_CHANNEL_CLOSED) {
        coro_finish_resume(coro);
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (resume_status == XR_RESUME_TIMEOUT) {
        coro_finish_resume(coro);
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);
}

XrCoroBlockResult xr_coro_chan_recv_resume(XrayIsolate *isolate, XrCoroutine *coro,
                                           XrSlotRef value_slot, XrSlotRef ok_slot) {
    if (!coro)
        return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);

    int resume_status = xr_coro_resume_load(coro);
    if (resume_status == XR_RESUME_CHANNEL) {
        XrValue *value_addr = xr_slot_value_address(value_slot);
        XrValue value = value_addr ? *value_addr : xr_null();
        value = xr_chan_copy_recv(isolate, value, coro);
        coro_finish_resume(coro);
        xr_slot_store_value(value_slot, value);
        xr_slot_store_value(ok_slot, xr_bool(true));
        return block_result(XR_CORO_BLOCK_READY, value, true);
    }
    if (resume_status == XR_RESUME_CHANNEL_CLOSED) {
        coro_finish_resume(coro);
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (resume_status == XR_RESUME_TIMEOUT) {
        coro_finish_resume(coro);
        xr_slot_store_value(value_slot, xr_null());
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);
}

XrCoroBlockResult xr_coro_chan_send(XrayIsolate *isolate, XrCoroutine *coro, XrChannel *ch,
                                    XrValue value, XrSlotRef result_slot, int64_t timeout_ms) {
    XR_DCHECK(ch != NULL, "xr_coro_chan_send: NULL channel");

    value = xr_chan_prepare_send(isolate, value);

    if (timeout_ms == 0) {
        if (xr_channel_try_send(ch, value)) {
            xr_slot_store_value(result_slot, xr_bool(true));
            return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
        }
        if (xr_channel_is_closed(ch)) {
            xr_slot_store_value(result_slot, xr_bool(false));
            return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
        }
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    XrChanResult chan_result = xr_channel_send(ch, value, coro);
    if (chan_result == XR_CHAN_OK) {
        xr_slot_store_value(result_slot, xr_bool(true));
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }
    if (chan_result == XR_CHAN_CLOSED) {
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (chan_result == XR_CHAN_NO_CORO) {
        xr_slot_store_value(result_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }
    if (chan_result == XR_CHAN_BLOCK) {
        if (timeout_ms > 0) {
            coro_arm_timeout(coro, timeout_ms);
        }
        return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
    }

    xr_slot_store_value(result_slot, xr_bool(false));
    return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
}

XrCoroBlockResult xr_coro_chan_recv(XrayIsolate *isolate, XrCoroutine *coro, XrChannel *ch,
                                    XrSlotRef value_slot, XrSlotRef ok_slot, int64_t timeout_ms) {
    XR_DCHECK(ch != NULL, "xr_coro_chan_recv: NULL channel");

    XrValue *recv_addr = xr_slot_value_address(value_slot);
    if (coro) {
        coro->recv_slot = recv_addr;
    }

    if (timeout_ms == 0) {
        XrValue recv_val;
        if (xr_chan_try_recv(isolate, ch, &recv_val, coro)) {
            xr_slot_store_value(value_slot, recv_val);
            xr_slot_store_value(ok_slot, xr_bool(true));
            return block_result(XR_CORO_BLOCK_READY, recv_val, true);
        }
        if (xr_channel_is_closed(ch)) {
            xr_slot_store_value(value_slot, xr_null());
            xr_slot_store_value(ok_slot, xr_bool(false));
            return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
        }
        xr_slot_store_value(value_slot, xr_null());
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    XrValue recv_val;
    XrChanResult chan_result = xr_channel_recv(ch, &recv_val, coro);
    if (chan_result == XR_CHAN_OK) {
        recv_val = xr_chan_copy_recv(isolate, recv_val, coro);
        xr_slot_store_value(value_slot, recv_val);
        xr_slot_store_value(ok_slot, xr_bool(true));
        return block_result(XR_CORO_BLOCK_READY, recv_val, true);
    }
    if (chan_result == XR_CHAN_CLOSED) {
        xr_slot_store_value(value_slot, xr_null());
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (chan_result == XR_CHAN_NO_CORO) {
        xr_slot_store_value(value_slot, xr_null());
        xr_slot_store_value(ok_slot, xr_bool(false));
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }
    if (chan_result == XR_CHAN_BLOCK) {
        if (timeout_ms > 0) {
            coro_arm_timeout(coro, timeout_ms);
        }
        return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
    }

    xr_slot_store_value(value_slot, xr_null());
    xr_slot_store_value(ok_slot, xr_bool(false));
    return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
}

static void coro_cancel_owned_timer(XrCoroutine *coro) {
    if (!coro || !coro->ext ||
        !atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed))
        return;

    XrWorker *worker = xr_current_worker();
    if (worker && worker->p.timer_wheel) {
        xr_twheel_cancel_timer(worker->p.timer_wheel, &coro->ext->timer);
    }
    atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
}

XrCoroBlockResult xr_coro_await_task_resume(XrCoroutine *coro, XrTask *task) {
    if (!coro)
        return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);

    if (xr_coro_resume_load(coro) == XR_RESUME_TIMEOUT) {
        coro_finish_resume(coro);
        if (task) {
            atomic_store_explicit((_Atomic(XrCoroutine *) *) &task->waiter, NULL,
                                  memory_order_relaxed);
        }
        atomic_store_explicit(&coro->await_task, NULL, memory_order_relaxed);
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }

    if (task &&
        atomic_load_explicit(&task->await_state, memory_order_acquire) == XR_AWAIT_RESOLVED) {
        coro_cancel_owned_timer(coro);
        atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
        atomic_store_explicit(&coro->await_task, NULL, memory_order_relaxed);
        return block_result(XR_CORO_BLOCK_READY, task->result, true);
    }

    return block_result(XR_CORO_BLOCK_NOT_RESUMED, xr_null(), false);
}

XrCoroBlockResult xr_coro_await_task(XrCoroutine *coro, XrTask *task, int64_t timeout_ms) {
    XR_DCHECK(task != NULL, "xr_coro_await_task: NULL task");

    uint8_t task_state = atomic_load_explicit(&task->state, memory_order_acquire);
    if (task_state == XR_TASK_COMPLETED) {
        return block_result(XR_CORO_BLOCK_READY, task->result, true);
    }
    if (task_state == XR_TASK_FAILED || task_state == XR_TASK_CANCELLED) {
        return block_result(XR_CORO_BLOCK_CLOSED, xr_null(), false);
    }
    if (timeout_ms == 0) {
        return block_result(XR_CORO_BLOCK_TIMEOUT, xr_null(), false);
    }
    if (!coro) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    atomic_store_explicit((_Atomic int *) &task->waiter_index, -1, memory_order_relaxed);
    atomic_store_explicit((_Atomic(XrCoroutine *) *) &task->waiter, coro, memory_order_release);

    int expected = XR_AWAIT_NONE;
    if (atomic_compare_exchange_strong_explicit(&task->await_state, &expected, XR_AWAIT_WAITING,
                                                memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit(&coro->await_task, task, memory_order_release);
        xr_coro_set_wait_reason(coro, XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
        if (timeout_ms > 0) {
            coro_arm_timeout(coro, timeout_ms);
        }
        return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
    }

    if (expected == XR_AWAIT_WAITING) {
        atomic_store_explicit(&coro->await_task, task, memory_order_release);
        xr_coro_set_wait_reason(coro, XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
        if (timeout_ms > 0) {
            coro_arm_timeout(coro, timeout_ms);
        }
        return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
    }

    XR_CHECK(expected == XR_AWAIT_RESOLVED, "await: unexpected await state");
    atomic_store_explicit((_Atomic(XrCoroutine *) *) &task->waiter, NULL, memory_order_relaxed);
    atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
    return block_result(XR_CORO_BLOCK_READY, task->result, true);
}
