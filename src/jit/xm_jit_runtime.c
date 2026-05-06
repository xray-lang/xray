/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_jit_runtime.c - JIT C-bridge runtime helpers
 *
 * KEY CONCEPT:
 *   C functions callable from JIT-compiled ARM64 code via CALL_C stubs.
 *   All value-returning helpers use the XrJitResult convention
 *   (x0=payload, x1=tag) to avoid global side-effect channels.
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
#include "../runtime/object/xshape.h"
#include "../runtime/object/xrange.h"
#include "../runtime/object/xset.h"
#include "../runtime/object/xstringbuilder.h"
#include "../runtime/object/xiterator.h"
#include "../runtime/object/xexception.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xenum.h"
#include "../runtime/value/xstruct_layout.h"
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
#include "../coro/xtask.h"
#include "../coro/xworker.h"
#include "../coro/xcoro_pool.h"
#include "../vm/xvm.h"
#include "../vm/xvm_internal.h"
#include "../runtime/value/xmethod_table.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../module/xmodule.h"
#include "xm_codegen.h"
#include "xm_jit_debug.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

XrJitResult xr_jit_call_self(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrProto *proto = (XrProto *) coro->jit_ctx->call_proto;
    if (!proto || !proto->jit_entry)
        return (XrJitResult){XM_DEOPT_MARKER, 0};

        // Recursive JIT call: same calling convention (coro, args_ptr)
#ifdef _WIN32
    int64_t payload = ((XmJitFn) proto->jit_entry)((intptr_t) coro, coro->jit_ctx->call_args);
    return (XrJitResult){payload, (uint64_t) coro->jit_ctx->call_result_tag};
#else
    return ((XmJitFn) proto->jit_entry)((intptr_t) coro, coro->jit_ctx->call_args);
#endif
}

/* ========== JIT Yield Frame Pre-push ========== */

/*
 * Read a GP register value from CALL_C stub saved area or JIT frame.
 *
 * During CALL_C, caller-saved regs (x1-x15) are saved to the C stack
 * at safepoint_saved_sp. Callee-saved regs (x20-x27) are on the JIT
 * frame at jit_frame_sp (saved by prologue).
 *
 * CALL_C stub layout (safepoint_saved_sp):
 *   +0: x1,x2  +16: x3,x4  +32: x5,x6  +48: x7,x8
 *   +64: x9,x10  +80: x11,x12  +96: x13,x14  +112: x15,LR
 *
 * JIT frame layout (jit_frame_sp):
 *   +0: FP,LR  +16: x19,x20  +32: x21,x22  +48: x23,x24
 *   +64: x25,x26  +80: x27,x28
 */
static inline int64_t jit_read_saved_gp(XrJitScratch *ctx, uint8_t phys_reg) {
    int64_t *saved_sp = (int64_t *) ctx->safepoint_saved_sp;
    int64_t *frame_sp = (int64_t *) ctx->jit_frame_sp;
    if (phys_reg >= 1 && phys_reg <= 15 && saved_sp)
        return saved_sp[phys_reg - 1];
    if (phys_reg >= 19 && phys_reg <= 27 && frame_sp)
        return frame_sp[2 + (phys_reg - 19)];
    return 0;
}

/*
 * Read an FP register value from CALL_C stub saved area or JIT frame.
 *
 * CALL_C stub: d0-d7 at safepoint_saved_sp + 128 + d*8
 * JIT frame:   d8-d15 at jit_frame_sp + 96 + (d-8)*8
 */
static inline int64_t jit_read_saved_fp(XrJitScratch *ctx, uint8_t phys_reg) {
    int64_t *saved_sp = (int64_t *) ctx->safepoint_saved_sp;
    int64_t *frame_sp = (int64_t *) ctx->jit_frame_sp;
    if (phys_reg <= 7 && saved_sp)
        return saved_sp[16 + phys_reg];
    if (phys_reg >= 8 && phys_reg <= 15 && frame_sp)
        return frame_sp[12 + (phys_reg - 8)];
    return 0;
}

/*
 * Pre-push an interpreter frame and populate the VM stack from JIT
 * register state. Used by JIT yield: after try-mode returns
 * WOULD_BLOCK, this function reconstructs the interpreter state so the
 * yieldable can be called in normal mode with a proper frame for
 * yield_setup_frame to operate on.
 *
 * Returns the bc_pc of the OP_INVOKE instruction, or -1 on failure.
 */
static int32_t jit_prepush_yield_frame(XrCoroutine *coro, uint32_t deopt_id) {
    XrJitScratch *jctx = coro->jit_ctx;
    XrProto *proto = (XrProto *) jctx->call_proto;
    if (!proto || !proto->deopt_table)
        return -1;
    if (deopt_id >= proto->ndeopt)
        return -1;

    XmRtDeoptEntry *entry = &((XmRtDeoptEntry *) proto->deopt_table)[deopt_id];
    XrayIsolate *isolate = coro->isolate;
    XrVMContext *vm = &isolate->vm_ctx;
    int32_t base_off = jctx->call_base_offset;
    int maxstack = proto->maxstacksize;

    // Bounds checks
    if (base_off + maxstack > vm->stack_capacity)
        return -1;
    if (vm->frame_count >= vm->frame_capacity)
        return -1;

    // Populate VM stack slots from saved register areas
    XrValue *frame_base = vm->stack + base_off;
    for (uint16_t i = 0; i < entry->nslots; i++) {
        XmRtDeoptSlot *s = &entry->slots[i];
        int bc = s->bc_slot;
        if (bc < 0 || bc >= maxstack)
            continue;

        int64_t raw = 0;
        switch (s->loc_kind) {
            case DEOPT_LOC_REG:
                raw = jit_read_saved_gp(jctx, s->loc.phys_reg);
                break;
            case DEOPT_LOC_FP_REG:
                raw = jit_read_saved_fp(jctx, s->loc.phys_reg);
                break;
            case DEOPT_LOC_SPILL: {
                // Read from deopt_spill_save[] — safe copy made before epilogue.
                int16_t slot = (int16_t) (s->loc.spill_offset / 8);
                if (slot >= 0 && slot < 32)
                    raw = jctx->deopt_spill_save[slot];
                break;
            }
            case DEOPT_LOC_CONST_I64:
                raw = s->loc.const_i64;
                break;
            case DEOPT_LOC_CONST_F64:
                memcpy(&raw, &s->loc.const_f64, sizeof(double));
                break;
            case DEOPT_LOC_CONST_PTR:
                raw = (int64_t) (intptr_t) s->loc.const_ptr;
                break;
            default:
                continue;
        }
        // Resolve xr_tag: prefer compile-time tag, fallback to runtime tag
        uint8_t tag = s->xr_tag;
        if (tag == XR_RTAG_UNKNOWN && bc >= 0 && bc < 256) {
            uint8_t rt = jctx->slot_runtime_tags[bc];
            if (rt != 0 && rt != XR_RTAG_UNKNOWN)
                tag = rt;
        }
        frame_base[bc] = deopt_reconstruct(raw, s->type, tag);
    }

    // Pre-push interpreter frame
    XrBcCallFrame *nf = &vm->frames[vm->frame_count++];
    nf->closure = (XrClosure *) jctx->call_closure;
    nf->pc = PROTO_CODE_BASE(proto) + entry->bc_pc + 1;  // next instruction
    nf->base_offset = base_off;
    nf->result_offset = 0;
    nf->flags = 0;
    nf->call_status = 0;
    nf->u.c.has_cfunc_result = false;
    nf->u.c.cfunc_result = xr_null();

    // Decode result_slot from the OP_INVOKE instruction at bc_pc
    XrInstruction invoke_inst = PROTO_CODE_BASE(proto)[entry->bc_pc];
    nf->u.c.result_slot = (int16_t) GETARG_A(invoke_inst);

    return (int32_t) entry->bc_pc;
}

/* ========== Method Invocation Bridge ========== */

// Called from JIT code via CALL_C for OP_INVOKE.
// Type hints for IC-guided fast dispatch in xr_jit_invoke_method.
// Encoded in bits 8-15 of the encoded parameter.
#define JIT_TYPE_HINT_NONE 0
#define JIT_TYPE_HINT_INT 1
#define JIT_TYPE_HINT_FLOAT 2
#define JIT_TYPE_HINT_BOOL 3
#define JIT_TYPE_HINT_STRING 4
#define JIT_TYPE_HINT_ARRAY 5
#define JIT_TYPE_HINT_MAP 6
#define JIT_TYPE_HINT_SET 7
#define JIT_TYPE_HINT_JSON 8
#define JIT_TYPE_HINT_INSTANCE 9
#define JIT_TYPE_HINT_ITERATOR 10
#define JIT_TYPE_HINT_CLASS 11

