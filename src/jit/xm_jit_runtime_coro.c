/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_jit_runtime_coro.c - JIT runtime helpers: coroutine, channel,
 *                          structured concurrency, OSR, print, arithmetic
 *
 * Contains: channel new/send/recv, scope enter/exit, go/spawn,
 * await, OSR entry/trigger, print, polymorphic arithmetic.
 */

#include "xm_jit.h"
#include "xm_jit_internal.h"
#include "xm.h"
#include "../base/xlog.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xmap.h"
#include "../runtime/object/xjson.h"
#include "../runtime/object/xrange.h"
#include "../runtime/object/xset.h"
#include "../runtime/object/xexception.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xenum.h"
#include "../runtime/value/xtype_names.h"
#include "../runtime/value/xvalue_print.h"
#include "../runtime/closure/xcell.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/xexec_frame.h"
#include "../runtime/xexec_state.h"
#include "../runtime/xerror_codes.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xstrbuf.h"
#include "../coro/xcoroutine.h"
#include "../coro/xchannel.h"
#include "../coro/xdeep_copy.h"
#include "../coro/xchannel_ops.h"
#include "../coro/xtask.h"
#include "../coro/xworker.h"
#include "../coro/xcoro_pool.h"
#include "../vm/xvm.h"
#include "../vm/xvm_internal.h"
#include "xm_codegen.h"
#include "xm_jit_debug.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
/* ========== Coroutine Runtime Helpers ========== */

// Called from JIT via CALL_C for OP_CHAN_NEW.
// extra_arg = buffer_size
// Returns raw channel pointer (tagged XrValue).
XrJitResult xr_jit_chan_new(XrCoroutine *coro, int64_t extra_arg) {
    uint32_t buffer_size = (uint32_t) (extra_arg & 0xFFFFFFFF);
    XrayIsolate *isolate = coro->isolate;
    if (!isolate)
        return XR_JIT_NULL();
    XrChannel *ch = xr_channel_new(isolate, buffer_size);
    if (!ch)
        return XR_JIT_NULL();
    XrValue v = xr_value_from_channel(ch);
    return XR_JIT_VAL(v);
}

// Called from JIT via CALL_C for OP_CHAN_CLOSE.
// jit_call_args[0] = channel raw pointer
// Returns 0.
XrJitResult xr_jit_chan_close(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val))
        return XR_JIT_OK();
    XrChannel *ch = xr_value_to_channel(ch_val);
    xr_channel_close(ch);
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_CHAN_IS_CLOSED.
// jit_call_args[0] = channel raw pointer
// Returns 1 if closed, 0 otherwise.
XrJitResult xr_jit_chan_is_closed(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val))
        return XR_JIT_BOOL(1);
    XrChannel *ch = xr_value_to_channel(ch_val);
    return XR_JIT_BOOL(xr_channel_is_closed(ch) ? 1 : 0);
}

// Called from JIT via CALL_C for OP_CHAN_TRY_SEND.
// jit_call_args[0] = channel raw, jit_call_args[1] = value raw
// extra_arg = slot_type of value
// Returns 1 on success, 0 on failure.
// Canonical logic in xr_chan_try_send (xchannel_ops.h).
XrJitResult xr_jit_chan_try_send(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val))
        return XR_JIT_BOOL(0);
    XrChannel *ch = xr_value_to_channel(ch_val);

    XrValue send_v =
        jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);

    bool success = xr_chan_try_send(coro->isolate, ch, send_v);
    return XR_JIT_BOOL(success ? 1 : 0);
}

// Called from JIT via CALL_C for OP_CHAN_TRY_RECV.
// jit_call_args[0] = channel raw
// Returns received value raw (or 0 for null). Stores ok flag in jit_call_args[1].
// Canonical logic in xr_chan_try_recv (xchannel_ops.h).
XrJitResult xr_jit_chan_try_recv(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        coro->jit_ctx->call_args[1] = 0;  // ok = false
        return XR_JIT_NULL();
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    XrValue recv_val;
    if (xr_chan_try_recv(coro->isolate, ch, &recv_val, coro)) {
        coro->jit_ctx->call_args[1] = 1;  // ok = true
        return XR_JIT_VAL(recv_val);
    }
    coro->jit_ctx->call_args[1] = 0;  // ok = false
    return XR_JIT_NULL();
}

/* ========== Blocking Channel Send/Recv (JIT CPS) ========== */

// Fast path for OP_CHAN_SEND: try non-blocking send.
// jit_call_args[0] = channel raw, jit_call_args[1] = value raw
// extra_arg = slot_type of value
// Returns XR_JIT_NULL on success, DEOPT_MARKER if needs blocking.
XrJitResult xr_jit_chan_send(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        return (XrJitResult) {XM_DEOPT_MARKER, 0};
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    if (xr_channel_is_closed(ch)) {
        return (XrJitResult) {XM_DEOPT_MARKER, 0};
    }

    XrValue send_v =
        jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);

    // Deep copy mutable values for buffer safety
    if (XR_IS_PTR(send_v) && xr_value_needs_copy(send_v)) {
        XrayIsolate *isolate = coro->isolate;
        send_v = xr_deep_copy(isolate, send_v, xr_isolate_get_gc(isolate));
    }

    // Store prepared value for block helper
    coro->jit_ctx->call_args[1] = send_v.i;
    coro->jit_ctx->call_args[2] = (int64_t) send_v.tag;

    bool ok = xr_channel_try_send(ch, send_v);
    if (ok) {
        xr_runtime_wake_channel(coro->isolate, ch, false);
        return XR_JIT_NULL();
    }
    // Need blocking → XM_SUSPEND will save regs then call block helper
    return (XrJitResult) {XM_DEOPT_MARKER, 0};
}

