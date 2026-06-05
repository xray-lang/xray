/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_chan_ops.c - Dispatch helpers for channel timeouts and select
 *
 * Implements channel send/recv with timeout and the select-block
 * driver. Declarations live in xvm_dispatch_helpers.h.
 *
 * Owns:
 *   - vm_select_block        (multiplex blocking select)
 *   - vm_chan_send_timeout   (Channel send with deadline)
 *   - vm_chan_recv_timeout   (Channel recv with deadline)
 */

#include "xvm_dispatch_helpers.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../os/os_time.h"
#include "../runtime/value/xstruct_layout.h"
#include "xvm_checks.h"
#include "xdebug.h"
#include "../runtime/xray_debug_hooks.h"
#include "../runtime/xstrbuf.h"
#include "../runtime/object/xstringbuilder.h"

#include "../runtime/object/xjson.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/object/xrange.h"
#include "../base/xutf8.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_feedback.h"
#include "../coro/xcoro_pool.h"
#include "../coro/xtask.h"
#include "../coro/xdeep_copy.h"
#include <string.h>

XR_FUNC XrDispatchAction vm_select_block(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                         XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                         XrInstruction *pc) {
    int base_reg = GETARG_A(instr);
    int ch_count = GETARG_B(instr);
    int case_count = GETARG_C(instr);

    XrCoroutine *coro = vm_get_coro(vm_ctx);
    if (!coro)
        return XR_DISP_NEXT;

    XrWorker *worker = xr_current_worker();
    if (!worker)
        return XR_DISP_NEXT;
    XrRuntime *runtime = worker->p.runtime;
    if (runtime) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.select_block_count);
    }

    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "select: out of memory");
    }

    int case_slots = case_count > ch_count ? case_count : ch_count;
    if (case_slots <= 0) {
        case_slots = ch_count;
    }
    XrSelectStorage *storage = &ext->select_storage;
    XrSelectCase *cases = NULL;
    if (case_slots <= XR_SELECT_INLINE_CASES) {
        cases = storage->inline_cases;
        if (runtime) {
            xr_sched_metric_inc(runtime, &runtime->sched_stats.select_inline_alloc_count);
        }
    } else {
        if (storage->heap_capacity < case_slots) {
            XrSelectCase *new_cases =
                (XrSelectCase *) xr_malloc((size_t) case_slots * sizeof(XrSelectCase));
            if (!new_cases) {
                VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "select: out of memory");
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
        cases = storage->heap_cases;
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
        XrValue ch_val = base[base_reg + ci];
        if (!xr_value_is_channel(ch_val)) {
            sw->cases[ci].channel = NULL;
            continue;
        }
        XrChannel *ch = xr_value_to_channel(ch_val);
        sw->cases[ci].channel = ch;
        sw->cases[ci].is_send = false;
        sw->cases[ci].result_reg = base_reg + ci;
        sw->cases[ci].owner = coro;
        if (atomic_load(&ch->is_timer))
            timer_ch = ch;
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

    // Arm sleep timer so the worker wakes the coro when the timer
    // channel fires.  The tw_timer callback writes data to the buffer; when
    // the coro re-polls after wakeup, OP_CHAN_TRY_RECV will find it.
    // Clamp remaining to at least 1ms so that xr_bump_timers fires both the
    // tw_timer and the sleep timer on the next tick (handles after 0 case).
    if (timer_ch && !atomic_load_explicit(&timer_ch->timer_fired, memory_order_acquire)) {
        int64_t now_ms = xr_monotonic_ticks();
        int64_t elapsed = now_ms - timer_ch->timer_start_ticks;
        int64_t remaining = timer_ch->timer_timeout_ms - elapsed;
        if (remaining < 1)
            remaining = 1;
        if (worker->p.timer_wheel) {
            if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
                xr_twheel_cancel_timer(worker->p.timer_wheel, &coro->ext->timer);
                atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
            }
            xr_worker_add_sleep_timer(worker, coro, remaining);
        }
    }

    // Notify dist channels about entering select (subscribe for push model)
    XrChannelDistHooks *dhooks = isolate ? isolate->channel_dist_hooks : NULL;
    if (dhooks && dhooks->on_select_enter) {
        for (int ci = 0; ci < sw->case_count; ci++) {
            if (!sw->cases[ci].channel)
                continue;
            XrChannel *dch = (XrChannel *) sw->cases[ci].channel;
            if (dch->dist) {
                dhooks->on_select_enter(dch);
            }
        }
    }

    frame->pc = pc;
    xr_worker_block_select(worker, coro, NULL, sw->case_count);

    return XR_DISP_BLOCKED;
}