// Classify receiver XrValue into JIT_TYPE_HINT_* constant.
// Used on IC miss (no hint or hint mismatch) to determine dispatch target.
static int jit_classify_receiver(XrValue receiver) {
    if (XR_IS_INT(receiver))
        return JIT_TYPE_HINT_INT;
    if (XR_IS_FLOAT(receiver))
        return JIT_TYPE_HINT_FLOAT;
    if (XR_IS_BOOL(receiver))
        return JIT_TYPE_HINT_BOOL;
    if (XR_IS_STRING(receiver))
        return JIT_TYPE_HINT_STRING;
    if (XR_IS_ARRAY(receiver))
        return JIT_TYPE_HINT_ARRAY;
    if (XR_IS_MAP(receiver))
        return JIT_TYPE_HINT_MAP;
    if (XR_IS_SET(receiver))
        return JIT_TYPE_HINT_SET;
    if (xr_value_is_json(receiver))
        return JIT_TYPE_HINT_JSON;
    if (xr_value_is_instance(receiver))
        return JIT_TYPE_HINT_INSTANCE;
    if (XR_IS_ITERATOR(receiver))
        return JIT_TYPE_HINT_ITERATOR;
    if (xr_value_is_class(receiver))
        return JIT_TYPE_HINT_CLASS;
    return JIT_TYPE_HINT_NONE;
}

// Build call_args array with receiver as this[0] for instance/class method calls.
static inline void jit_build_this_args(XrValue *call_args, XrValue receiver, XrValue *args,
                                       int nargs) {
    call_args[0] = receiver;
    for (int i = 0; i < nargs && i < 8; i++)
        call_args[1 + i] = args[i];
}

// call_args[0] = receiver payload, call_args[1..n] = arg payloads.
// call_arg_tags[0..n] = per-slot XR_TAG_* bytes (set by codegen's emit_call_args_from_pool).
// encoded = (method_symbol << 32) | (deopt_id << 16) | (type_hint << 8) | nargs.
// deopt_id: builder-provided deopt snapshot index for yieldable recovery.
// type_hint: IC-derived receiver type (0=none, classify at runtime).
XrJitResult xr_jit_invoke_method(XrCoroutine *coro, int64_t encoded) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate)
        return XR_JIT_NULL();

    int method_symbol = (int) (encoded >> 32);
    int deopt_id = (int) ((encoded >> 16) & 0xFFFF);
    int type_hint = (int) ((encoded >> 8) & 0xFF);
    int nargs = (int) (encoded & 0xFF);

    // Reconstruct receiver and args from call_args[] payloads and
    // call_arg_tags[] type bytes. The codegen stores per-byte tags in
    // call_arg_tags[] (compile-time known or dynamically patched from
    // slot_runtime_tags[] at each call site).
    uint8_t recv_tag = coro->jit_ctx->call_arg_tags[0];
    // Invoke receivers are always heap objects (module, instance, class, etc.).
    // When the compile-time tag is unknown, reconstruct as PTR to avoid
    // misclassifying a pointer payload as integer.
    if (recv_tag == XR_RTAG_UNKNOWN || recv_tag == XR_RTAG_NUMERIC)
        recv_tag = XR_TAG_PTR;
    XrValue receiver = jit_value_from_tag(coro->jit_ctx->call_args[0], recv_tag);
    XrValue args[16];
    for (int i = 0; i < nargs && i < 15; i++) {
        int64_t raw = coro->jit_ctx->call_args[1 + i];
        uint8_t tag = coro->jit_ctx->call_arg_tags[1 + i];
        args[i] = jit_value_from_tag(raw, tag);
    }

    // Determine receiver type for dispatch.
    // Always verify hint against actual receiver — static type info can be
    // stale when bytecode registers are reused across different code regions.
    int recv_type = type_hint;
    if (recv_type == JIT_TYPE_HINT_NONE) {
        recv_type = jit_classify_receiver(receiver);
    } else {
        // Validate PTR-based hints against actual GC type
        int actual = jit_classify_receiver(receiver);
        if (actual != recv_type)
            recv_type = actual;
    }

    // Unified dispatch — single switch covers all types
    XrValue result = XR_NULL_VAL;
    switch (recv_type) {
        case JIT_TYPE_HINT_INT: {
            const XrMethodSlot *slot =
                xr_method_table_lookup(XR_TID_INT, method_symbol, SYMBOL_BUILTIN_COUNT);
            result = slot ? slot->fn(isolate, receiver, args, nargs) : XR_NOTFOUND;
            break;
        }
        case JIT_TYPE_HINT_FLOAT: {
            const XrMethodSlot *slot =
                xr_method_table_lookup(XR_TID_FLOAT, method_symbol, SYMBOL_BUILTIN_COUNT);
            result = slot ? slot->fn(isolate, receiver, args, nargs) : XR_NOTFOUND;
            break;
        }
        case JIT_TYPE_HINT_BOOL: {
            /* Bool dispatches through the unified method table; missing
             * symbols return XR_NOTFOUND and let the post-switch
             * "method not found" path produce a uniform diagnostic. */
            const XrMethodSlot *slot =
                xr_method_table_lookup(XR_TID_BOOL, method_symbol, SYMBOL_BUILTIN_COUNT);
            result = slot ? slot->fn(isolate, receiver, args, nargs) : XR_NOTFOUND;
            break;
        }
        case JIT_TYPE_HINT_STRING: {
            const XrMethodSlot *slot =
                xr_method_table_lookup(XR_TID_STRING, method_symbol, SYMBOL_BUILTIN_COUNT);
            result = slot ? slot->fn(isolate, receiver, args, nargs) : XR_NOTFOUND;
            break;
        }
        case JIT_TYPE_HINT_ARRAY: {
            const XrMethodSlot *slot =
                xr_method_table_lookup(XR_TID_ARRAY, method_symbol, SYMBOL_BUILTIN_COUNT);
            result = slot ? slot->fn(isolate, receiver, args, nargs) : XR_NOTFOUND;
            break;
        }
        case JIT_TYPE_HINT_MAP: {
            const XrMethodSlot *slot =
                xr_method_table_lookup(XR_TID_MAP, method_symbol, SYMBOL_BUILTIN_COUNT);
            result = slot ? slot->fn(isolate, receiver, args, nargs) : XR_NOTFOUND;
            break;
        }
        case JIT_TYPE_HINT_SET: {
            const XrMethodSlot *slot =
                xr_method_table_lookup(XR_TID_SET, method_symbol, SYMBOL_BUILTIN_COUNT);
            result = slot ? slot->fn(isolate, receiver, args, nargs) : XR_NOTFOUND;
            break;
        }
        case JIT_TYPE_HINT_JSON: {
            const XrMethodSlot *slot =
                xr_method_table_lookup(XR_TID_JSON, method_symbol, SYMBOL_BUILTIN_COUNT);
            result = slot ? slot->fn(isolate, receiver, args, nargs) : XR_NOTFOUND;
            break;
        }
        case JIT_TYPE_HINT_ITERATOR: {
            XrIterator *iter = xr_value_to_iterator(receiver);
            if (iter) {
                if (method_symbol == SYMBOL_HASNEXT) {
                    result = xr_bool(xr_iterator_has_next(iter));
                } else if (method_symbol == SYMBOL_NEXT) {
                    result = xr_iterator_next(iter);
                }
            }
            break;
        }
        case JIT_TYPE_HINT_INSTANCE: {
            XrInstance *inst = xr_value_to_instance(receiver);
            if (!inst) {
                // Fallback: try direct cast if receiver is a valid pointer
                if (XR_IS_PTR(receiver) && receiver.ptr) {
                    XrGCHeader *hdr = (XrGCHeader *) receiver.ptr;
                    if (hdr->type == XR_TINSTANCE)
                        inst = (XrInstance *) receiver.ptr;
                }
                if (!inst)
                    break;
            }
            XrMethod *method = xr_class_lookup_method(inst->klass, method_symbol);
            if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                XrValue call_args[9];
                jit_build_this_args(call_args, receiver, args, nargs);
                result = method->as.primitive(isolate, call_args, nargs + 1);
            } else if (method && method->type == XMETHOD_CLOSURE && method->as.closure) {
                XrValue call_args[9];
                jit_build_this_args(call_args, receiver, args, nargs);
                bool saved = xr_isolate_get_suppress_exception_print(isolate);
                xr_isolate_set_suppress_exception_print(isolate, true);
                result = xr_vm_call_closure(isolate, method->as.closure, call_args, nargs + 1);
                xr_isolate_set_suppress_exception_print(isolate, saved);
                if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
                    coro->jit_ctx->exception = (void *) coro->vm_ctx.current_exception.ptr;
                    coro->vm_ctx.current_exception = XR_NULL_VAL;
                    return XR_JIT_NULL();
                }
            }
            break;
        }
        case JIT_TYPE_HINT_CLASS: {
            // Constructor call: new ClassName(args...)
            XrClass *cls = xr_value_to_class(receiver);
            if (cls) {
                XrInstance *inst = xr_instance_new(isolate, cls);
                if (!inst)
                    break;
                XrValue inst_val = xr_value_from_instance(inst);
                XrMethod *ctor = xr_class_lookup_method(cls, method_symbol);
                if (ctor && ctor->type == XMETHOD_CLOSURE && ctor->as.closure) {
                    // Call constructor with instance as this
                    XrValue call_args[9];
                    call_args[0] = inst_val;
                    for (int i = 0; i < nargs && i < 8; i++)
                        call_args[1 + i] = args[i];
                    bool saved = xr_isolate_get_suppress_exception_print(isolate);
                    xr_isolate_set_suppress_exception_print(isolate, true);
                    xr_vm_call_closure(isolate, ctor->as.closure, call_args, nargs + 1);
                    xr_isolate_set_suppress_exception_print(isolate, saved);
                    if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
                        coro->jit_ctx->exception = (void *) coro->vm_ctx.current_exception.ptr;
                        coro->vm_ctx.current_exception = XR_NULL_VAL;
                        return XR_JIT_NULL();
                    }
                } else if (ctor && ctor->type == XMETHOD_PRIMITIVE && ctor->as.primitive) {
                    result = ctor->as.primitive(isolate, args, nargs);
                    break;
                }
                // Return instance (constructor modifies it in-place)
                result = inst_val;
            }
            break;
        }
        default: {
            // Generic PTR fallback: try native type class lookup
            if (XR_IS_PTR(receiver) && receiver.ptr) {
                XrGCHeader *hdr = (XrGCHeader *) receiver.ptr;
                // Module receiver: look up exported function and call it
                if (hdr->type == XR_TMODULE) {
                    XrModule *mod = (XrModule *) receiver.ptr;
                    XrValue export_val = xr_module_get_sym(mod, method_symbol);
                    // Use already-reconstructed args[] (bitmap-tagged, precise)
                    if (XR_IS_PTR(export_val) && export_val.ptr) {
                        XrGCHeader *fn_hdr = (XrGCHeader *) export_val.ptr;
                        if (fn_hdr->type == XR_TCFUNCTION) {
                            XrCFunction *cfunc = (XrCFunction *) export_val.ptr;
                            if (cfunc->is_yieldable) {
                                /* Try-mode fast path: if IO is ready, completes
                                 * inline (zero deopt). */
                                coro->jit_try_mode = true;
                                XrValue try_result;
                                XrCFuncResult status =
                                    cfunc->as.yieldable(isolate, args, nargs + 1, &try_result);
                                coro->jit_try_mode = false;

                                if (status == XR_CFUNC_DONE) {
                                    return (XrJitResult){try_result.i, 0};
                                }

                                /* WOULD_BLOCK: pre-push interpreter frame and
                                 * call yieldable in normal mode. Avoids deopt +
                                 * interpreter re-execution of OP_INVOKE. If
                                 * pre-push fails, fall back to the deopt path
                                 * below. */
                                if (jit_prepush_yield_frame(coro, (uint32_t) deopt_id) >= 0) {
                                    XrValue normal_result;
                                    XrCFuncResult st2 = cfunc->as.yieldable(
                                        isolate, args, nargs + 1, &normal_result);

                                    if (st2 == XR_CFUNC_DONE) {
                                        /* IO became ready between try-mode and
                                         * normal call. Pop the pre-pushed frame
                                         * and return result to JIT. */
                                        isolate->vm_ctx.frame_count--;
                                        return (XrJitResult){normal_result.i, 0};
                                    }
                                    /* BLOCKED/YIELD: yield_setup_frame has set up
                                     * continuation on the pre-pushed frame. Signal
                                     * VM to skip deopt recovery and return the
                                     * appropriate result directly. */
                                    coro->jit_ctx->yield_frame_pushed = true;
                                    coro->jit_ctx->yield_vm_result = (uint8_t) st2;
                                }

                                /* Trigger deopt exit (deopt stub tears down JIT
                                 * frame). VM checks yield_frame_pushed to decide
                                 * whether to do normal deopt recovery or return
                                 * BLOCKED/YIELD immediately. */
                                coro->jit_ctx->invoke_deopt_id = (uint32_t) deopt_id;
                                coro->jit_ctx->deopt_id = UINT32_MAX;
                                return (XrJitResult){XM_DEOPT_MARKER, 0};
                            }
                            result = cfunc->as.func(isolate, args, nargs);
                        } else if (fn_hdr->type == XR_TFUNCTION) {
                            // Script closure: call via VM
                            XrClosure *fn = (XrClosure *) export_val.ptr;
                            bool saved = xr_isolate_get_suppress_exception_print(isolate);
                            xr_isolate_set_suppress_exception_print(isolate, true);
                            result = xr_vm_call_closure(isolate, fn, args, nargs);
                            xr_isolate_set_suppress_exception_print(isolate, saved);
                            if (!XR_IS_NULL(coro->vm_ctx.current_exception)) {
                                coro->jit_ctx->exception =
                                    (void *) coro->vm_ctx.current_exception.ptr;
                                coro->vm_ctx.current_exception = XR_NULL_VAL;
                                return XR_JIT_NULL();
                            }
                        }
                    }
                    break;
                }
                XrClass *klass = xr_value_get_class(isolate, receiver);
                if (klass) {
                    XrMethod *method = xr_class_lookup_method(klass, method_symbol);
                    if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                        XrValue call_args[9];
                        jit_build_this_args(call_args, receiver, args, nargs);
                        result = method->as.primitive(isolate, call_args, nargs + 1);
                    }
                }
            }
            break;
        }
    }

    return XR_JIT_RESULT(result);
}