// Block helper for channel send (called from XM_SUSPEND after regs saved).
// Follows Go gopark pattern: regs already saved → call xr_channel_send
// which sets BLOCKED under lock.
// Returns 0 if blocked, 1 if send succeeded during retry.
XrJitResult xr_jit_chan_send_block(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        coro->jit_suspend->result = xr_null().i;
        coro->jit_suspend->result_tag = XR_TAG_NULL;
        return (XrJitResult) {1, 0};  // not blocked, handle error inline
    }
    XrChannel *ch = xr_value_to_channel(ch_val);
    uint8_t val_tag = (uint8_t) (coro->jit_ctx->call_args[2] & 0xFF);
    XrValue send_v = jit_value_from_tag(coro->jit_ctx->call_args[1], val_tag);

    coro->send_value = send_v;
    XrChanResult cr = xr_channel_send(ch, send_v, coro);
    if (cr == XR_CHAN_OK) {
        coro->jit_suspend->result = xr_null().i;
        coro->jit_suspend->result_tag = XR_TAG_NULL;
        return (XrJitResult) {1, 0};  // succeeded during retry
    }
    if (cr == XR_CHAN_BLOCK) {
        // BLOCKED already set by xr_channel_send under lock (gopark pattern).
        // Do NOT touch flags after this point — coro may already be woken
        // by another worker. VM bytecode path also doesn't set wait_reason.
        return (XrJitResult) {0, 0};  // blocked
    }
    // Closed or error: store null result, continue inline
    coro->jit_suspend->result = xr_null().i;
    coro->jit_suspend->result_tag = XR_TAG_NULL;
    return XR_JIT_OK();
}

// Fast path for OP_CHAN_RECV: try non-blocking recv.
// jit_call_args[0] = channel raw
// Returns XrJitResult with value on success, DEOPT_MARKER if needs blocking.
// Stores ok flag in jit_call_args[1].
XrJitResult xr_jit_chan_recv(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        coro->jit_ctx->call_args[1] = 0;
        return XR_JIT_NULL();
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    bool ok;
    XrValue value = xr_channel_try_recv(ch, &ok);
    if (ok) {
        // Deep copy to receiver's heap
        if (XR_IS_PTR(value) && xr_value_needs_copy(value)) {
            value = xr_deep_copy_to_coro(coro->isolate, value, coro);
        }
        coro->jit_ctx->call_args[1] = 1;
        return XR_JIT_RESULT(value);
    }

    if (xr_channel_is_closed(ch)) {
        coro->jit_ctx->call_args[1] = 0;
        return XR_JIT_NULL();
    }

    // Need blocking → XM_SUSPEND will save regs then call block helper
    return (XrJitResult) {XM_DEOPT_MARKER, 0};
}

// Block helper for channel recv (called from XM_SUSPEND after regs saved).
// Sets recv_slot to &suspend_regs[23] so sender writes directly there.
// Returns 0 if blocked, 1 if recv succeeded during retry.
XrJitResult xr_jit_chan_recv_block(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue ch_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_channel(ch_val)) {
        coro->jit_suspend->result = xr_null().i;
        coro->jit_suspend->result_tag = XR_TAG_NULL;
        return XR_JIT_OK();
    }
    XrChannel *ch = xr_value_to_channel(ch_val);

    // recv_slot must point to persistent memory — sender writes *recv_slot
    // after dequeuing this coro from recvq.  Use coro's bytecode stack[0]
    // which is always allocated and persistent (JIT doesn't use it).
    coro->recv_slot = &coro->vm_ctx.stack[0];

    XrValue out;
    XrChanResult cr = xr_channel_recv(ch, &out, coro);
    if (cr == XR_CHAN_OK) {
        // Deep copy to receiver's heap
        if (XR_IS_PTR(out) && xr_value_needs_copy(out)) {
            out = xr_deep_copy_to_coro(coro->isolate, out, coro);
        }
        coro->jit_suspend->result = out.i;
        coro->jit_suspend->result_tag = out.tag;
        return (XrJitResult) {1, 0};  // succeeded during retry
    }
    if (cr == XR_CHAN_CLOSED) {
        coro->jit_suspend->result = xr_null().i;
        coro->jit_suspend->result_tag = XR_TAG_NULL;
        return (XrJitResult) {1, 0};
    }
    if (cr == XR_CHAN_BLOCK) {
        // BLOCKED already set by xr_channel_recv under lock (gopark pattern).
        // recv_slot → stack[0] is persistent; sender will write there.
        // Worker resume path copies stack[0] → jit_suspend.result before
        // calling xm_jit_resume.
        // Do NOT touch flags — coro may already be woken by another worker.
        return (XrJitResult) {0, 0};  // blocked
    }
    coro->jit_suspend->result = xr_null().i;
    coro->jit_suspend->result_tag = XR_TAG_NULL;
    return XR_JIT_OK();
}

/* ========== Method Invocation ========== */

