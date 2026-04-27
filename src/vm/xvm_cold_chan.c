/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_cold_chan.c - Cold-path implementations for channel timeouts
 *                   and select multiplex
 *
 * Holds the noinline bodies for the channel send-with-timeout /
 * recv-with-timeout pair and the select-block driver. Function
 * declarations live in xvm_cold_paths.h.
 *
 * Owns:
 *   - vm_select_block        (multiplex blocking select)
 *   - vm_chan_send_timeout   (Channel send with deadline)
 *   - vm_chan_recv_timeout   (Channel recv with deadline)
 */

#include "xvm_cold_paths.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../os/os_time.h"
#include "../runtime/value/xstruct_layout.h"
#include "xvm_checks.h"
#include "xdebug.h"
#include "../runtime/xray_debug_hooks.h"
#include "../runtime/xstrbuf.h"
#include "../runtime/object/xstringbuilder.h"
#include "../runtime/object/xslice.h"
#include "../runtime/object/xjson.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/object/xrange.h"
#include "../runtime/object/xutf8.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_feedback.h"
#include "../coro/xcoro_pool.h"
#include "../coro/xtask.h"
#include "../coro/xdeep_copy.h"

XR_NOINLINE int vm_select_block(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                                XrValue *base, XrBcCallFrame *frame, XrInstruction *pc) {
    int base_reg = GETARG_A(instr);
    int ch_count = GETARG_B(instr);
    int case_count = GETARG_C(instr);

    XrCoroutine *coro = vm_cold_get_coro(vm_ctx);
    if (!coro)
        return VM_COLD_BREAK;

    XrWorker *worker = xr_current_worker();
    if (!worker)
        return VM_COLD_BREAK;

    void **channels = xr_malloc(ch_count * sizeof(void *));
    XrChannel *timer_ch = NULL;
    int valid_count = 0;
    (void) valid_count;

    for (int ci = 0; ci < ch_count; ci++) {
        XrValue ch_val = base[base_reg + ci];
        if (!xr_value_is_channel(ch_val)) {
            channels[ci] = NULL;
            continue;
        }
        XrChannel *ch = xr_value_to_channel(ch_val);
        channels[ci] = ch;
        valid_count++;
        if (atomic_load(&ch->is_timer))
            timer_ch = ch;
    }

    XrSelectWait *sw = xr_malloc(sizeof(XrSelectWait));
    if (!sw) {
        xr_free(channels);
        VM_COLD_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "select: out of memory");
    }
    sw->cases = xr_malloc(case_count * sizeof(XrSelectCase));
    if (!sw->cases) {
        xr_free(sw);
        xr_free(channels);
        VM_COLD_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "select: out of memory");
    }

    for (int ci = 0; ci < ch_count && ci < case_count; ci++) {
        sw->cases[ci].channel = channels[ci];
        sw->cases[ci].is_send = false;
        sw->cases[ci].result_reg = base_reg + ci;
        sw->cases[ci].bucket_next = NULL;
        sw->cases[ci].owner = coro;
    }
    sw->case_count = ch_count < case_count ? ch_count : case_count;
    sw->timer_channel = timer_ch;
    sw->timer_case_index = -1;
    atomic_store(&sw->triggered, false);

    for (int ci = 0; ci < ch_count; ci++) {
        if (channels[ci] == timer_ch) {
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
        for (int ci = 0; ci < ch_count; ci++) {
            if (!channels[ci])
                continue;
            XrChannel *dch = (XrChannel *) channels[ci];
            if (dch->dist) {
                dhooks->on_select_enter(dch);
            }
        }
    }

    frame->pc = pc;
    xr_worker_block_select(worker, coro, channels, ch_count);
    xr_free(channels);

    return VM_COLD_BLOCKED;
}