/* ========== Shared Variable Access ========== */

// Load a shared (global) variable.
// extra_arg = absolute shared index (Bx + shared_offset, pre-computed by builder).
XrJitResult xr_jit_get_shared(XrCoroutine *coro, int64_t shared_index) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate)
        return XR_JIT_NULL();
    XrValue val =
        xr_shared_array_get(&xr_isolate_get_vm_state(isolate)->shared, (int) shared_index);
    return XR_JIT_RESULT(val);
}

// Store a shared (global) variable.
// extra_arg = (val_bc_slot<<24) | (val_slot_type<<16) | shared_index
// The value to store is pre-written to jit_call_args[0] by JIT code.
XrJitResult xr_jit_set_shared(XrCoroutine *coro, int64_t extra_arg) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate)
        return XR_JIT_OK();
    int shared_idx = (int) (extra_arg & 0xFFFF);
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    xr_shared_array_set(&xr_isolate_get_vm_state(isolate)->shared, shared_idx, val);
    return XR_JIT_OK();
}

/* ========== JIT Exception Handling ========== */

// Called from JIT code (via CALL_C) when OP_THROW is executed.
// Sets coro->jit_ctx->exception so JIT can branch to catch handler.
// extra_arg = raw exception value (XrValue.i payload, tag=PTR assumed).
XrJitResult xr_jit_throw(XrCoroutine *coro, int64_t exception_raw) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate) {
        coro->jit_ctx->exception = NULL;
        return XR_JIT_OK();
    }

    // Reconstruct XrValue from raw payload
    XrValue exception;
    exception.descriptor = 0;
    exception.tag = XR_TAG_PTR;
    exception.i = exception_raw;

    // Auto-wrap non-exception values
    if (!xr_is_exception(exception)) {
        exception = xr_exception_from_value(isolate, exception);
    }

    // Store exception pointer for JIT catch handler
    coro->jit_ctx->exception = (void *) exception.ptr;
    return XR_JIT_OK();
}

/* ========== Generic Index Operations ========== */