// Called from JIT via CALL_C for OP_INVOKE_DIRECT.
// jit_call_args[0] = receiver (raw value, tagged or ptr depending on slot type)
// jit_call_args[1..nargs] = method arguments
// extra_arg = (recv_xr_tag << 24) | (method_idx << 8) | nargs
// Resolves method closure from instance's class, then calls via VM interpreter.
XrJitResult xr_jit_invoke_direct(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t recv_tag = (uint8_t) ((extra_arg >> 24) & 0xFF);
    int method_idx = (int) ((extra_arg >> 8) & 0xFFFF);
    int nargs = (int) (extra_arg & 0xFF);

    XrValue recv_val = jit_value_from_tag(coro->jit_ctx->call_args[0], recv_tag);
    if (!XR_IS_PTR(recv_val) || !recv_val.ptr)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};
    XrInstance *inst = xr_value_to_instance(recv_val);
    if (!inst)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    XrClass *cls = xr_instance_get_class(inst);
    if (!cls || method_idx >= cls->method_count)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    XrMethod *method = &cls->methods[method_idx];

    /* Primitive method: call C function directly.  This mirrors the
     * defensive guard added to OP_INVOKE_DIRECT in the VM interpreter —
     * accessing .as.closure on a PRIMITIVE slot reads the wrong union
     * member and crashes. */
    if (method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
        XrayIsolate *isolate = coro->isolate;
        if (!isolate)
            return (XrJitResult) {XM_DEOPT_MARKER, 0};
        XrValue prim_args[16];
        XrValue receiver = xr_value_from_instance(inst);
        for (int i = 0; i < nargs && i < 15; i++)
            prim_args[i] = jit_value_from_tag(coro->jit_ctx->call_args[1 + i], XR_TAG_I64);
        XrValue result = method->as.primitive(isolate, receiver, prim_args, nargs);
        if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
            coro->jit_ctx->exception = (void *) coro->vm_ctx.current_exception.ptr;
            coro->vm_ctx.current_exception = XR_NULL_VAL;
            return XR_JIT_NULL();
        }
        return XR_JIT_RESULT(result);
    }

    XrClosure *closure = method->as.closure;
    if (!closure || !closure->proto)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    XrProto *proto = closure->proto;

    // Build args array: [0]=this(receiver), [1..nargs]=method args
    // Must use xr_value_from_instance so heap_type is set correctly;
    // the VM interpreter's xr_value_to_instance checks heap_type.
    XrValue args[16];
    args[0] = xr_value_from_instance(inst);
    for (int i = 0; i < nargs && i < 15; i++) {
        int64_t raw = coro->jit_ctx->call_args[1 + i];
        uint8_t tag = XR_TAG_I64;
        if (proto->param_types && (i + 1) < proto->param_types_count && proto->param_types[i + 1])
            tag = slot_type_to_xr_tag(xr_type_to_slot_type(proto->param_types[i + 1]));
        args[1 + i] = jit_value_from_tag(raw, tag);
    }

    XrayIsolate *isolate = coro->isolate;
    if (!isolate)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    bool saved_suppress = xr_isolate_get_suppress_exception_print(isolate);
    xr_isolate_set_suppress_exception_print(isolate, true);
    XrValue result = xr_vm_call_closure(isolate, closure, args, nargs + 1);
    xr_isolate_set_suppress_exception_print(isolate, saved_suppress);

    if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
        coro->jit_ctx->exception = (void *) coro->vm_ctx.current_exception.ptr;
        coro->vm_ctx.current_exception = XR_NULL_VAL;
        return XR_JIT_NULL();
    }

    {
        uint8_t rst =
            proto->return_type_info ? xr_type_to_slot_type(proto->return_type_info) : XR_SLOT_ANY;
        if (XR_SLOT_IS_FLOAT(rst)) {
            int64_t raw;
            memcpy(&raw, &result.f, sizeof(int64_t));
            return (XrJitResult) {raw, XR_TAG_F64};
        }
    }
    return XR_JIT_RESULT(result);
}

/* ========== JIT→General Function Call (OP_CALL) ========== */