XR_NOINLINE int vm_chan_send_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                                     XrValue *base, XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrValue ch_val = base[b];
    if (!xr_value_is_channel(ch_val)) {
        base[a] = xr_bool(false);
        return VM_COLD_BREAK;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    XrValue value = vm_chan_copy_send(isolate, base[c]);
    XrValue timeout_val = base[c + 1];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val))
        timeout_ms = XR_TO_INT(timeout_val);
    else if (XR_IS_FLOAT(timeout_val))
        timeout_ms = (int64_t) XR_TO_FLOAT(timeout_val);
    if (timeout_ms < 0)
        timeout_ms = 0;

    // Try immediate send
    if (xr_channel_try_send(ch, value)) {
        base[a] = xr_bool(true);
        xr_runtime_wake_channel(isolate, ch, false);
        return VM_COLD_BREAK;
    }
    if (xr_channel_is_closed(ch)) {
        base[a] = xr_bool(false);
        return VM_COLD_BREAK;
    }
    if (timeout_ms <= 0) {
        base[a] = xr_bool(false);
        return VM_COLD_BREAK;
    }

    XrCoroutine *current = vm_cold_get_coro(vm_ctx);
    if (current) {
        int64_t now_us = (int64_t) (xr_time_monotonic_ns() / 1000ULL);

        if (current->channel_deadline == 0)
            current->channel_deadline = now_us + timeout_ms * 1000LL;

        if (now_us >= current->channel_deadline) {
            current->channel_deadline = 0;
            base[a] = xr_bool(false);
            return VM_COLD_BREAK;
        }
        if (xr_channel_try_send(ch, value)) {
            current->channel_deadline = 0;
            base[a] = xr_bool(true);
            xr_runtime_wake_channel(isolate, ch, false);
            return VM_COLD_BREAK;
        }
        if (xr_channel_is_closed(ch)) {
            current->channel_deadline = 0;
            base[a] = xr_bool(false);
            return VM_COLD_BREAK;
        }

        frame->pc = pc - 1;
        return VM_COLD_YIELD;
    }

    // Main thread: synchronous polling
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
        sched_yield();
    }
    return VM_COLD_BREAK;
}

XR_NOINLINE int vm_chan_recv_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                                     XrValue *base, XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrValue ch_val = base[b];
    if (!xr_value_is_channel(ch_val)) {
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    XrValue timeout_val = base[c];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val))
        timeout_ms = XR_TO_INT(timeout_val);
    else if (XR_IS_FLOAT(timeout_val))
        timeout_ms = (int64_t) XR_TO_FLOAT(timeout_val);

    // Try immediate receive
    bool ok;
    XrValue value = xr_channel_try_recv(ch, &ok);
    if (ok) {
        base[a] = vm_chan_copy_recv(isolate, value, vm_ctx);
        xr_runtime_wake_channel(isolate, ch, true);
        return VM_COLD_BREAK;
    }
    if (xr_channel_is_closed(ch)) {
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }
    if (timeout_ms <= 0) {
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }

    XrCoroutine *current = vm_cold_get_coro(vm_ctx);
    if (current) {
        int64_t now_us = (int64_t) (xr_time_monotonic_ns() / 1000ULL);

        if (current->channel_deadline == 0)
            current->channel_deadline = now_us + timeout_ms * 1000LL;

        if (now_us >= current->channel_deadline) {
            current->channel_deadline = 0;
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }
        value = xr_channel_try_recv(ch, &ok);
        if (ok) {
            current->channel_deadline = 0;
            base[a] = vm_chan_copy_recv(isolate, value, vm_ctx);
            xr_runtime_wake_channel(isolate, ch, true);
            return VM_COLD_BREAK;
        }
        if (xr_channel_is_closed(ch)) {
            current->channel_deadline = 0;
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }

        frame->pc = pc - 1;
        return VM_COLD_YIELD;
    }

    // Main thread: synchronous polling
    uint64_t start_ns = xr_time_monotonic_ns();
    while (1) {
        int64_t elapsed_ms = (int64_t) ((xr_time_monotonic_ns() - start_ns) / 1000000ULL);
        if (elapsed_ms >= timeout_ms) {
            base[a] = xr_null();
            break;
        }
        value = xr_channel_try_recv(ch, &ok);
        if (ok) {
            base[a] = vm_chan_copy_recv(isolate, value, vm_ctx);
            xr_runtime_wake_channel(isolate, ch, true);
            break;
        }
        if (xr_channel_is_closed(ch)) {
            base[a] = xr_null();
            break;
        }
        sched_yield();
    }
    return VM_COLD_BREAK;
}