// Called from JIT via CALL_C for OP_GETPROP (shape-miss or non-Json fallback).
// jit_call_args[0] = obj raw (ptr), extra_arg = global SymbolId
XrJitResult xr_jit_getprop(XrCoroutine *coro, int64_t symbol_id) {
    XrValue obj;
    obj.ptr = (void *) coro->jit_ctx->call_args[0];
    obj.tag = XR_TAG_PTR;

    SymbolId sym = (SymbolId) (uint32_t) symbol_id;

    if (!obj.ptr || (uintptr_t) obj.ptr < 0x1000)
        return XR_JIT_NULL();
    int heap_type = XR_GC_GET_TYPE((XrGCHeader *) obj.ptr);

    // String properties
    if (heap_type == XR_TSTRING) {
        XrString *str = (XrString *) obj.ptr;
        if (sym == SYMBOL_LENGTH)
            return (XrJitResult){(int64_t) xr_string_char_length(str), XR_TAG_I64};
        if (sym == SYMBOL_BYTE_LENGTH)
            return (XrJitResult){(int64_t) str->length, XR_TAG_I64};
        if (sym == SYMBOL_IS_EMPTY)
            return (XrJitResult){str->length == 0 ? 1 : 0, XR_TAG_I64};
    }

    // Array / ArraySlice properties
    if (heap_type == XR_TARRAY || heap_type == XR_TARRAY_SLICE) {
        XrArray *arr = (XrArray *) obj.ptr;
        if (sym == SYMBOL_LENGTH)
            return (XrJitResult){(int64_t) arr->length, XR_TAG_I64};
        if (sym == SYMBOL_IS_EMPTY)
            return (XrJitResult){arr->length == 0 ? 1 : 0, XR_TAG_I64};
    }

    // Map properties
    if (heap_type == XR_TMAP) {
        XrMap *map = (XrMap *) obj.ptr;
        if (sym == SYMBOL_LENGTH)
            return (XrJitResult){(int64_t) xr_map_size(map), XR_TAG_I64};
        if (sym == SYMBOL_IS_EMPTY)
            return (XrJitResult){xr_map_is_empty(map) ? 1 : 0, XR_TAG_I64};
    }

    // Set properties
    if (heap_type == XR_TSET) {
        XrSet *set = (XrSet *) obj.ptr;
        if (sym == SYMBOL_LENGTH)
            return (XrJitResult){(int64_t) xr_set_size(set), XR_TAG_I64};
        if (sym == SYMBOL_IS_EMPTY)
            return (XrJitResult){xr_set_is_empty(set) ? 1 : 0, XR_TAG_I64};
    }

    // Enum value properties
    if (heap_type == XR_TENUM_VALUE) {
        XrEnumValue *ev = (XrEnumValue *) obj.ptr;
        if (sym == SYMBOL_VALUE)
            return XR_JIT_RESULT(ev->raw_value);
        if (sym == SYMBOL_NAME) {
            XrValue _n = xr_string_value(
                xr_string_new(coro->isolate, ev->member_name, strlen(ev->member_name)));
            return XR_JIT_RESULT(_n);
        }
        if (sym == SYMBOL_ORDINAL)
            return (XrJitResult){(int64_t) ev->member_index, XR_TAG_I64};
    }

    // Json object field access
    if (xr_value_is_json(obj)) {
        XrJson *json = xr_value_to_json(obj);
        XrValue result = xr_json_get(coro->isolate, json, sym);
        return XR_JIT_RESULT(result);
    }

    return XR_JIT_NULL();
}

// Called from JIT via CALL_C for OP_SETPROP.
// jit_call_args[0] = obj raw (ptr), jit_call_args[1] = value raw
// extra_arg = (val_bc_slot<<40) | (symbol_id<<8) | val_slot_type
XrJitResult xr_jit_setprop(XrCoroutine *coro, int64_t extra_arg) {
    SymbolId sym = (SymbolId) (uint32_t) ((extra_arg >> 8) & 0xFFFFFFFF);

    XrValue obj;
    obj.ptr = (void *) coro->jit_ctx->call_args[0];
    obj.tag = XR_TAG_PTR;

    XrValue value =
        jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);

    if (xr_value_is_json(obj)) {
        XrJson *json = xr_value_to_json(obj);
        if (!xr_json_set(coro->isolate, json, sym, value)) {
            return (XrJitResult){XM_DEOPT_MARKER, 0};
        }
    }

    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_INDEX_GET.
// jit_call_args[0] = obj raw, jit_call_args[1] = key raw
// extra_arg = (obj_slot_type << 8) | key_slot_type
XrJitResult xr_jit_index_get(XrCoroutine *coro, int64_t extra_arg) {
    XrValue obj_val =
        jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrValue key_val =
        jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);

    XrValue result = xr_null();

    if (XR_IS_PTR(obj_val) && obj_val.ptr) {
        int heap_type = XR_GC_GET_TYPE((XrGCHeader *) obj_val.ptr);

        if (heap_type == XR_TARRAY && XR_IS_INT(key_val)) {
            XrArray *arr = (XrArray *) obj_val.ptr;
            int idx = (int) key_val.i;
            if ((unsigned) idx < (unsigned) arr->length && arr->data) {
                result = (arr->elem_type == XR_ELEM_ANY) ? ((XrValue *) arr->data)[idx]
                                                         : xr_array_get_element(arr, idx);
            }
        } else if (heap_type == XR_TMAP) {
            XrMap *map = (XrMap *) obj_val.ptr;
            bool found;
            result = xr_map_get(map, key_val, &found);
            if (!found)
                result = xr_null();
        } else if (heap_type == XR_TSTRING && XR_IS_INT(key_val)) {
            XrString *str = (XrString *) obj_val.ptr;
            size_t idx = (size_t) key_val.i;
            XrString *ch = xr_string_char_at_unicode(coro->isolate, str, idx);
            result = ch ? xr_string_value(ch) : xr_null();
        } else if (heap_type == XR_TJSON && XR_IS_PTR(key_val)) {
            XrJson *json = (XrJson *) obj_val.ptr;
            XrString *key_str = (XrString *) key_val.ptr;
            result = xr_json_get_by_key(coro->isolate, json, key_str->data);
        }
    }

    return XR_JIT_RESULT(result);
}