// Called from JIT code via CALL_C for general function calls.
// jit_call_args[0] = raw closure pointer, jit_call_args[1..n] = raw arg values.
// extra_arg = nargs (low 8 bits).
// Tries JIT path first, falls back to VM interpreter.
XrJitResult xr_jit_call_func(XrCoroutine *coro, int64_t nargs_encoded) {
    int nargs = (int) (nargs_encoded & 0xFF);
    int64_t raw_closure = coro->jit_ctx->call_args[0];
    // Detect poison values from uninitialized JIT registers (OSR edge case)
    if (raw_closure == 0 || ((uint64_t) raw_closure >> 48) != 0)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    // Class call: allocate instance + call constructor inline
    XrGCHeader *callee_gc = (XrGCHeader *) raw_closure;
    if (XR_GC_GET_TYPE(callee_gc) == XR_TCLASS) {
        XrClass *klass = (XrClass *) raw_closure;
        XrayIsolate *isolate = coro->isolate;
        if (!isolate)
            return (XrJitResult) {XM_DEOPT_MARKER, 0};

        // Allocate instance on coroutine heap (normal storage mode)
        XrInstance *instance = xr_instance_new(isolate, klass);
        if (!instance)
            return (XrJitResult) {XM_DEOPT_MARKER, 0};

        // Look up constructor method (cached symbol for speed)
        static int ctor_sym = -1;
        if (ctor_sym < 0) {
            XrSymbolTable *st = (XrSymbolTable *) isolate->symbol_table;
            ctor_sym = (int) xr_symbol_lookup_in_table(st, XR_KEYWORD_CONSTRUCTOR);
        }
        XrMethod *ctor = (ctor_sym > 0) ? xr_class_lookup_method(klass, ctor_sym) : NULL;

        if (ctor && ctor->type == XMETHOD_CLOSURE) {
            // User constructor: shift args right, insert instance as arg[0]
            for (int i = nargs; i > 0; i--)
                coro->jit_ctx->call_args[1 + i] = coro->jit_ctx->call_args[i];
            coro->jit_ctx->call_args[1] = (int64_t) (uintptr_t) instance;
            coro->jit_ctx->call_args[0] = (int64_t) (uintptr_t) ctor->as.closure;

            // Recursive call to handle constructor (JIT fast path + VM fallback)
            xr_jit_call_func(coro, (nargs + 1) & 0xFF);
        } else if (ctor && ctor->type == XMETHOD_PRIMITIVE) {
            // Native constructor: box args and call C function
            XrValue args[16];
            for (int i = 0; i < nargs && i < 15; i++)
                args[i] = jit_value_from_tag(coro->jit_ctx->call_args[1 + i], XR_TAG_I64);
            ctor->as.primitive(isolate, xr_value_from_instance(instance), args, nargs);
        }

        // Return instance pointer regardless of constructor outcome
        return (XrJitResult) {(int64_t) (uintptr_t) instance, XR_TAG_PTR};
    }

    XrClosure *closure = (XrClosure *) raw_closure;
    if (!closure->proto)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    XrProto *proto = closure->proto;

    // On-demand JIT compilation for uncompiled callees.
    // Callbacks in JIT-compiled loops (filter/map/reduce) are small closures
    // that never reach the VM hot threshold. Compile them here on first call
    // to avoid expensive xr_vm_call_closure VM re-entry on every iteration.
    if (!proto->jit_entry && proto->deopt_count <= 3 && proto->bb_leaders && coro->isolate) {
        XmJitState *jit = coro->isolate->vm.jit;
        if (jit && jit->enabled) {
            // Synthesize or fix param_types for untyped params (default to i64).
            // Caller passes raw i64 values; this matches the VM fallback's
            // default tag (XR_TAG_I64) at line ~2340 below.
            // TFA may have created param_types but left params as NULL (ANY),
            // which is_jit_eligible rejects. Fill in missing param types.
            if (proto->numparams > 0 && !proto->type_feedback) {
                if (!proto->param_types) {
                    proto->param_types =
                        (struct XrType **) xr_calloc(proto->numparams, sizeof(struct XrType *));
                    if (proto->param_types)
                        proto->param_types_count = proto->numparams;
                }
                if (proto->param_types) {
                    for (int i = 0; i < proto->numparams; i++) {
                        if (i < proto->param_types_count && !proto->param_types[i])
                            proto->param_types[i] = xr_slot_type_to_type(NULL, XR_SLOT_I64);
                    }
                }
            }
            bool ok = xm_jit_try_compile(jit, proto);
            if (!ok)
                proto->deopt_count = 4;  // prevent retry on failure
        }
    }

    // JIT fast path: callee compiled (either pre-compiled or just-now compiled)
    if (proto->jit_entry) {
        void *saved_proto = coro->jit_ctx->call_proto;
        void *saved_closure = coro->jit_ctx->call_closure;
        coro->jit_ctx->call_proto = proto;
        coro->jit_ctx->call_closure = closure;
        // Set param_tags for callee's dynamic op tag lookups.
        // Derive tags from callee's param_types (the STORE_CORO builder path
        // does NOT populate call_arg_tags, so we use declared types).
        // Callee prologue copies param_tags[i] → vreg_runtime_tags[i].
        for (int i = 0; i < nargs && i < 8; i++) {
            uint8_t tag = XR_TAG_I64;
            if (proto->param_types && i < proto->param_types_count && proto->param_types[i])
                tag = slot_type_to_xr_tag(xr_type_to_slot_type(proto->param_types[i]));
            coro->jit_ctx->param_tags[i] = (int64_t) tag;
        }
#ifdef _WIN32
        int64_t payload =
            ((XmJitFn) proto->jit_entry)((intptr_t) coro, &coro->jit_ctx->call_args[1]);
        XrJitResult ret = {payload, (uint64_t) coro->jit_ctx->call_result_tag};
#else
        XrJitResult ret =
            ((XmJitFn) proto->jit_entry)((intptr_t) coro, &coro->jit_ctx->call_args[1]);
#endif
        coro->jit_ctx->call_proto = saved_proto;
        coro->jit_ctx->call_closure = saved_closure;
        if (ret.payload != XM_DEOPT_MARKER)
            return ret;
        // Callee deoptimized — clear stale deopt_id so the outer JIT
        // caller's post-CALL_C CBNZ check doesn't see it and mistakenly
        // deopt the outer function with the callee's deopt_id.
        coro->jit_ctx->deopt_id = 0;
        // Check if callee threw (jit_exception set by xr_jit_throw)
        if (coro->jit_ctx->exception)
            return XR_JIT_NULL();
    }

    // VM fallback: BOX args and call via interpreter
    XrayIsolate *isolate = coro->isolate;
    if (!isolate)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    XrValue args[16];
    for (int i = 0; i < nargs && i < 15; i++) {
        int64_t raw = coro->jit_ctx->call_args[1 + i];
        /* call_arg_tags[1+i] is set by emit_call_args_from_pool from the
         * caller's compile-time/runtime knowledge — use it first.  Only
         * fall back to proto->param_types when it is UNKNOWN, which is
         * what unions / dynamic args land as. */
        uint8_t tag = coro->jit_ctx->call_arg_tags[1 + i];
        if (tag == XR_RTAG_UNKNOWN || tag == 0) {
            tag = XR_TAG_I64;
            if (proto->param_types && i < proto->param_types_count && proto->param_types[i])
                tag = slot_type_to_xr_tag(xr_type_to_slot_type(proto->param_types[i]));
            if (tag == XR_RTAG_UNKNOWN)
                tag = XR_TAG_I64;
        }
        args[i] = jit_value_from_tag(raw, tag);
    }

    // Suppress VM uncaught-exception print (JIT caller handles exceptions)
    bool saved_suppress = xr_isolate_get_suppress_exception_print(isolate);
    xr_isolate_set_suppress_exception_print(isolate, true);
    XrValue result = xr_vm_call_closure(isolate, closure, args, nargs);
    xr_isolate_set_suppress_exception_print(isolate, saved_suppress);

    // Check if callee threw an exception (propagate to JIT catch handler)
    if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
        coro->jit_ctx->exception = (void *) coro->vm_ctx.current_exception.ptr;
        coro->vm_ctx.current_exception = XR_NULL_VAL;
        return XR_JIT_NULL();
    }

    // UNBOX result based on return type
    {
        uint8_t rst =
            proto->return_type_info ? xr_type_to_slot_type(proto->return_type_info) : XR_SLOT_ANY;
        if (XR_SLOT_IS_FLOAT(rst)) {
            int64_t raw;
            memcpy(&raw, &result.f, sizeof(int64_t));
            return (XrJitResult) {raw, XR_TAG_F64};
        }
    }
    return XR_JIT_RESULT(result);
}

/* ========== OSR Entry Bridge ========== */

// OSR calling convention: same as normal — x0=coro, x1=pointer to int64_t values[]
bool xm_jit_osr_enter(void *osr_entry, XrCoroutine *coro, int64_t *values, uint8_t return_type,
                      XrValue *result) {
    if (!osr_entry || !result)
        return false;

#ifdef _WIN32
    int64_t payload = ((XmJitFn) osr_entry)((intptr_t) coro, values);
    if (payload == XM_DEOPT_MARKER)
        return false;
    uint8_t tag = (uint8_t) coro->jit_ctx->call_result_tag;
#else
    XrJitResult jr = ((XmJitFn) osr_entry)((intptr_t) coro, values);
    if (jr.payload == XM_DEOPT_MARKER)
        return false;
    int64_t payload = jr.payload;
    uint8_t tag = (uint8_t) jr.tag;
#endif
    if (tag == XR_RTAG_UNKNOWN) {
        *result = jit_value_from_tag(payload, slot_type_to_xr_tag(return_type));
    } else {
        *result = jit_value_from_tag(payload, tag);
    }
    return true;
}

