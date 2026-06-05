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
    if (!coro) {
        return XR_DISP_NEXT;
    }

    XrCoroBlockResult result =
        xr_coro_select_block(isolate, coro, &base[base_reg], ch_count, case_count, base_reg);
    if (result.kind == XR_CORO_BLOCK_ERROR) {
        VM_THROW(frame, pc, XR_ERR_OUT_OF_MEMORY, "select: out of memory");
    }
    if (result.kind == XR_CORO_BLOCK_BLOCKED) {
        frame->pc = pc;
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

    frame->pc = pc - 1;
    frame->call_status |= XR_CALL_YIELDED;
    XrCoroBlockResult result = xr_coro_chan_send(isolate, current, ch, base[c], xr_slot_none(), -1);
    if (result.kind == XR_CORO_BLOCK_READY) {
        frame->call_status &= ~XR_CALL_YIELDED;
        base[a] = xr_null();
        return XR_DISP_NEXT;
    }
    if (result.kind == XR_CORO_BLOCK_CLOSED) {
        frame->call_status &= ~XR_CALL_YIELDED;
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "Channel is closed");
    }
    if (result.kind == XR_CORO_BLOCK_BLOCKED) {
        return XR_DISP_BLOCKED;
    }

    frame->call_status &= ~XR_CALL_YIELDED;
    VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "Channel send failed");
}

XR_FUNC XrDispatchAction vm_chan_recv(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);

    XrCoroutine *current = vm_get_coro(vm_ctx);
    if (current) {
        XrCoroBlockResult resumed = xr_coro_chan_recv_resume(
            isolate, current, xr_slot_xvalue_ptr(&base[a]), xr_slot_xvalue_ptr(&base[a + 1]));
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

    frame->pc = pc - 1;
    frame->call_status |= XR_CALL_YIELDED;
    XrCoroBlockResult result = xr_coro_chan_recv(isolate, current, ch, xr_slot_xvalue_ptr(&base[a]),
                                                 xr_slot_xvalue_ptr(&base[a + 1]), -1);
    if (result.kind == XR_CORO_BLOCK_READY || result.kind == XR_CORO_BLOCK_CLOSED) {
        frame->call_status &= ~XR_CALL_YIELDED;
        return XR_DISP_NEXT;
    }
    if (result.kind == XR_CORO_BLOCK_BLOCKED) {
        return XR_DISP_BLOCKED;
    }

    frame->call_status &= ~XR_CALL_YIELDED;
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