// Called from JIT via CALL_C for OP_INDEX_SET.
// jit_call_args[0] = obj raw, jit_call_args[1] = key raw, jit_call_args[2] = val raw
// extra_arg = (val_bc_slot<<24) | (obj_st<<16) | (key_st<<8) | val_st
//   val_bc_slot: bc register slot for val — used to resolve CALLEE_SETS via slot_runtime_tags
XrJitResult xr_jit_index_set(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue obj_val =
        jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrValue key_val =
        jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    XrValue new_val =
        jit_value_from_tag(coro->jit_ctx->call_args[2], coro->jit_ctx->call_arg_tags[2]);

    if (XR_IS_PTR(obj_val) && obj_val.ptr) {
        int heap_type = XR_GC_GET_TYPE((XrGCHeader *) obj_val.ptr);

        if (heap_type == XR_TARRAY && XR_IS_INT(key_val)) {
            XrArray *arr = (XrArray *) obj_val.ptr;
            int idx = (int) key_val.i;
            if ((unsigned) idx < (unsigned) arr->length) {
                xr_array_set_element(arr, idx, new_val);
            }
        } else if (heap_type == XR_TMAP) {
            XrMap *map = (XrMap *) obj_val.ptr;
            xr_map_set(map, key_val, new_val);
        } else if (heap_type == XR_TJSON && XR_IS_PTR(key_val)) {
            XrJson *json = (XrJson *) obj_val.ptr;
            XrString *key_str = (XrString *) key_val.ptr;
            xr_json_set_by_key(coro->isolate, json, key_str->data, new_val);
        }
    }

    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_TARRAY_GET.
// jit_call_args[0] = array ptr (raw), jit_call_args[1] = index (raw i64)
// Returns raw i64 value from typed array (no tag).
XrJitResult xr_jit_tarray_get(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrArray *arr = (XrArray *) coro->jit_ctx->call_args[0];
    int64_t idx = coro->jit_ctx->call_args[1];

    if (!arr || idx < 0 || idx >= arr->length)
        return XR_JIT_NULL();

    // Typed array stores raw i64 values
    if (arr->elem_type == XR_ELEM_I64) {
        return XR_JIT_INT(((int64_t *) arr->data)[idx]);
    }
    // Regular array stores tagged XrValue - extract i64 payload
    if (arr->elem_type == XR_ELEM_ANY) {
        XrValue *elems = (XrValue *) arr->data;
        return XR_JIT_VAL(elems[idx]);
    }
    return XR_JIT_NULL();
}

// Called from JIT via CALL_C for OP_TARRAY_SET.
// jit_call_args[0] = array ptr (raw), jit_call_args[1] = index (raw i64),
// jit_call_args[2] = value (raw i64)
// extra_arg = val_slot_type (for correct tag reconstruction on ELEM_ANY arrays)
XrJitResult xr_jit_tarray_set(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrArray *arr = (XrArray *) coro->jit_ctx->call_args[0];
    int64_t idx = coro->jit_ctx->call_args[1];
    int64_t raw_val = coro->jit_ctx->call_args[2];

    if (!arr || idx < 0 || idx >= arr->length)
        return XR_JIT_OK();

    // Typed array stores raw i64 values
    if (arr->elem_type == XR_ELEM_I64) {
        ((int64_t *) arr->data)[idx] = raw_val;
    }
    // Regular array stores tagged XrValue — reconstruct with correct tag
    else if (arr->elem_type == XR_ELEM_ANY) {
        XrValue *elems = (XrValue *) arr->data;
        elems[idx] = jit_value_from_tag(raw_val, coro->jit_ctx->call_arg_tags[2]);
    }
    return XR_JIT_OK();
}

/* ========== Map Operations ========== */

// Called from JIT via CALL_C for OP_MAP_GET / OP_MAP_GETK.
// jit_call_args[0] = map/json ptr, jit_call_args[1] = key raw
// extra_arg = key_slot_type
// Handles both XrMap and XrJson objects (VM OP_MAP_GETK supports both).
XrJitResult xr_jit_map_get(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    void *obj = (void *) coro->jit_ctx->call_args[0];
    if (!obj)
        return XR_JIT_NULL();
    XrGCHeader *hdr = (XrGCHeader *) obj;
    XrValue key = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    XrValue result = xr_null();
    if (hdr->type == XR_TMAP) {
        XrMap *map = (XrMap *) obj;
        bool found;
        result = xr_map_get(map, key, &found);
        if (!found)
            result = xr_null();
    } else if (hdr->type == XR_TJSON && XR_IS_PTR(key)) {
        XrJson *json = (XrJson *) obj;
        XrString *key_str = (XrString *) key.ptr;
        result = xr_json_get_by_key(coro->isolate, json, key_str->data);
    }
    return XR_JIT_RESULT(result);
}

// Called from JIT via CALL_C for OP_MAP_SET / OP_MAP_SETK.
// jit_call_args[0] = map/json ptr, jit_call_args[1] = key raw, jit_call_args[2] = val raw
// extra_arg = (key_st << 8) | val_st
// Handles both XrMap and XrJson objects (VM OP_MAP_SETK supports both).
XrJitResult xr_jit_map_set(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    void *obj = (void *) coro->jit_ctx->call_args[0];
    if (!obj)
        return XR_JIT_OK();
    XrGCHeader *hdr = (XrGCHeader *) obj;
    XrValue key = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[2], coro->jit_ctx->call_arg_tags[2]);
    if (hdr->type == XR_TMAP) {
        XrMap *map = (XrMap *) obj;
        xr_map_set(map, key, val);
    } else if (hdr->type == XR_TJSON && XR_IS_PTR(key)) {
        XrJson *json = (XrJson *) obj;
        XrString *key_str = (XrString *) key.ptr;
        if (!xr_json_set_by_key(coro->isolate, json, key_str->data, val)) {
            return (XrJitResult){XM_DEOPT_MARKER, 0};
        }
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_MAP_INCREMENT.
// jit_call_args[0] = map ptr, jit_call_args[1] = key raw
// extra_arg = key_slot_type
XrJitResult xr_jit_map_increment(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrMap *map = (XrMap *) coro->jit_ctx->call_args[0];
    if (!map)
        return XR_JIT_OK();
    XrValue key = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    bool found;
    XrValue cur = xr_map_get(map, key, &found);
    XrValue newval;
    newval.descriptor = 0;
    newval.tag = XR_TAG_I64;
    newval.i = found ? cur.i + 1 : 1;
    xr_map_set(map, key, newval);
    return XR_JIT_OK();
}

/* ========== Builtin Access ========== */

// Called from JIT via CALL_C for OP_GETBUILTIN.
// extra_arg = builtin_index
// Returns raw i64 payload of the builtin XrValue.
XrJitResult xr_jit_getbuiltin(XrCoroutine *coro, int64_t extra_arg) {
    int idx = (int) (extra_arg & 0xFFFF);
    XrayIsolate *isolate = coro->isolate;
    if (idx < xr_isolate_get_vm_state(isolate)->builtin_count) {
        XrValue val = xr_isolate_get_vm_state(isolate)->builtins[idx];
        return XR_JIT_VAL(val);
    }
    return XR_JIT_NULL();
}

/* ========== Range Operations ========== */

// Called from JIT via CALL_C for OP_NEWRANGE.
// jit_call_args[0] = start (i64), jit_call_args[1] = end (i64)
// Returns raw ptr to XrRange.
XrJitResult xr_jit_newrange(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    int64_t start = coro->jit_ctx->call_args[0];
    int64_t end = coro->jit_ctx->call_args[1];
    XrRange *range = xr_range_new(coro, start, end);
    return XR_JIT_PTR(range);
}

// Called from JIT via CALL_C for OP_RANGE_UNPACK.
// jit_call_args[0] = range ptr
// Returns packed: count in return value, step and start stored in jit_call_args[1..2]
XrJitResult xr_jit_range_unpack(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrRange *rng = (XrRange *) coro->jit_ctx->call_args[0];
    if (!rng)
        return XR_JIT_OK();
    int64_t count = xr_range_length(rng);
    coro->jit_ctx->call_args[1] = rng->step;
    coro->jit_ctx->call_args[2] = rng->start;
    return XR_JIT_INT(count);
}

/* ========== Type Check ========== */

// Called from JIT via CALL_C for OP_IS.
// jit_call_args[0] = value raw
// extra_arg = (xr_tag << 8) | expected_type_id
XrJitResult xr_jit_is_type(XrCoroutine *coro, int64_t extra_arg) {
    int expected_tid = (int) (extra_arg & 0xFF);
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrTypeId actual_tid = xr_value_typeid(val);
    return XR_JIT_BOOL((actual_tid == (XrTypeId) expected_tid) ? 1 : 0);
}

// Called from JIT via CALL_C for OP_CHECKTYPE.
// jit_call_args[0] = value raw
// jit_call_args[1] = expected bitmask (int64)
// extra_arg = val_tag
// Returns: 1 if type matches bitmask, 0 if mismatch (triggers deopt)
XrJitResult xr_jit_checktype(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    int64_t mask = coro->jit_ctx->call_args[1];
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrTypeId actual_tid = xr_value_typeid(val);
    return XR_JIT_BOOL(((1LL << actual_tid) & mask) ? 1 : 0);
}

/* ========== String Operations ========== */

// Called from JIT via CALL_C for OP_CHR.
// jit_call_args[0] = codepoint (i64)
// Returns raw ptr to XrString (or null i64 if invalid).
XrJitResult xr_jit_chr(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    int64_t cp = coro->jit_ctx->call_args[0];
    if (cp < 0 || cp > 0x10FFFF)
        return XR_JIT_NULL();
    char buf[4];
    int len = 0;
    if (cp < 0x80) {
        buf[0] = (char) cp;
        len = 1;
    } else if (cp < 0x800) {
        buf[0] = (char) (0xC0 | (cp >> 6));
        buf[1] = (char) (0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char) (0xE0 | (cp >> 12));
        buf[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char) (0x80 | (cp & 0x3F));
        len = 3;
    } else {
        buf[0] = (char) (0xF0 | (cp >> 18));
        buf[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char) (0x80 | (cp & 0x3F));
        len = 4;
    }
    XrString *s = xr_string_intern(coro->isolate, buf, len, 0);
    return XR_JIT_PTR(s);
}

// Called from JIT via CALL_C for OP_SUBSTRING.
// jit_call_args[0] = string ptr, jit_call_args[1] = start, jit_call_args[2] = end
// Returns raw i64 of result string XrValue.
XrJitResult xr_jit_substring(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrString *str = (XrString *) coro->jit_ctx->call_args[0];
    if (!str)
        return XR_JIT_NULL();
    int start = (int) coro->jit_ctx->call_args[1];
    int end = (int) coro->jit_ctx->call_args[2];
    int slen = (int) str->length;
    if (start < 0)
        start = 0;
    if (end > slen)
        end = slen;
    if (start >= end)
        return XR_JIT_PTR(xr_string_intern(coro->isolate, "", 0, 0));
    XrString *sub = xr_string_intern(coro->isolate, str->data + start, end - start, 0);
    return XR_JIT_PTR(sub);
}

// Called from JIT via CALL_C for OP_STR_REPEAT.
// jit_call_args[0] = string ptr, jit_call_args[1] = count
// Returns raw i64 of result string XrValue.
XrJitResult xr_jit_str_repeat(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrString *str = (XrString *) coro->jit_ctx->call_args[0];
    if (!str)
        return XR_JIT_NULL();
    int count = (int) coro->jit_ctx->call_args[1];
    if (count <= 0)
        return XR_JIT_PTR(xr_string_intern(coro->isolate, "", 0, 0));
    if (count == 1)
        return XR_JIT_PTR(str);
    int slen = (int) str->length;
    int total = slen * count;
    if (total > 1024 * 1024)
        return XR_JIT_NULL();  // safety limit
    char *buf = xr_malloc(total);
    if (!buf)
        return XR_JIT_NULL();
    for (int i = 0; i < count; i++) {
        memcpy(buf + i * slen, str->data, slen);
    }
    XrString *result = xr_string_intern(coro->isolate, buf, total, 0);
    xr_free(buf);
    return XR_JIT_PTR(result);
}

// Called from JIT via CALL_C for OP_TYPENAME.
// jit_call_args[0] = value raw
// extra_arg = (slot_hint << 8) | xr_tag
// Returns raw i64 of result string XrValue.
XrJitResult xr_jit_typename(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    const char *type_name = xr_typeid_name(xr_value_typeid(val));
    size_t len = strlen(type_name);
    XrString *str = xr_string_intern(coro->isolate, type_name, len, 0);
    return XR_JIT_PTR(str);
}

// Called from JIT via CALL_C for OP_DUMP.
// jit_call_args[0] = value raw
// extra_arg = (xr_tag << 8) | depth
XrJitResult xr_jit_dump(XrCoroutine *coro, int64_t extra_arg) {
    int depth = (int) (extra_arg & 0xFF);
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    xr_value_dump(val, depth);
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for XM_RT_EQ when operands are not both numeric.
// jit_call_args[0] = value A raw, jit_call_args[1] = value B raw.
// extra_arg = (a_xr_tag << 8) | b_xr_tag
// Returns 1 if equal, 0 otherwise.
XrJitResult xr_jit_rt_eq(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    int64_t raw_a = coro->jit_ctx->call_args[0];
    int64_t raw_b = coro->jit_ctx->call_args[1];

    // Fast path 1: identical raw values → equal
    if (raw_a == raw_b)
        return XR_JIT_BOOL(1);

    uint8_t a_tag = coro->jit_ctx->call_arg_tags[0];
    uint8_t b_tag = coro->jit_ctx->call_arg_tags[1];

    // Fast path 2: both PTR tags, both aligned → compare GC types
    if (a_tag == XR_TAG_PTR && b_tag == XR_TAG_PTR && raw_a != 0 && raw_b != 0 &&
        (raw_a & 0x7) == 0 && (raw_b & 0x7) == 0) {
        XrGCHeader *ga = (XrGCHeader *) (intptr_t) raw_a;
        XrGCHeader *gb = (XrGCHeader *) (intptr_t) raw_b;
        if (ga->type != gb->type)
            return XR_JIT_BOOL(0);
    }

    XrValue a = jit_value_from_tag(raw_a, a_tag);
    XrValue b = jit_value_from_tag(raw_b, b_tag);
    return XR_JIT_BOOL(vm_values_equal(a, b) ? 1 : 0);
}

// Called from JIT via CALL_C for OP_EQK with non-numeric constants.
// jit_call_args[0] = value A raw, jit_call_args[1] = value B raw (constant).
// extra_arg = (a_slot_type << 8) | b_tag
// Returns 1 if equal, 0 otherwise.
XrJitResult xr_jit_eq_value(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t b_tag = (uint8_t) (extra_arg & 0xFF);
    XrValue a = jit_value_from_tag(coro->jit_ctx->call_args[0], coro->jit_ctx->call_arg_tags[0]);
    XrValue b;
    b.descriptor = 0;
    b.tag = b_tag;
    b.i = coro->jit_ctx->call_args[1];
    if (b_tag == XR_TAG_PTR && b.i != 0) {
        XrGCHeader *gc = (XrGCHeader *) (intptr_t) b.i;
        b.heap_type = (uint16_t) gc->type;
    }
    return XR_JIT_BOOL(vm_values_equal(a, b) ? 1 : 0);
}

// UPVAL_GET: read upvalue from current closure.
// extra_arg = upvalue index.
// Returns XrJitResult with payload and tag.
XrJitResult xr_jit_upval_get(XrCoroutine *coro, int64_t upval_index) {
    XrClosure *cl = (XrClosure *) coro->jit_ctx->call_closure;
    if (!cl || upval_index < 0 || upval_index >= cl->upval_count) {
        return XR_JIT_NULL();
    }
    XrValue val = cl->upvals[upval_index];
    return XR_JIT_VAL(val);
}

// extra_arg = child proto pointer (cast to int64_t).
// Flat upvalue model: OP_CLOSURE populates closure->upvals[] from proto descriptors.
// Returns raw closure pointer.
// Automatically populates UPVAL_SRC_UPVAL entries from enclosing closure;
// UPVAL_SRC_REG entries must be set via xr_jit_closure_set_upval afterwards.
XrJitResult xr_jit_closure_new(XrCoroutine *coro, int64_t proto_raw) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate)
        return XR_JIT_NULL();

    XrProto *child = (XrProto *) (uintptr_t) proto_raw;
    if (!child)
        return XR_JIT_NULL();

    XrClosure *closure = xr_closure_new(isolate, child, coro);
    if (!closure)
        return XR_JIT_NULL();

    // Populate UPVAL_SRC_UPVAL entries from enclosing closure
    XrClosure *enclosing = (XrClosure *) coro->jit_ctx->call_closure;
    int nuv = DYNARRAY_COUNT(&child->upvalues);
    for (int j = 0; j < nuv; j++) {
        UpvalInfo *uv = &DYNARRAY_GET(&child->upvalues, j, UpvalInfo);
        if (uv->source == UPVAL_SRC_UPVAL && enclosing) {
            int idx = uv->index;
            closure->upvals[j] =
                (idx < enclosing->upval_count) ? enclosing->upvals[idx] : xr_null();
        }
    }

    return XR_JIT_PTR(closure);
}

// Store a register value into closure->upvals[idx].
// call_args[0] = raw closure pointer, call_args[1] = raw payload.
// extra_arg = (upval_index << 8) | tag.
XrJitResult xr_jit_closure_set_upval(XrCoroutine *coro, int64_t encoded) {
    int idx = (int) ((uint64_t) encoded >> 8);
    uint8_t tag = (uint8_t) (encoded & 0xFF);
    XrClosure *cl = (XrClosure *) (uintptr_t) coro->jit_ctx->call_args[0];
    if (!cl || idx >= cl->upval_count)
        return XR_JIT_OK();

    int64_t raw = coro->jit_ctx->call_args[1];
    XrValue val = {0};
    if (tag == XR_TAG_PTR) {
        val = xr_make_ptr_val((void *) (uintptr_t) raw);
    } else {
        val.i = raw;
        val.tag = tag;
    }
    cl->upvals[idx] = val;
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_TOSTRING (JIT mode).
// jit_call_args[0] = raw value to convert.
// extra_arg = xr_tag (precise tag from builder).
// Returns raw ptr to XrString*.
XrJitResult xr_jit_tostring(XrCoroutine *coro, int64_t xr_tag_arg) {
    XrayIsolate *isolate = coro->isolate;
    if (!isolate)
        return XR_JIT_NULL();
    int64_t raw = coro->jit_ctx->call_args[0];
    uint8_t tag = coro->jit_ctx->call_arg_tags[0];
    XrValue val = jit_value_from_tag(raw, tag);
    XrString *str = xr_value_to_string(isolate, val);
    return XR_JIT_PTR(str);
}

// Called from JIT via CALL_C for OP_STRBUF_NEW (JIT mode).
// Returns raw ptr to new StringBuilder.
XrJitResult xr_jit_strbuf_new(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrStringBuilder *sb = xr_stringbuilder_new(coro);
    return XR_JIT_PTR(sb);
}

// Called from JIT via CALL_C for OP_STRBUF_APPEND (JIT mode).
// jit_call_args[0] = sb ptr, jit_call_args[1] = value raw
// extra_arg = (bc_slot_hint << 8) | val_slot_type
XrJitResult xr_jit_strbuf_append(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrStringBuilder *sb = (XrStringBuilder *) coro->jit_ctx->call_args[0];
    if (!sb)
        return XR_JIT_OK();
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[1], coro->jit_ctx->call_arg_tags[1]);
    if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        xr_stringbuilder_append_str(sb, s);
    } else if (XR_IS_INT(val)) {
        xr_stringbuilder_append_int(sb, XR_TO_INT(val));
    } else if (XR_IS_FLOAT(val)) {
        xr_stringbuilder_append_float(sb, XR_TO_FLOAT(val));
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_STRBUF_FINISH (JIT mode).
// jit_call_args[0] = sb ptr
// Returns raw i64 of result string XrValue.
XrJitResult xr_jit_strbuf_finish(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrStringBuilder *sb = (XrStringBuilder *) coro->jit_ctx->call_args[0];
    if (!sb)
        return XR_JIT_NULL();
    XrString *result = xr_stringbuilder_to_string(sb);
    return XR_JIT_PTR(result);
}

/* ========== Set Operations ========== */

// Called from JIT via CALL_C for OP_NEWSET.
// extra_arg = (elem_tid << 2) | flags
// Returns raw ptr to XrSet.
XrJitResult xr_jit_newset(XrCoroutine *coro, int64_t extra_arg) {
    (void) extra_arg;
    XrSet *set = xr_set_new(coro);
    if (!set)
        return XR_JIT_NULL();
    return XR_JIT_PTR(set);
}

/* ========== Field Access ========== */

// Called from JIT via CALL_C for OP_GETFIELD_IC.
// jit_call_args[0] = instance ptr
// extra_arg = field_name constant index (encoded as raw string ptr)
// Returns raw i64 payload of the field value.
XrJitResult xr_jit_getfield_ic(XrCoroutine *coro, int64_t extra_arg) {
    XrInstance *inst = (XrInstance *) coro->jit_ctx->call_args[0];
    if (!inst) {
        return XR_JIT_NULL();
    }
    XrString *field_name = (XrString *) (uintptr_t) extra_arg;
    if (!field_name) {
        return XR_JIT_NULL();
    }
    XrClass *cls = xr_instance_get_class(inst);
    int idx = xr_class_lookup_field_by_name(coro->isolate, cls, field_name->data);
    if (idx < 0) {
        return XR_JIT_NULL();
    }
    XrValue val = inst->fields[idx];
    return XR_JIT_VAL(val);
}

/* ========== Enum Operations ========== */

// Called from JIT via CALL_C for OP_ENUM_ACCESS.
// jit_call_args[0] = enum_type ptr, jit_call_args[1] = member_index
XrJitResult xr_jit_enum_access(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrEnumType *et = (XrEnumType *) coro->jit_ctx->call_args[0];
    int idx = (int) coro->jit_ctx->call_args[1];
    if (!et || idx < 0 || (uint32_t) idx >= et->member_count) {
        return XR_JIT_NULL();
    }
    return XR_JIT_PTR(et->members[idx].instance);
}

// Called from JIT via CALL_C for OP_ENUM_NAME.
// jit_call_args[0] = enum_value ptr
XrJitResult xr_jit_enum_name(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    XrEnumValue *ev = (XrEnumValue *) coro->jit_ctx->call_args[0];
    if (!ev || !ev->member_name)
        return XR_JIT_NULL();
    size_t len = strlen(ev->member_name);
    XrString *s = xr_string_intern(coro->isolate, ev->member_name, len, 0);
    return XR_JIT_PTR(s);
}

// Called from JIT via CALL_C for OP_ENUM_CONVERT.
// jit_call_args[0] = enum_type ptr, jit_call_args[1] = value raw
// extra_arg = xr_tag
XrJitResult xr_jit_enum_convert(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t val_tag = (uint8_t) (extra_arg & 0xFF);
    XrEnumType *et = (XrEnumType *) coro->jit_ctx->call_args[0];
    if (!et) {
        return XR_JIT_NULL();
    }
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[1], val_tag);
    XrEnumValue *result = xr_enum_from_value(et, val);
    if (result) {
        return XR_JIT_PTR(result);
    }
    return XR_JIT_NULL();
}

/* ========== Bytes Operations ========== */

// Called from JIT via CALL_C for OP_BYTES_NEW.
// jit_call_args[0..nargs-1] = byte values
// extra_arg = nargs
XrJitResult xr_jit_bytes_new(XrCoroutine *coro, int64_t extra_arg) {
    int nargs = (int) (extra_arg & 0xFF);
    XrArray *arr = xr_array_new(coro);
    if (!arr)
        return XR_JIT_NULL();
    for (int i = 0; i < nargs && i < 8; i++) {
        xr_array_push(arr, XR_FROM_INT(coro->jit_ctx->call_args[i]));
    }
    return XR_JIT_PTR(arr);
}

/* ========== Struct Native Storage ========== */

// OP_NEW_STRUCT: allocate in per-frame struct_area (zero heap allocation).
// call_args[0] = class ptr (XrClass*), extra_arg = slot_offset (C operand)
// Returns raw struct_ptr (into struct_area).
// Precondition: struct_area already allocated by VM startfunc before JIT entry.
XrJitResult xr_jit_new_struct(XrCoroutine *coro, int64_t extra_arg) {
    int slot_offset = (int) extra_arg;
    XrClass *cls = (XrClass *) (uintptr_t) coro->jit_ctx->call_args[0];
    XrStructLayout *layout = cls->struct_layout;
    XR_DCHECK(layout != NULL, "xr_jit_new_struct: struct_layout required");
    int frame_idx = coro->vm_ctx.frame_count - 1;
    XR_DCHECK(coro->vm_ctx.struct_areas && coro->vm_ctx.struct_areas[frame_idx],
              "xr_jit_new_struct: struct_area not allocated");
    uint8_t *struct_ptr = coro->vm_ctx.struct_areas[frame_idx] + slot_offset * 16;
    *(XrClass **) struct_ptr = cls;
    memset(struct_ptr + 8, 0, layout->total_size);
    return XR_JIT_PTR(struct_ptr);
}

// OP_STRUCT_GET: read native field from stack-allocated struct, box to XrValue.
// call_args[0] = struct_ptr (raw), extra_arg = field_idx
// Returns XrValue.i (raw payload of boxed result).
XrJitResult xr_jit_struct_get(XrCoroutine *coro, int64_t extra_arg) {
    int field_idx = (int) extra_arg;
    uint8_t *struct_ptr = (uint8_t *) (uintptr_t) coro->jit_ctx->call_args[0];
    XrClass *cls = *(XrClass **) struct_ptr;
    XrStructLayout *layout = cls->struct_layout;
    XrStructFieldLayout *field = &layout->fields[field_idx];
    uint8_t *fp = struct_ptr + 8 + field->offset;
    XrValue result;
    switch (field->native_type) {
        case XR_NATIVE_I64:
            result = XR_FROM_INT(*(int64_t *) fp);
            break;
        case XR_NATIVE_F64:
            result = XR_FROM_FLOAT(*(double *) fp);
            break;
        case XR_NATIVE_BOOL:
            result.descriptor = 0;
            result.i = *(uint8_t *) fp ? 1 : 0;
            result.tag = XR_TAG_BOOL;
            break;
        case XR_NATIVE_I32:
            result = XR_FROM_INT((int64_t) * (int32_t *) fp);
            break;
        case XR_NATIVE_U32:
            result = XR_FROM_INT((int64_t) * (uint32_t *) fp);
            break;
        case XR_NATIVE_I16:
            result = XR_FROM_INT((int64_t) * (int16_t *) fp);
            break;
        case XR_NATIVE_U16:
            result = XR_FROM_INT((int64_t) * (uint16_t *) fp);
            break;
        case XR_NATIVE_I8:
            result = XR_FROM_INT((int64_t) * (int8_t *) fp);
            break;
        case XR_NATIVE_U8:
            result = XR_FROM_INT((int64_t) * (uint8_t *) fp);
            break;
        case XR_NATIVE_F32:
            result = XR_FROM_FLOAT((double) *(float *) fp);
            break;
        case XR_NATIVE_STRING: {
            XrString *s = *(XrString **) fp;
            result = s ? XR_FROM_STR(s) : xr_null();
            break;
        }
        case XR_NATIVE_STRUCT:
            result = xr_struct_ref(fp, field->sub_layout_id);
            break;
        case XR_NATIVE_ARRAY:
            result = xr_array_ref(fp, field->elem_native_type, field->elem_count);
            break;
        default:
            result = xr_null();
            break;
    }
    return XR_JIT_VAL(result);
}

// OP_STRUCT_SET: unbox XrValue and write native field to stack-allocated struct.
// call_args[0] = struct_ptr (raw), call_args[1] = value raw (i64 bits)
// extra_arg = field_idx | (xr_tag << 8)
XrJitResult xr_jit_struct_set(XrCoroutine *coro, int64_t extra_arg) {
    int field_idx = (int) (extra_arg & 0xFF);
    int val_tag = (int) ((extra_arg >> 8) & 0xFF);
    uint8_t *struct_ptr = (uint8_t *) (uintptr_t) coro->jit_ctx->call_args[0];
    int64_t raw_val = coro->jit_ctx->call_args[1];
    XrClass *cls = *(XrClass **) struct_ptr;
    XrStructLayout *layout = cls->struct_layout;
    XrStructFieldLayout *field = &layout->fields[field_idx];
    uint8_t *fp = struct_ptr + 8 + field->offset;

    switch (field->native_type) {
        case XR_NATIVE_I64:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I8:
        case XR_NATIVE_U8: {
            int64_t ival = raw_val;
            switch (field->native_type) {
                case XR_NATIVE_I64:
                    *(int64_t *) fp = ival;
                    break;
                case XR_NATIVE_I32:
                    *(int32_t *) fp = (int32_t) ival;
                    break;
                case XR_NATIVE_U32:
                    *(uint32_t *) fp = (uint32_t) ival;
                    break;
                case XR_NATIVE_I16:
                    *(int16_t *) fp = (int16_t) ival;
                    break;
                case XR_NATIVE_U16:
                    *(uint16_t *) fp = (uint16_t) ival;
                    break;
                case XR_NATIVE_I8:
                    *(int8_t *) fp = (int8_t) ival;
                    break;
                case XR_NATIVE_U8:
                    *(uint8_t *) fp = (uint8_t) ival;
                    break;
                default:
                    break;
            }
            break;
        }
        case XR_NATIVE_F64:
        case XR_NATIVE_F32: {
            double dval;
            if (val_tag == XR_TAG_F64) {
                memcpy(&dval, &raw_val, sizeof(double));
            } else {
                dval = (double) raw_val;  // int → float conversion
            }
            if (field->native_type == XR_NATIVE_F64)
                *(double *) fp = dval;
            else
                *(float *) fp = (float) dval;
            break;
        }
        case XR_NATIVE_BOOL:
            *(uint8_t *) fp = raw_val ? 1 : 0;
            break;
        case XR_NATIVE_STRING: {
            *(XrString **) fp = (XrString *) (uintptr_t) raw_val;
            break;
        }
        case XR_NATIVE_STRUCT: {
            uint8_t *src_ptr = (uint8_t *) (uintptr_t) raw_val;
            memcpy(fp, src_ptr, field->size);
            break;
        }
        default:
            break;
    }
    return XR_JIT_OK();
}

// OP_STRUCT_COPY: memcpy struct to new struct_area slot.
// call_args[0] = src struct_ptr (raw), extra_arg = dst_slot_offset (C operand)
// Returns raw dst struct_ptr.
XrJitResult xr_jit_struct_copy(XrCoroutine *coro, int64_t extra_arg) {
    int slot_offset = (int) extra_arg;
    uint8_t *src_ptr = (uint8_t *) (uintptr_t) coro->jit_ctx->call_args[0];
    XrClass *cls = *(XrClass **) src_ptr;
    XrStructLayout *layout = cls->struct_layout;
    int frame_idx = coro->vm_ctx.frame_count - 1;
    uint8_t *dst_ptr = coro->vm_ctx.struct_areas[frame_idx] + slot_offset * 16;
    memcpy(dst_ptr, src_ptr, 8 + layout->total_size);
    return XR_JIT_PTR(dst_ptr);
}

/* ========== Copy/Misc Operations ========== */

// Called from JIT via CALL_C for OP_COPY.
// jit_call_args[0] = value raw
// extra_arg = xr_tag
// Returns raw i64 of deep-copied XrValue.
XrJitResult xr_jit_deep_copy(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t val_tag = (uint8_t) (extra_arg & 0xFF);

    // Fast path: flat_copyable struct (skip CopyContext + type dispatch)
    if (val_tag == XR_TAG_PTR || val_tag == XR_RTAG_UNKNOWN) {
        void *raw = (void *) (uintptr_t) coro->jit_ctx->call_args[0];
        if (raw) {
            XrGCHeader *hdr = (XrGCHeader *) raw;
            if (hdr->type == XR_TINSTANCE) {
                XrInstance *inst = (XrInstance *) raw;
                XrClass *cls = inst->klass;
                if ((cls->flags & (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) ==
                    (XR_CLASS_VALUE_TYPE | XR_CLASS_FLAT_COPYABLE)) {
                    uint32_t fc = xr_class_instance_field_count(cls);
                    size_t sz = sizeof(XrInstance) + sizeof(XrValue) * fc;
                    XrGC *gc = xr_isolate_get_gc(coro->isolate);
                    XrInstance *dst = (XrInstance *) xr_gc_alloc(gc, sz, XR_TINSTANCE);
                    if (dst) {
                        dst->klass = cls;
                        dst->gc.extra = (dst->gc.extra & 0x01) | (inst->gc.extra & ~0x01);
                        if (fc > 0)
                            memcpy(dst->fields, inst->fields, sizeof(XrValue) * fc);
                        return XR_JIT_PTR(dst);
                    }
                }
            }
        }
    }

    // Generic path
    XrValue val = jit_value_from_tag(coro->jit_ctx->call_args[0], val_tag);
    XrValue result = xr_deep_copy(coro->isolate, val, NULL);
    return XR_JIT_VAL(result);
}

/* ========== Slice Operations ========== */

// Called from JIT via CALL_C for OP_SLICE.
// jit_call_args[0] = source ptr, jit_call_args[1] = start, jit_call_args[2] = end
// Returns raw i64 of result XrValue.
XrJitResult xr_jit_slice(XrCoroutine *coro, int64_t unused) {
    (void) unused;
    void *src = (void *) coro->jit_ctx->call_args[0];
    int32_t start = (int32_t) coro->jit_ctx->call_args[1];
    int32_t end = (int32_t) coro->jit_ctx->call_args[2];
    if (!src)
        return XR_JIT_NULL();
    XrArray *arr = (XrArray *) src;
    XrArray *slice = xr_array_slice(coro, arr, start, end);
    return slice ? XR_JIT_PTR(slice) : XR_JIT_NULL();
}

/* ========== Assert Operations ========== */

// Called from JIT via CALL_C for OP_ASSERT.
// jit_call_args[0] = condition raw (truthy/falsy)
// extra_arg = (negate << 16) | (loc_const_idx & 0xFFFF)
// Returns 0 on success, sets jit_exception on failure.
XrJitResult xr_jit_assert(XrCoroutine *coro, int64_t extra_arg) {
    int negate = (int) ((extra_arg >> 16) & 0x1);
    int64_t cond_raw = coro->jit_ctx->call_args[0];
    // Simple truthy check: non-zero = truthy
    bool truthy = (cond_raw != 0);
    bool failed = negate ? truthy : !truthy;
    if (failed) {
        // Set a generic assertion exception
        const char *fn_name = negate ? "assert_false" : "assert";
        fprintf(stderr, "\nASSERTION FAILED: %s()\n\n", fn_name);
        return (XrJitResult){XM_DEOPT_MARKER, 0};
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_ASSERT_EQ.
// jit_call_args[0] = actual raw, jit_call_args[1] = expected raw
// extra_arg = (a_bc_slot << 24) | (b_bc_slot << 16) | (actual_st << 8) | expected_st
XrJitResult xr_jit_assert_eq(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t a_bc = (uint8_t) ((extra_arg >> 24) & 0xFF);
    uint8_t b_bc = (uint8_t) ((extra_arg >> 16) & 0xFF);
    uint8_t a_tag = coro->jit_ctx->call_arg_tags[0];
    uint8_t b_tag = coro->jit_ctx->call_arg_tags[1];
    XrValue actual = jit_value_from_tag(coro->jit_ctx->call_args[0], a_tag);
    XrValue expect = jit_value_from_tag(coro->jit_ctx->call_args[1], b_tag);
    if (!xr_value_deep_eq(actual, expect)) {
        fprintf(stderr, "\nASSERTION FAILED (JIT): assert_eq() values are not equal\n");
        fprintf(stderr, "  extra_arg=0x%llx a_bc=%d b_bc=%d a_tag=%d b_tag=%d\n",
                (unsigned long long) extra_arg, a_bc, b_bc, a_tag, b_tag);
        fprintf(stderr, "  raw_a=0x%llx raw_b=0x%llx\n",
                (unsigned long long) coro->jit_ctx->call_args[0],
                (unsigned long long) coro->jit_ctx->call_args[1]);
        fprintf(stderr, "  actual: tag=%d i=%lld  expect: tag=%d i=%lld\n", actual.tag,
                (long long) actual.i, expect.tag, (long long) expect.i);
        if (a_bc != 0xFF)
            fprintf(stderr, "  slot_runtime_tags[%d]=%d\n", a_bc,
                    coro->jit_ctx->slot_runtime_tags[a_bc]);
        return (XrJitResult){XM_DEOPT_MARKER, 0};
    }
    return XR_JIT_OK();
}

// Called from JIT via CALL_C for OP_ASSERT_NE.
// jit_call_args[0] = actual raw, jit_call_args[1] = expected raw
// extra_arg = (a_bc_slot << 24) | (b_bc_slot << 16) | (actual_st << 8) | expected_st
XrJitResult xr_jit_assert_ne(XrCoroutine *coro, int64_t extra_arg) {
    uint8_t a_bc = (uint8_t) ((extra_arg >> 24) & 0xFF);
    uint8_t b_bc = (uint8_t) ((extra_arg >> 16) & 0xFF);
    uint8_t a_tag = coro->jit_ctx->call_arg_tags[0];
    uint8_t b_tag = coro->jit_ctx->call_arg_tags[1];
    XrValue actual = jit_value_from_tag(coro->jit_ctx->call_args[0], a_tag);
    XrValue expect = jit_value_from_tag(coro->jit_ctx->call_args[1], b_tag);
    if (xr_value_deep_eq(actual, expect)) {
        fprintf(stderr, "\nASSERTION FAILED: assert_ne()\n\n");
        return (XrJitResult){XM_DEOPT_MARKER, 0};
    }
    return XR_JIT_OK();
}