/* ========== Background Result Installation ========== */

/*
 * Install a completed background compilation result into proto fields.
 * Called from both OP_CALL and OSR paths when jit_entry_pending is ready.
 * Safe to call from any worker thread — proto fields are only written once
 * (bg thread never writes proto fields directly; only jit_entry_pending).
 *
 * Uses CAS on jit_entry_pending to ensure exactly-once installation
 * when multiple coroutines race to install the same result.
 */
void xm_jit_install_bg_result(XrProto *proto) {
    void *pending = atomic_load_explicit(&proto->jit_entry_pending, memory_order_acquire);
    if (!pending || (uintptr_t) pending <= 1)
        return;

    // CAS to claim installation (prevent double-install from racing workers)
    if (!atomic_compare_exchange_strong_explicit(&proto->jit_entry_pending, &pending, NULL,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        // Another thread installed it — jit_entry should be set now
        return;
    }

    XmBgResult *bgr = (XmBgResult *) pending;
    // Use unified install helper — single write sequence with release fence.
    // Ownership of heap-allocated fields (stack_map, deopt_table, osr_entries)
    // is transferred to proto; bgr itself is freed after.
    XmInstallData idata = {
        .code = bgr->code,
        .fast_entry = bgr->fast_entry,
        .resume_entry = bgr->resume_entry,
        .opt_level = bgr->opt_level,
        .stack_map = bgr->stack_map,
        .deopt_table = bgr->deopt_table,
        .ndeopt = bgr->ndeopt,
        .osr_entries = bgr->osr_entries,
        .nosr = bgr->nosr,
    };
    xm_jit_install_to_proto(proto, &idata);
    xr_free(bgr);
}

/* ========== OSR Trigger (called from VM at loop back-edges) ========== */

int xm_jit_osr_trigger(XmJitState *jit, XrProto *proto, XrCoroutine *coro, uint32_t bc_pc,
                       XrValue *base, int maxstack, uint8_t return_type, XrValue *result) {
    if (!jit || !jit->enabled || !proto || !result)
        return XM_JIT_DEOPT;

    // Step 1: ensure compiled code is available
    if (!proto->jit_entry) {
        // Check if background compilation finished
        if (jit->bg_queue) {
            void *pending = atomic_load_explicit(&proto->jit_entry_pending, memory_order_acquire);
            if (pending && (uintptr_t) pending > 1) {
                xm_jit_install_bg_result(proto);
            }
        }
        // Still no jit_entry → try sync compile
        if (!proto->jit_entry) {
            if (!xm_jit_try_compile(jit, proto))
                return XM_JIT_DEOPT;
        }
    }

    // Step 2: find OSR entry matching this loop header
    if (!proto->osr_entries || proto->nosr == 0)
        return XM_JIT_DEOPT;

    XmOsrEntry *entries = (XmOsrEntry *) proto->osr_entries;
    XmOsrEntry *match = NULL;
    for (uint32_t i = 0; i < proto->nosr; i++) {
        if (entries[i].bc_offset == bc_pc) {
            match = &entries[i];
            break;
        }
    }
    if (!match) {
        return XM_JIT_DEOPT;
    }

    // Step 3: build values array from interpreter registers
    // values[slot] = raw payload of R(slot)
    int nslots = maxstack < 256 ? maxstack : 256;
    int64_t values[256];
    for (int i = 0; i < nslots; i++) {
        values[i] = base[i].i;
    }

    // Step 4: compute absolute OSR entry address
    void *osr_entry = (uint8_t *) proto->jit_entry + match->entry_offset;

    // Step 5: set closure pointer for upvalue access
    // The closure is in the current call frame
    coro->jit_ctx->call_proto = proto;

    // Step 6: enter JIT at loop header
    int saved_id = coro->id;  // save before JIT call (coro may be resumed by another worker)
#ifdef _WIN32
    int64_t osr_payload = ((XmJitFn) osr_entry)((intptr_t) coro, values);
#else
    XrJitResult osr_jr = ((XmJitFn) osr_entry)((intptr_t) coro, values);
    int64_t osr_payload = osr_jr.payload;
#endif

    if (osr_payload == XM_DEOPT_MARKER) {
        // JIT ran but deoptimized mid-function. It may have produced
        // side effects (spawned coroutines, pushed to arrays, etc.).
        // Recover interpreter state from deopt info so the interpreter
        // continues from the deopt point, not from the OSR entry.
        int32_t deopt_pc = xm_jit_deopt_recover(coro, base, maxstack);
        if (deopt_pc >= 0) {
            coro->jit_ctx->osr_deopt_pc = deopt_pc;
        } else {
            coro->jit_ctx->osr_deopt_pc = -1;
        }
        return XM_JIT_DEOPT;
    }

    if (osr_payload == XM_SUSPEND_MARKER) {
        // JIT suspended at channel/await blocking point during OSR.
        // resume_entry/proto already set by XM_SUSPEND codegen.
        // Return XM_JIT_SUSPEND on the stack — no racy side-channel needed.
        *result = xr_null();
        return XM_JIT_SUSPEND;
    }

#ifdef _WIN32
    uint8_t osr_tag = (uint8_t) coro->jit_ctx->call_result_tag;
#else
    uint8_t osr_tag = (uint8_t) osr_jr.tag;
#endif
    if (osr_tag == XR_RTAG_UNKNOWN) {
        *result = jit_value_from_tag(osr_payload, slot_type_to_xr_tag(return_type));
    } else {
        *result = jit_value_from_tag(osr_payload, osr_tag);
    }
    return XM_JIT_OK;
}

/* ========== Print Helper ========== */

// Called from JIT codegen for XM_RT_ARRAY_NEW.
// extra_arg = capacity
// Returns raw ptr to XrArray.
XrJitResult xr_jit_rt_array_new(XrCoroutine *coro, int64_t capacity) {
    XrArray *arr = xr_array_with_capacity(coro, (int) capacity);
    return XR_JIT_PTR(arr);
}

// Called from JIT codegen for XM_RT_ARRAY_PUSH.
// call_args[0] = array ptr, call_args[1] = value raw payload
// extra_arg = val_slot_type
XrJitResult xr_jit_rt_array_push(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrArray *arr = (XrArray *) (uintptr_t) coro->jit_ctx->call_args[0];
    if (!arr)
        return XR_JIT_OK();
    // Guard: type mismatch can pass non-array objects here
    if (XR_GC_GET_TYPE((XrGCHeader *) arr) != XR_TARRAY)
        return XR_JIT_OK();
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    xr_array_push(arr, val);
    return XR_JIT_OK();
}

// Called from JIT codegen for XM_RT_ARRAY_LEN.
// call_args[0] = array ptr
// Returns array length as i64.
XrJitResult xr_jit_rt_array_len(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrArray *arr = (XrArray *) (uintptr_t) coro->jit_ctx->call_args[0];
    if (!arr)
        return XR_JIT_INT(0);
    return XR_JIT_INT((int64_t) arr->length);
}

// Called from JIT codegen for XM_RT_MAP_NEW.
// extra_arg = capacity (unused, always creates default-sized map)
// Returns raw ptr to XrMap.
XrJitResult xr_jit_rt_map_new(XrCoroutine *coro, int64_t capacity) {
    (void) capacity;
    XrMap *map = xr_map_new(coro);
    return XR_JIT_PTR(map);
}

// Called from JIT via CALL_C for polymorphic OP_ADD.
// call_args[0] = lhs raw payload, call_args[1] = rhs raw payload
// Handles: int+int, float+float, mixed numeric, string concat.
XrJitResult xr_jit_rt_add(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);

    // Integer addition
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XrValue r;
        r.descriptor = 0;
        r.tag = XR_TAG_I64;
        r.i = (int64_t) ((uint64_t) XR_TO_INT(vb) + (uint64_t) XR_TO_INT(vc));
        return XR_JIT_RESULT(r);
    }
    // Float promotion
    {
        double nb = 0, nc = 0;
        if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
            XrValue r;
            r.descriptor = 0;
            r.tag = XR_TAG_F64;
            r.f = nb + nc;
            int64_t raw;
            memcpy(&raw, &r.f, sizeof(double));
            return (XrJitResult) {raw, XR_TAG_F64};
        }
    }
    // String concatenation
    XrayIsolate *isolate = coro->isolate;
    XrString *str_b = xr_value_to_string(isolate, vb);
    XrString *str_c = xr_value_to_string(isolate, vc);
    XrStrBuf *sb = xr_strbuf_tmp(isolate);
    xr_strbuf_append_str(sb, str_b);
    xr_strbuf_append_str(sb, str_c);
    XrValue result = xr_string_value(xr_strbuf_to_string(sb));
    return XR_JIT_RESULT(result);
}

