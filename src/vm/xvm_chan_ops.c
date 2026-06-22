/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_chan_ops.c - Dispatch helpers for channel, timer, and select operations
 *
 * Implements channel send/recv, timer channel creation, sleep blocking,
 * and the select-block driver. Declarations live in xvm_dispatch_helpers.h.
 *
 * Owns:
 *   - vm_time_dispatch      (timer channel creation and sleep)
 *   - vm_select_block        (multiplex blocking select)
 *   - vm_chan_send         (Channel send)
 *   - vm_chan_recv         (Channel recv)
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
#include "../coro/xcoro_pool.h"
#include "../coro/xtask.h"
#include "../coro/xdeep_copy.h"
#include <string.h>

#define VM_SELECT_RESULT_SLOT_STACK_CAP 256

static inline XrDispatchAction vm_chan_ready_next_or_yield(XrayIsolate *isolate,
                                                           XrCoroutine *current,
                                                           XrBcCallFrame *frame,
                                                           XrInstruction *pc) {
    return vm_ready_operation_next_or_yield(isolate, current, frame, pc,
                                            XR_VM_CHAN_READY_REDUCTION_COST);
}

static XrDispatchAction vm_time_after_impl(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                           XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                           XrInstruction *pc) {
    (void) vm_ctx;
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    XrValue timeout_val = base[b];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val)) {
        timeout_ms = XR_TO_INT(timeout_val);
    } else if (XR_IS_FLOAT(timeout_val)) {
        timeout_ms = (int64_t) XR_TO_FLOAT(timeout_val);
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }

    XrChannel *timer_ch = xr_channel_new_timer_vm(isolate, timeout_ms);
    if (!timer_ch) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "time.after: out of memory");
    }

    XrWorker *worker = xr_current_worker();
    if (worker && worker->p.timer_wheel) {
        xr_channel_timer_arm(timer_ch, worker->p.timer_wheel);
    }

    base[a] = xr_value_from_channel(timer_ch);
    return XR_DISP_NEXT;
}

static XrDispatchAction vm_sleep_impl(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc) {
    int a = GETARG_A(instr);
    XrValue val = base[a];

    int64_t milliseconds = 0;
    if (XR_IS_INT(val)) {
        milliseconds = XR_TO_INT(val);
    } else if (XR_IS_FLOAT(val)) {
        milliseconds = (int64_t) XR_TO_FLOAT(val);
    }
    if (milliseconds <= 0) {
        return XR_DISP_NEXT;
    }

    XrCoroutine *coro = vm_get_coro(vm_ctx);
    if (coro) {
        vm_suspend_continue_from_next(frame, pc);
        XrCoroBlockResult sleep_result = xr_coro_sleep(coro, milliseconds);
        if (sleep_result.kind == XR_CORO_BLOCK_BLOCKED) {
            return XR_DISP_BLOCKED;
        }
        if (sleep_result.kind == XR_CORO_BLOCK_ERROR) {
            VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "sleep: out of memory");
        }
        if (sleep_result.kind != XR_CORO_BLOCK_NO_CORO) {
            return XR_DISP_NEXT;
        }
    }

    xr_time_sleep_ms((uint64_t) milliseconds);
    return XR_DISP_NEXT;
}

XR_FUNC XrDispatchAction vm_time_dispatch(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                          XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                          XrInstruction *pc) {
    switch (GET_OPCODE(instr)) {
        case OP_TIME_AFTER:
            return vm_time_after_impl(isolate, vm_ctx, instr, base, frame, pc);
        case OP_SLEEP:
            return vm_sleep_impl(isolate, vm_ctx, instr, base, frame, pc);
        default:
            return XR_DISP_FATAL;
    }
}

