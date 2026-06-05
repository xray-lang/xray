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
#include <string.h>

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/object/xarray.h"
#include "../runtime/xisolate_internal.h"
#include "xchannel_ops.h"
#include "xcoroutine.h"
#include "xdeep_copy.h"
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
            if (slot.type_id != XR_REP_TAGGED)
                return NULL;
            if (!slot.base)
                return NULL;
            return (XrValue *) ((uint8_t *) slot.base + slot.offset);
        default:
            return NULL;
    }
}

bool xr_slot_store_value(XrSlotRef slot, XrValue value) {
    switch (slot.kind) {
        case XR_SLOT_NONE:
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *(XrValue *) slot.base = value;
            return true;
        case XR_SLOT_AOT_FRAME_OFFSET:
        case XR_SLOT_JIT_SUSPEND: {
            if (!slot.base)
                return false;
            void *addr = (uint8_t *) slot.base + slot.offset;
            if (slot.type_id == XR_REP_I64) {
                *(int64_t *) addr = XR_TO_INT(value);
                return true;
            }
            if (slot.type_id == XR_REP_F64) {
                *(double *) addr = XR_TO_FLOAT(value);
                return true;
            }
            *(XrValue *) addr = value;
            return true;
        }
        default:
            return false;
    }
}

bool xr_slot_load_value(XrSlotRef slot, XrValue *out_value) {
    if (!out_value)
        return false;
    switch (slot.kind) {
        case XR_SLOT_NONE:
            *out_value = xr_null();
            return true;
        case XR_SLOT_XVALUE_PTR:
            if (!slot.base)
                return false;
            *out_value = *(XrValue *) slot.base;
            return true;
        case XR_SLOT_AOT_FRAME_OFFSET:
        case XR_SLOT_JIT_SUSPEND: {
            if (!slot.base)
                return false;
            void *addr = (uint8_t *) slot.base + slot.offset;
            if (slot.type_id == XR_REP_I64) {
                *out_value = xr_int(*(int64_t *) addr);
                return true;
            }
            if (slot.type_id == XR_REP_F64) {
                *out_value = xr_float(*(double *) addr);
                return true;
            }
            *out_value = *(XrValue *) addr;
            return true;
        }
        default:
            return false;
    }
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
        XrValue value = xr_null();
        (void) xr_slot_load_value(value_slot, &value);
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
        coro->recv_slot_ref = value_slot;
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
    if (worker) {
        xr_worker_cancel_timer(worker, coro);
        return;
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

XrValue xr_coro_await_result_value(XrayIsolate *isolate, XrCoroutine *dst_coro, XrTask *task,
                                   bool discard_result) {
    if (discard_result || !task)
        return xr_null();

    XrValue result = task->result;
    if (dst_coro && isolate && xr_value_needs_copy(result)) {
        result = xr_deep_copy_to_coro(isolate, result, dst_coro);
        task->result = result;
    }
    return result;
}

static bool await_store_result(XrayIsolate *isolate, XrCoroutine *coro, XrTask *task,
                               XrSlotRef result_slot, bool discard_result, XrValue *out_value) {
    XrValue result = xr_coro_await_result_value(isolate, coro, task, discard_result);
    if (out_value)
        *out_value = result;
    return xr_slot_store_value(result_slot, result);
}

XrCoroBlockResult xr_coro_await_task_resume_slot(XrayIsolate *isolate, XrCoroutine *coro,
                                                 XrTask *task, XrSlotRef result_slot,
                                                 bool discard_result) {
    XrCoroBlockResult result = xr_coro_await_task_resume(coro, task);
    if (result.kind == XR_CORO_BLOCK_READY) {
        XrValue value = xr_null();
        if (!await_store_result(isolate, coro, task, result_slot, discard_result, &value))
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
        result.value = value;
    } else if (result.kind == XR_CORO_BLOCK_TIMEOUT || result.kind == XR_CORO_BLOCK_CLOSED) {
        if (!xr_slot_store_value(result_slot, xr_null()))
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }
    return result;
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

XrCoroBlockResult xr_coro_await_task_slot(XrayIsolate *isolate, XrCoroutine *coro, XrTask *task,
                                          XrSlotRef result_slot, int64_t timeout_ms,
                                          bool discard_result) {
    XrCoroBlockResult result = xr_coro_await_task(coro, task, timeout_ms);
    if (result.kind == XR_CORO_BLOCK_READY) {
        XrValue value = xr_coro_await_result_value(isolate, coro, task, discard_result);
        if (!xr_slot_store_value(result_slot, value))
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
        result.value = value;
    } else if (result.kind == XR_CORO_BLOCK_TIMEOUT || result.kind == XR_CORO_BLOCK_CLOSED ||
               result.kind == XR_CORO_BLOCK_NO_CORO) {
        if (!xr_slot_store_value(result_slot, xr_null()))
            return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }
    return result;
}

XrCoroBlockResult xr_coro_sleep(XrCoroutine *coro, int64_t milliseconds) {
    if (milliseconds <= 0) {
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }
    if (!coro) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }

    ext->yield_info.wait_fd = -1;
    ext->yield_info.wait_events = 0;
    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_SLEEP >> XR_CORO_WAIT_SHIFT);

    XrWorker *worker = xr_current_worker();
    if (!worker || !worker->p.timer_wheel) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    xr_worker_add_sleep_timer(worker, coro, milliseconds);
    if (!atomic_load_explicit(&ext->timer_active, memory_order_relaxed)) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }
    return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
}