// call_args[0] = lhs raw payload, call_args[1] = rhs raw payload
// Handles: int-int, mixed numeric (float promotion).
XrJitResult xr_jit_rt_sub(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XrValue r;
        r.descriptor = 0;
        r.tag = XR_TAG_I64;
        r.i = (int64_t) ((uint64_t) XR_TO_INT(vb) - (uint64_t) XR_TO_INT(vc));
        return XR_JIT_RESULT(r);
    }
    double nb = 0, nc = 0;
    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
        XrValue r;
        r.descriptor = 0;
        r.tag = XR_TAG_F64;
        r.f = nb - nc;
        int64_t raw;
        memcpy(&raw, &r.f, sizeof(double));
        return (XrJitResult) {raw, XR_TAG_F64};
    }
    return XR_JIT_NULL();
}

XrJitResult xr_jit_rt_mul(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        XrValue r;
        r.descriptor = 0;
        r.tag = XR_TAG_I64;
        r.i = (int64_t) ((uint64_t) XR_TO_INT(vb) * (uint64_t) XR_TO_INT(vc));
        return XR_JIT_RESULT(r);
    }
    double nb = 0, nc = 0;
    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
        XrValue r;
        r.descriptor = 0;
        r.tag = XR_TAG_F64;
        r.f = nb * nc;
        int64_t raw;
        memcpy(&raw, &r.f, sizeof(double));
        return (XrJitResult) {raw, XR_TAG_F64};
    }
    return XR_JIT_NULL();
}

XrJitResult xr_jit_rt_div(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    // Division always produces float
    double nb = 0, nc = 0;
    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
        if (nc == 0.0) {
            XrValue exc = xr_exception_newf(coro->isolate, XR_ERR_DIV_BY_ZERO, "division by zero");
            coro->jit_ctx->exception = (void *) exc.ptr;
            return XR_JIT_NULL();
        }
        XrValue r;
        r.descriptor = 0;
        r.tag = XR_TAG_F64;
        r.f = nb / nc;
        int64_t raw;
        memcpy(&raw, &r.f, sizeof(double));
        return (XrJitResult) {raw, XR_TAG_F64};
    }
    XrValue exc = xr_exception_newf(coro->isolate, XR_ERR_DIV_BY_ZERO, "division by zero");
    coro->jit_ctx->exception = (void *) exc.ptr;
    return XR_JIT_NULL();
}

XrJitResult xr_jit_rt_mod(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue vb = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrValue vc = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    if (XR_IS_INT(vb) && XR_IS_INT(vc)) {
        int64_t b = XR_TO_INT(vb), c = XR_TO_INT(vc);
        if (c == 0) {
            XrValue exc = xr_exception_newf(coro->isolate, XR_ERR_DIV_BY_ZERO, "division by zero");
            coro->jit_ctx->exception = (void *) exc.ptr;
            return XR_JIT_NULL();
        }
        XrValue r;
        r.descriptor = 0;
        r.tag = XR_TAG_I64;
        r.i = xr_int_mod_wrap(b, c);
        return XR_JIT_RESULT(r);
    }
    double nb = 0, nc = 0;
    if (XR_TONUMBER(vb, nb) && XR_TONUMBER(vc, nc)) {
        if (nc == 0.0) {
            XrValue exc = xr_exception_newf(coro->isolate, XR_ERR_DIV_BY_ZERO, "division by zero");
            coro->jit_ctx->exception = (void *) exc.ptr;
            return XR_JIT_NULL();
        }
        XrValue r;
        r.descriptor = 0;
        r.tag = XR_TAG_F64;
        r.f = fmod(nb, nc);
        int64_t raw;
        memcpy(&raw, &r.f, sizeof(double));
        return (XrJitResult) {raw, XR_TAG_F64};
    }
    XrValue exc2 = xr_exception_newf(coro->isolate, XR_ERR_DIV_BY_ZERO, "division by zero");
    coro->jit_ctx->exception = (void *) exc2.ptr;
    return XR_JIT_NULL();
}