XR_FUNC XrDispatchAction vm_chan_send_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                              XrInstruction instr, XrValue *base,
                                              XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrValue ch_val = base[b];
    if (!xr_value_is_channel(ch_val)) {
        base[a] = xr_bool(false);
        return XR_DISP_NEXT;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    XrValue value = base[c];
    XrValue timeout_val = base[c + 1];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val))
        timeout_ms = XR_TO_INT(timeout_val);
    else if (XR_IS_FLOAT(timeout_val))
        timeout_ms = (int64_t) XR_TO_FLOAT(timeout_val);
    if (timeout_ms < 0)
        timeout_ms = 0;

    XrCoroutine *current = vm_get_coro(vm_ctx);
    if (current) {
        XrSlotRef result_slot = xr_slot_xvalue_ptr(&base[a]);
        XrCoroBlockResult resumed = xr_coro_chan_send_resume(current, result_slot);
        if (resumed.kind == XR_CORO_BLOCK_READY || resumed.kind == XR_CORO_BLOCK_TIMEOUT ||
            resumed.kind == XR_CORO_BLOCK_CLOSED) {
            return XR_DISP_NEXT;
        }
        XrCoroBlockResult result =
            xr_coro_chan_send(isolate, current, ch, value, result_slot, timeout_ms);
        if (result.kind == XR_CORO_BLOCK_READY || result.kind == XR_CORO_BLOCK_TIMEOUT ||
            result.kind == XR_CORO_BLOCK_CLOSED || result.kind == XR_CORO_BLOCK_NO_CORO) {
            return XR_DISP_NEXT;
        }
        if (result.kind == XR_CORO_BLOCK_BLOCKED) {
            frame->pc = pc - 1;
            return XR_DISP_BLOCKED;
        }
        base[a] = xr_bool(false);
        return XR_DISP_NEXT;
    }

    // Main thread: synchronous polling
    value = xr_chan_prepare_send(isolate, value);
    if (xr_channel_try_send(ch, value)) {
        base[a] = xr_bool(true);
        xr_runtime_wake_channel(isolate, ch, false);
        return XR_DISP_NEXT;
    }
    if (xr_channel_is_closed(ch) || timeout_ms <= 0) {
        base[a] = xr_bool(false);
        return XR_DISP_NEXT;
    }

    uint64_t start_ns = xr_time_monotonic_ns();
    while (1) {
        int64_t elapsed_ms = (int64_t) ((xr_time_monotonic_ns() - start_ns) / 1000000ULL);
        if (elapsed_ms >= timeout_ms) {
            base[a] = xr_bool(false);
            break;
        }
        if (xr_channel_try_send(ch, value)) {
            base[a] = xr_bool(true);
            xr_runtime_wake_channel(isolate, ch, false);
            break;
        }
        if (xr_channel_is_closed(ch)) {
            base[a] = xr_bool(false);
            break;
        }
        xr_thread_yield();
    }
    return XR_DISP_NEXT;
}

XR_FUNC XrDispatchAction vm_chan_recv_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                              XrInstruction instr, XrValue *base,
                                              XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrValue ch_val = base[b];
    if (!xr_value_is_channel(ch_val)) {
        base[a] = xr_null();
        base[a + 1] = xr_bool(false);
        return XR_DISP_NEXT;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    XrValue timeout_val = base[c];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val))
        timeout_ms = XR_TO_INT(timeout_val);
    else if (XR_IS_FLOAT(timeout_val))
        timeout_ms = (int64_t) XR_TO_FLOAT(timeout_val);

    XrCoroutine *current = vm_get_coro(vm_ctx);
    if (current) {
        XrSlotRef value_slot = xr_slot_xvalue_ptr(&base[a]);
        XrSlotRef ok_slot = xr_slot_xvalue_ptr(&base[a + 1]);
        XrCoroBlockResult resumed = xr_coro_chan_recv_resume(isolate, current, value_slot, ok_slot);
        if (resumed.kind == XR_CORO_BLOCK_READY || resumed.kind == XR_CORO_BLOCK_TIMEOUT ||
            resumed.kind == XR_CORO_BLOCK_CLOSED) {
            return XR_DISP_NEXT;
        }
        XrCoroBlockResult result =
            xr_coro_chan_recv(isolate, current, ch, value_slot, ok_slot, timeout_ms);
        if (result.kind == XR_CORO_BLOCK_READY || result.kind == XR_CORO_BLOCK_TIMEOUT ||
            result.kind == XR_CORO_BLOCK_CLOSED || result.kind == XR_CORO_BLOCK_NO_CORO) {
            return XR_DISP_NEXT;
        }
        if (result.kind == XR_CORO_BLOCK_BLOCKED) {
            frame->pc = pc - 1;
            return XR_DISP_BLOCKED;
        }
        base[a] = xr_null();
        base[a + 1] = xr_bool(false);
        return XR_DISP_NEXT;
    }

    // Main thread: synchronous polling
    XrValue recv_val;
    if (xr_chan_try_recv(isolate, ch, &recv_val, NULL)) {
        base[a] = recv_val;
        base[a + 1] = xr_bool(true);
        return XR_DISP_NEXT;
    }
    if (xr_channel_is_closed(ch) || timeout_ms <= 0) {
        base[a] = xr_null();
        base[a + 1] = xr_bool(false);
        return XR_DISP_NEXT;
    }

    uint64_t start_ns = xr_time_monotonic_ns();
    while (1) {
        int64_t elapsed_ms = (int64_t) ((xr_time_monotonic_ns() - start_ns) / 1000000ULL);
        if (elapsed_ms >= timeout_ms) {
            base[a] = xr_null();
            base[a + 1] = xr_bool(false);
            break;
        }
        if (xr_chan_try_recv(isolate, ch, &recv_val, NULL)) {
            base[a] = recv_val;
            base[a + 1] = xr_bool(true);
            break;
        }
        if (xr_channel_is_closed(ch)) {
            base[a] = xr_null();
            base[a + 1] = xr_bool(false);
            break;
        }
        xr_thread_yield();
    }
    return XR_DISP_NEXT;
}