static XrSelectCase *coro_select_alloc_cases(XrRuntime *runtime, XrSelectStorage *storage,
                                             int case_slots) {
    if (case_slots <= XR_SELECT_INLINE_CASES) {
        if (runtime) {
            xr_sched_metric_inc(runtime, &runtime->sched_stats.select_inline_alloc_count);
        }
        return storage->inline_cases;
    }

    if (storage->heap_capacity < case_slots) {
        XrSelectCase *new_cases =
            (XrSelectCase *) xr_malloc((size_t) case_slots * sizeof(XrSelectCase));
        if (!new_cases) {
            return NULL;
        }
        if (storage->heap_cases) {
            xr_free(storage->heap_cases);
        }
        storage->heap_cases = new_cases;
        storage->heap_capacity = case_slots;
        if (runtime) {
            xr_sched_metric_inc(runtime, &runtime->sched_stats.select_heap_alloc_count);
        }
    }
    return storage->heap_cases;
}

static void coro_select_notify_enter(XrayIsolate *isolate, XrSelectWait *sw) {
    XrChannelDistHooks *dhooks =
        isolate ? (XrChannelDistHooks *) isolate->channel_dist_hooks : NULL;
    if (!dhooks || !dhooks->on_select_enter) {
        return;
    }

    for (int ci = 0; ci < sw->case_count; ci++) {
        XrChannel *ch = (XrChannel *) sw->cases[ci].channel;
        if (ch && ch->dist) {
            dhooks->on_select_enter(ch);
        }
    }
}

static void coro_select_arm_timer(XrWorker *worker, XrCoroutine *coro, XrChannel *timer_ch) {
    if (!worker || !coro || !timer_ch ||
        atomic_load_explicit(&timer_ch->timer_fired, memory_order_acquire)) {
        return;
    }

    int64_t now_ms = xr_monotonic_ticks();
    int64_t elapsed = now_ms - timer_ch->timer_start_ticks;
    int64_t remaining = timer_ch->timer_timeout_ms - elapsed;
    if (remaining < 1) {
        remaining = 1;
    }
    if (worker->p.timer_wheel) {
        coro_cancel_owned_timer(coro);
        xr_worker_add_sleep_timer(worker, coro, remaining);
    }
}