// Called from JIT via CALL_C for OP_TYPEOF.
// jit_call_args[0] = value raw payload
// extra_arg = xr_tag of the value
XrJitResult xr_jit_typeof(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    int64_t raw = coro->jit_ctx->call_args[0];
    XrValue val = jit_value_from_tag(raw, coro->jit_ctx->call_arg_tags[0]);
    return XR_JIT_INT((int64_t) xr_value_typeid(val));
}

// Called from JIT via CALL_C for OP_PRINT.
// jit_call_args[0] = value raw payload
// extra_arg encoding: [15:8]=val_xr_tag, bit1=add_space, bit0=newline
XrJitResult xr_jit_print(XrCoroutine *coro, int64_t extra_arg) {
    int newline = (int) (extra_arg & 1);
    int add_space = (int) ((extra_arg >> 1) & 1);

    int64_t raw = coro->jit_ctx->call_args[0];
    XrValue val = jit_value_from_tag(raw, coro->jit_ctx->call_arg_tags[0]);

    if (add_space)
        printf(" ");

    XrayIsolate *isolate = coro->isolate;
    if (isolate) {
        XrString *s = xr_value_to_string(isolate, val);
        if (s)
            printf("%s", s->data);
    }
    if (newline)
        printf("\n");
    return XR_JIT_OK();
}

/* ========== Structured Concurrency Helpers ========== */