XR_FUNC XrDispatchAction vm_select_block(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                         XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                         XrInstruction *pc) {
    int base_reg = GETARG_A(instr);
    int ch_count = GETARG_B(instr);
    int case_count = GETARG_C(instr);

    XrCoroutine *coro = vm_get_coro(vm_ctx);
    if (!coro) {
        return XR_DISP_NEXT;
    }

    int slot_count = ch_count < case_count ? ch_count : case_count;
    XrSlotRef stack_result_slots[VM_SELECT_RESULT_SLOT_STACK_CAP];
    XrSlotRef *result_slots = stack_result_slots;
    if (slot_count > VM_SELECT_RESULT_SLOT_STACK_CAP) {
        result_slots = (XrSlotRef *) xr_malloc((size_t) slot_count * sizeof(XrSlotRef));
        if (!result_slots) {
            VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "select: out of memory");
        }
    }
    for (int i = 0; i < slot_count; i++) {
        result_slots[i] = xr_slot_xvalue_ptr(&base[base_reg + i]);
    }

    vm_suspend_continue_from_next(frame, pc);
    XrCoroBlockResult result =
        xr_coro_select_block(coro, &base[base_reg], ch_count, result_slots, case_count);
    if (result_slots != stack_result_slots) {
        xr_free(result_slots);
    }
    if (result.kind == XR_CORO_BLOCK_ERROR) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "select: out of memory");
    }
    if (result.kind == XR_CORO_BLOCK_BLOCKED) {
        vm_suspend_continue_from_next(frame, pc);
        return XR_DISP_BLOCKED;
    }
    return XR_DISP_NEXT;
}

XR_FUNC XrDispatchAction vm_chan_send(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrCoroutine *current = vm_get_coro(vm_ctx);
    XrCoroBlockResult resumed = xr_coro_chan_send_resume(current, xr_slot_none());
    if (resumed.kind == XR_CORO_BLOCK_READY) {
        base[a] = xr_null();
        return XR_DISP_NEXT;
    }
    if (resumed.kind == XR_CORO_BLOCK_CLOSED) {
        base[a] = xr_null();
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "Channel is closed");
    }

    XrValue ch_val = base[b];
    if (!xr_value_is_channel(ch_val)) {
        base[a] = xr_null();
        VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "send: expected Channel");
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    vm_suspend_replay_yielded(frame, pc);
    XrCoroBlockResult result = xr_coro_chan_send(current, ch, base[c], xr_slot_none(), -1);
    if (result.kind == XR_CORO_BLOCK_READY) {
        vm_suspend_clear_yielded(frame);
        base[a] = xr_null();
        return vm_chan_ready_next_or_yield(isolate, current, frame, pc);
    }
    if (result.kind == XR_CORO_BLOCK_CLOSED) {
        vm_suspend_clear_yielded(frame);
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "Channel is closed");
    }
    if (result.kind == XR_CORO_BLOCK_BLOCKED) {
        /* Frame state saved above; the dispatch loop may switch to a
         * just-woken LIFO partner without exiting run(). */
        return XR_DISP_SWITCH;
    }

    vm_suspend_clear_yielded(frame);
    VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "Channel send failed");
}