XrCoroBlockResult xr_coro_select_block(XrayIsolate *isolate, XrCoroutine *coro,
                                       const XrValue *channel_values, int ch_count, int case_count,
                                       int result_reg_base) {
    if (!coro) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    XrWorker *worker = xr_current_worker();
    if (!worker) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }
    XrRuntime *runtime = worker->p.runtime;
    if (runtime) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.select_block_count);
    }

    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }

    int case_slots = case_count > ch_count ? case_count : ch_count;
    if (case_slots <= 0) {
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }

    XrSelectStorage *storage = &ext->select_storage;
    XrSelectCase *cases = coro_select_alloc_cases(runtime, storage, case_slots);
    if (!cases) {
        return block_result(XR_CORO_BLOCK_ERROR, xr_null(), false);
    }

    memset(cases, 0, (size_t) case_slots * sizeof(XrSelectCase));
    memset(&storage->wait, 0, sizeof(storage->wait));

    XrSelectWait *sw = &storage->wait;
    sw->cases = cases;
    sw->case_count = ch_count < case_count ? ch_count : case_count;
    sw->timer_channel = NULL;
    sw->timer_case_index = -1;
    atomic_store(&sw->triggered, false);

    XrChannel *timer_ch = NULL;
    for (int ci = 0; ci < ch_count && ci < sw->case_count; ci++) {
        XrValue ch_val = channel_values ? channel_values[ci] : xr_null();
        if (!xr_value_is_channel(ch_val)) {
            sw->cases[ci].channel = NULL;
            continue;
        }

        XrChannel *ch = xr_value_to_channel(ch_val);
        sw->cases[ci].channel = ch;
        sw->cases[ci].is_send = false;
        sw->cases[ci].result_reg = result_reg_base + ci;
        sw->cases[ci].owner = coro;
        if (atomic_load(&ch->is_timer)) {
            timer_ch = ch;
        }
    }

    sw->timer_channel = timer_ch;
    for (int ci = 0; ci < sw->case_count; ci++) {
        if (sw->cases[ci].channel == timer_ch) {
            sw->timer_case_index = ci;
            break;
        }
    }

    coro->select_wait = sw;
    coro->select_ready_case = -1;
    coro_select_arm_timer(worker, coro, timer_ch);
    coro_select_notify_enter(isolate, sw);

    xr_worker_block_select(worker, coro, NULL, sw->case_count);
    return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
}

XrCoroBlockResult xr_coro_scope_exit(XrCoroutine *coro, uint8_t scope_mode) {
    if (!coro) {
        return block_result(XR_CORO_BLOCK_NO_CORO, xr_null(), false);
    }

    XrScopeContext *scope = coro->current_scope;
    if (!scope) {
        return block_result(XR_CORO_BLOCK_READY, xr_null(), true);
    }

    if (atomic_load(&scope->count) > 0) {
        xr_coro_set_wait_reason(coro, XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT);
        return block_result(XR_CORO_BLOCK_BLOCKED, xr_null(), false);
    }
    atomic_store(&coro->wait_count, 0);

    if (scope_mode == XR_SCOPE_LINKED && !XR_IS_NULL(scope->first_error)) {
        XrValue err = scope->first_error;
        bool err_is_value = scope->first_error_is_value;
        coro->current_scope = scope->parent;
        xr_free(scope);
        return block_result(XR_CORO_BLOCK_ERROR, err, err_is_value);
    }

    XrValue supervisor_result = xr_null();
    if (scope_mode == XR_SCOPE_SUPERVISOR) {
        if (scope->errors && scope->errors->length > 0) {
            supervisor_result = xr_value_from_array(scope->errors);
        } else {
            XrArray *empty = xr_array_new(coro);
            supervisor_result = empty ? xr_value_from_array(empty) : xr_null();
        }
    }

    coro->current_scope = scope->parent;
    xr_free(scope);
    return block_result(XR_CORO_BLOCK_READY, supervisor_result, true);
}