// Called from JIT via CALL_C for OP_SCOPE_ENTER.
// Creates a new scope context and pushes it onto the coro's scope stack.
// Returns 0.
XrJitResult xr_jit_scope_enter(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    if (!coro)
        return XR_JIT_OK();

    atomic_store(&coro->wait_count, 0);
    atomic_store(&coro->any_done, false);

    XrScopeContext *scope = (XrScopeContext *) xr_malloc(sizeof(XrScopeContext));
    if (scope) {
        atomic_store(&scope->count, 0);
        // JIT only handles the WAIT mode here; LINKED / SUPERVISOR
        // scopes deopt to the interpreter (see OP_SCOPE_ENTER in
        // xvm_dispatch_misc.inc.c) which is the path that allocates
        // errors[] eagerly for the supervisor case.
        scope->mode = XR_SCOPE_WAIT;
        atomic_init(&scope->cancel_requested, false);
        atomic_init(&scope->child_lock, false);
        scope->first_error = xr_null();
        scope->errors = NULL;
        scope->first_child = NULL;
        scope->parent = coro->current_scope;
        scope->owner = coro;
        coro->current_scope = scope;
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_SCOPE_EXIT.
// If all children done (wait_count==0): unlinks and frees scope, returns 0.
// If children still running: returns XM_DEOPT_MARKER to deopt back to interpreter.
XrJitResult xr_jit_scope_exit(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    if (!coro)
        return XR_JIT_OK();

    XrScopeContext *scope = coro->current_scope;
    if (!scope)
        return XR_JIT_OK();

    if (atomic_load(&scope->count) > 0) {
        // Children still running — deopt to interpreter for blocking wait
        return (XrJitResult) {XM_DEOPT_MARKER, 0};
    }

    // All children done — safe to unlink and free scope
    coro->current_scope = scope->parent;
    xr_free(scope);
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_GO.
// jit_call_args[0] = closure raw (XrValue tagged as PTR)
// jit_call_args[1..nargs] = argument values (raw payloads)
// extra_arg = nargs | (fire_and_forget << 7)
// Creates the coroutine, sets up scope, schedules via xr_runtime_spawn.
// Returns the coroutine XrValue raw payload (for R[A]).
// Note: skips continuation stealing for simplicity — coro scheduled normally.
XrJitResult xr_jit_go(XrCoroutine *coro, int64_t extra_arg) {
    int c_raw = (int) (extra_arg & 0xFF);
    bool fire_and_forget = (c_raw & 0x80) != 0;
    int nargs = c_raw & 0x7F;

    XrValue fn_val = jit_value_from_tag(coro->jit_ctx->call_args[0], XR_TAG_PTR);
    if (!xr_value_is_closure(fn_val))
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    struct XrClosure *closure = xr_value_to_closure(fn_val);
    XrProto *proto = closure->proto;

    if (!proto->is_coro_safe)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};
    if (nargs != proto->numparams)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    // Build args from call_args[1..nargs].
    // Primary tag source: call_arg_tags[] (precise compile-time tags from codegen).
    // Fallback: proto->param_types for truly unknown (any) types.
    XrValue args[16];
    for (int i = 0; i < nargs && i < 16; i++) {
        int64_t raw = coro->jit_ctx->call_args[1 + i];
        uint8_t tag = coro->jit_ctx->call_arg_tags[1 + i];
        if (tag == XR_RTAG_UNKNOWN && proto->param_types && i < proto->param_types_count &&
            proto->param_types[i])
            tag = slot_type_to_xr_tag(xr_type_to_slot_type(proto->param_types[i]));
        args[i] = jit_value_from_tag(raw, tag);
    }

    XrayIsolate *isolate = coro->isolate;
    if (!isolate)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    XrCoroutine *child = xr_coro_create(isolate, closure, args, nargs, NULL, NULL, 0);
    if (!child)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    // Create Task handle (mirrors vm_spawn)
    XrTask *task = xr_task_create(coro, child);
    if (!task)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};

    if (fire_and_forget)
        child->gc_flags |= XR_CORO_GC_RECYCLABLE;

    // Setup scope tracking
    XrScopeContext *scope = coro->current_scope;
    if (scope) {
        child->parent_scope = scope;
        atomic_fetch_add_explicit(&scope->count, 1, memory_order_relaxed);
    }

    // Schedule the coroutine (no continuation stealing in JIT path)
    XrRuntime *runtime = (XrRuntime *) xr_isolate_get_vm_state(isolate)->runtime;
    if (!runtime)
        return (XrJitResult) {XM_DEOPT_MARKER, 0};
    xr_runtime_spawn(runtime, child);

    XrValue result = xr_value_from_task(task);
    return XR_JIT_VAL(result);
}

// Called from JIT via CALL_C for OP_AWAIT.
// jit_call_args[0] = coro raw (XrValue tagged as PTR)
// extra_arg = discard_result flag
// Fast path: if Task is done, return result immediately.
// Slow path: return XM_DEOPT_MARKER to trigger deopt to interpreter.
// All await coordination lives on XrTask; raw coro await no longer supported.
XrJitResult xr_jit_await(XrCoroutine *coro, int64_t extra_arg) {
    int discard_result = (int) (extra_arg & 0xFF);

    int64_t raw0 = coro->jit_ctx->call_args[0];
    XrValue val = jit_value_from_tag(raw0, XR_TAG_PTR);

    // Task fast path: return result if already done
    if (xr_value_is_task(val)) {
        XrTask *task = xr_value_to_task(val);
        uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
        if (tstate == XR_TASK_COMPLETED) {
            XrValue res = discard_result ? xr_null()
                                         : xr_deep_copy_to_coro(coro->isolate, task->result, coro);
            return XR_JIT_RESULT(res);
        }
        if (tstate == XR_TASK_FAILED || tstate == XR_TASK_CANCELLED) {
            return XR_JIT_NULL();
        }
    }

    // Not done or not a Task → deopt to interpreter for blocking wait
    return (XrJitResult) {XM_DEOPT_MARKER, 0};
}

/*
 * JIT suspend helper for AWAIT blocking path.
 *
 * Called from XM_SUSPEND codegen AFTER live registers are saved to
 * coro->jit_suspend-> Performs the CAS coordination on Task to
 * register this coro as waiter.
 *
 * The Task ptr was stored in jit_call_args[0] by the preceding CALL_C
 * (xr_jit_await) which returned DEOPT_MARKER to signal "not done".
 * extra_arg encodes: bits[0:7] = discard_result.
 *
 * Returns:
 *   0 → blocked (JIT returns SUSPEND_MARKER, worker yields coro)
 *   1 → race: task completed during CAS, result in jit_suspend.result
 *       (JIT reloads regs and continues inline)
 */
XrJitResult xr_jit_await_block(XrCoroutine *coro, int64_t extra_arg) {
    int discard_result = (int) (extra_arg & 0xFF);

    /* Retrieve Task pointer from the preceding xr_jit_await call.
     * The fast-path helper stored it in call_args[0]. */
    int64_t raw0 = coro->jit_ctx->call_args[0];
    XrValue val = jit_value_from_tag(raw0, XR_TAG_PTR);
    if (!xr_value_is_task(val))
        return (XrJitResult) {0, 0};  // safety fallback
    XrTask *task = xr_value_to_task(val);

    // Re-check: task may have completed between fast-path and here
    uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
    if (tstate == XR_TASK_COMPLETED || tstate == XR_TASK_FAILED || tstate == XR_TASK_CANCELLED) {
        // Race win: result ready — store into suspend_regs[23] for inline resume
        XrValue res = xr_null();
        if (tstate == XR_TASK_COMPLETED && !discard_result) {
            res = xr_deep_copy_to_coro(coro->isolate, task->result, coro);
        }
        coro->jit_suspend->result = res.i;
        coro->jit_suspend->result_tag = res.tag;
        return (XrJitResult) {1, 0};  // not blocked — JIT continues
    }

    // CAS: NONE → WAITING (mirror vm_await logic in xvm_coro_ops.c:2439)
    atomic_store_explicit((_Atomic int *) &task->waiter_index, -1, memory_order_relaxed);
    atomic_store_explicit((_Atomic(XrCoroutine *) *) &task->waiter, coro, memory_order_release);

    int expected = XR_AWAIT_NONE;
    if (atomic_compare_exchange_strong_explicit(&task->await_state, &expected, XR_AWAIT_WAITING,
                                                memory_order_acq_rel, memory_order_acquire)) {
        /* Successfully registered as waiter — coro will be woken by
         * xr_task_wake_waiter when executor completes. */
        atomic_store_explicit(&coro->await_task, task, memory_order_release);
        uint32_t old_flags = xr_coro_flags_load(coro);
        uint32_t new_flags =
            xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
        atomic_store_explicit(&coro->flags, new_flags, memory_order_release);
        return (XrJitResult) {0, 0};  // blocked — JIT saves resume info and returns SUSPEND_MARKER
    }

    if (expected == XR_AWAIT_RESOLVED) {
        /* Race: executor completed between our state check and CAS.
         * Result already in task->result. */
        atomic_store_explicit((_Atomic(XrCoroutine *) *) &task->waiter, NULL, memory_order_relaxed);
        XrValue res = xr_null();
        if (!discard_result) {
            res = xr_deep_copy_to_coro(coro->isolate, task->result, coro);
        }
        coro->jit_suspend->result = res.i;
        coro->jit_suspend->result_tag = res.tag;
        atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
        return (XrJitResult) {1, 0};  // not blocked — JIT continues
    }

    // XR_AWAIT_WAITING: concurrent await on same task (overwrite waiter)
    atomic_store_explicit(&coro->await_task, task, memory_order_release);
    uint32_t old_flags2 = xr_coro_flags_load(coro);
    uint32_t new_flags2 =
        xr_coro_set_wait_reason_flags(old_flags2, XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
    atomic_store_explicit(&coro->flags, new_flags2, memory_order_release);
    return (XrJitResult) {0, 0};  // blocked
}