XR_FUNC XrDispatchAction vm_chan_recv(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);

    XrCoroutine *current = vm_get_coro(vm_ctx);
    if (current) {
        XrCoroBlockResult resumed = xr_coro_chan_recv_resume(current, xr_slot_xvalue_ptr(&base[a]),
                                                             xr_slot_xvalue_ptr(&base[a + 1]));
        if (resumed.kind == XR_CORO_BLOCK_READY) {
            return XR_DISP_NEXT;
        }
        if (resumed.kind == XR_CORO_BLOCK_TIMEOUT) {
            base[a] = xr_null();
            base[a + 1] = xr_bool(false);
            return XR_DISP_NEXT;
        }
    }

    XrValue ch_val = base[b];
    if (!xr_value_is_channel(ch_val)) {
        base[a] = xr_null();
        base[a + 1] = xr_bool(false);
        VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "recv: expected Channel");
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    // Replay-capable registration with delivery capability: copy-free wakes
    // are delivered by the waker and resume at the next instruction; values
    // needing a receive-side deep copy replay through the resume protocol.
    vm_suspend_replay_yielded(frame, pc);
    XrCoroBlockResult result = xr_coro_chan_recv(current, ch, xr_slot_xvalue_ptr(&base[a]),
                                                 xr_slot_xvalue_ptr(&base[a + 1]), -1, true);
    if (result.kind == XR_CORO_BLOCK_READY || result.kind == XR_CORO_BLOCK_CLOSED) {
        vm_suspend_clear_yielded(frame);
        if (result.kind == XR_CORO_BLOCK_READY)
            return vm_chan_ready_next_or_yield(isolate, current, frame, pc);
        return XR_DISP_NEXT;
    }
    if (result.kind == XR_CORO_BLOCK_BLOCKED) {
        /* Frame state saved above; the dispatch loop may switch to a
         * just-woken LIFO partner without exiting run(). */
        return XR_DISP_SWITCH;
    }

    vm_suspend_clear_yielded(frame);
    base[a] = xr_null();
    base[a + 1] = xr_bool(false);
    VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "recv: need to use blocking recv in coroutine");
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
        vm_suspend_replay_current(frame, pc);
        XrCoroBlockResult result = xr_coro_chan_send(current, ch, value, result_slot, timeout_ms);
        if (result.kind == XR_CORO_BLOCK_READY || result.kind == XR_CORO_BLOCK_TIMEOUT ||
            result.kind == XR_CORO_BLOCK_CLOSED || result.kind == XR_CORO_BLOCK_NO_CORO) {
            vm_suspend_continue_from_next(frame, pc);
            if (result.kind == XR_CORO_BLOCK_READY)
                return vm_chan_ready_next_or_yield(isolate, current, frame, pc);
            return XR_DISP_NEXT;
        }
        if (result.kind == XR_CORO_BLOCK_BLOCKED) {
            return XR_DISP_BLOCKED;
        }
        vm_suspend_continue_from_next(frame, pc);
        base[a] = xr_bool(false);
        return XR_DISP_NEXT;
    }

    // Main thread: synchronous polling
    value = xr_chan_prepare_send(isolate, value);
    if (xr_channel_try_send(ch, value)) {
        base[a] = xr_bool(true);
        xr_runtime_wake_channel(ch->scheduler, ch, false);
        return XR_DISP_NEXT;
    }
    if (xr_channel_is_closed(ch) || timeout_ms <= 0) {
        xr_chan_abandon_send_core(xr_isolate_get_runtime_core(isolate), value);
        base[a] = xr_bool(false);
        return XR_DISP_NEXT;
    }

    uint64_t start_ns = xr_time_monotonic_ns();
    while (1) {
        int64_t elapsed_ms = (int64_t) ((xr_time_monotonic_ns() - start_ns) / 1000000ULL);
        if (elapsed_ms >= timeout_ms) {
            xr_chan_abandon_send_core(xr_isolate_get_runtime_core(isolate), value);
            base[a] = xr_bool(false);
            break;
        }
        if (xr_channel_try_send(ch, value)) {
            base[a] = xr_bool(true);
            xr_runtime_wake_channel(ch->scheduler, ch, false);
            break;
        }
        if (xr_channel_is_closed(ch)) {
            xr_chan_abandon_send_core(xr_isolate_get_runtime_core(isolate), value);
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
        XrCoroBlockResult resumed = xr_coro_chan_recv_resume(current, value_slot, ok_slot);
        if (resumed.kind == XR_CORO_BLOCK_READY || resumed.kind == XR_CORO_BLOCK_TIMEOUT ||
            resumed.kind == XR_CORO_BLOCK_CLOSED) {
            return XR_DISP_NEXT;
        }
        vm_suspend_replay_current(frame, pc);
        XrCoroBlockResult result =
            xr_coro_chan_recv(current, ch, value_slot, ok_slot, timeout_ms, false);
        if (result.kind == XR_CORO_BLOCK_READY || result.kind == XR_CORO_BLOCK_TIMEOUT ||
            result.kind == XR_CORO_BLOCK_CLOSED || result.kind == XR_CORO_BLOCK_NO_CORO) {
            vm_suspend_continue_from_next(frame, pc);
            if (result.kind == XR_CORO_BLOCK_READY)
                return vm_chan_ready_next_or_yield(isolate, current, frame, pc);
            return XR_DISP_NEXT;
        }
        if (result.kind == XR_CORO_BLOCK_BLOCKED) {
            return XR_DISP_BLOCKED;
        }
        vm_suspend_continue_from_next(frame, pc);
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
