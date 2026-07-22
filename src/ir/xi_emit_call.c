/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit_call.c - Bytecode emission for function/method/builtin calls,
 *                  string concat, print
 */

#include "xi_emit_internal.h"
#include "../frontend/analyzer/xa_intrinsic_registry.h"
#include "../runtime/mem/xobj_header.h"

static bool emit_shared_slot_is_function(EmitCtx *ctx, int64_t slot) {
    if (!ctx || !ctx->func || slot < 0)
        return false;

    for (XiFunc *f = ctx->func; f; f = f->parent_func) {
        if (!f->shared_slot_funcs || slot >= (int64_t) f->shared_slot_func_count)
            continue;
        if (f->shared_slot_funcs[slot])
            return true;
    }
    return false;
}

static bool emit_callee_is_plain_closure(EmitCtx *ctx, XiValue *callee) {
    while (callee && xi_copy_is_identity_alias(callee) && callee->nargs >= 1)
        callee = callee->args[0];
    if (!callee)
        return false;
    if (callee->op == XI_CLOSURE_NEW)
        return true;
    if (callee->op == XI_GET_SHARED)
        return emit_shared_slot_is_function(ctx, callee->aux_int);
    return false;
}

static bool emit_call_is_channel_send_boundary(const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 2 || !v->args[0] || !v->args[0]->type ||
        v->args[0]->type->kind != XR_KIND_CHANNEL)
        return false;
    const char *method = (const char *) v->aux;
    return method && (strcmp(method, "send") == 0 || strcmp(method, "trySend") == 0 ||
                      strcmp(method, "sendTimeout") == 0);
}

static void emit_channel_method_transfer_annotation(EmitCtx *ctx, const XiValue *v) {
    if (!emit_call_is_channel_send_boundary(v))
        return;
    uint8_t mode = xi_chan_send_transfer_mode(v);
    if (mode == XR_TRANSFER_SHARE)
        return;
    emit_inst(ctx, CREATE_ABx(OP_NOP, 6, (uint32_t) mode));
}

static bool emit_call_scratch_window(EmitCtx *ctx, uint32_t width, XiEmitReg *base_out) {
    if (!base_out || width == 0) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return false;
    }
    if (ctx->max_reg + width > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return false;
    }
    *base_out = (XiEmitReg) ctx->max_reg;
    ctx->max_reg += width;
    return true;
}

static void emit_copy_call_results(EmitCtx *ctx, XiEmitReg dst, XiEmitReg base, int nresults) {
    if (dst == NO_REG || nresults <= 0)
        return;
    for (int r = 0; r < nresults; r++) {
        XiEmitReg from = (XiEmitReg) (base + (uint16_t) r);
        XiEmitReg to = (XiEmitReg) (dst + (uint16_t) r);
        if (from != to)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, to, from, 0));
    }
}

/* Function call: args[0]=callee, args[1..n]=params
 * aux_int bits 0-7: flags (1=self_call)
 * aux_int bits 8-15: nresults (0 means 1) */
XR_FUNC void xi_emit_call(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    bool self_call = ((v->aux_int & 0xFF) == 1);
    int nresults = (int) ((v->aux_int >> 8) & 0xFF);
    if (nresults == 0)
        nresults = 1;

    /* Reserve result registers so the allocator won't reuse them */
    uint32_t result_top = (uint32_t) dst + (uint32_t) nresults;
    if (result_top > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    if (nresults > 1 && ctx->next_reg < result_top) {
        ctx->next_reg = result_top;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
    }

    if (self_call) {
        for (uint16_t a = 1; a < v->nargs; a++) {
            (void) reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK)
                return;
        }
        uint32_t width = (uint32_t) nargs + 1;
        if ((uint32_t) nresults > width)
            width = (uint32_t) nresults;
        XiEmitReg base = NO_REG;
        if (!emit_call_scratch_window(ctx, width, &base))
            return;

        /* Recursive self-call: OP_CALLSELF uses frame->closure.
         * nresults == 0 signals tail call (reuse current frame). */
        int self_nresults = (v->flags & XI_FLAG_TAIL) ? 0 : nresults;
        for (uint16_t a = 1; a < v->nargs; a++) {
            XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK)
                return;
            XiEmitReg target = (XiEmitReg) (base + a);
            if (arg_reg != target) {
                emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }
        }
        emit_inst(ctx, CREATE_ABC(OP_CALLSELF, base, nargs, (uint8_t) self_nresults));
        emit_copy_call_results(ctx, dst, base, nresults);
    } else {
        for (uint16_t a = 0; a < v->nargs; a++) {
            (void) reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK)
                return;
        }
        uint32_t width = (uint32_t) nargs + 1;
        if ((uint32_t) nresults > width)
            width = (uint32_t) nresults;
        XiEmitReg base = NO_REG;
        if (!emit_call_scratch_window(ctx, width, &base))
            return;

        XiEmitReg callee = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        if (callee != base) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, base, callee, 0));
        }
        for (uint16_t a = 1; a < v->nargs; a++) {
            XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK)
                return;
            XiEmitReg target = (XiEmitReg) (base + a);
            if (arg_reg != target) {
                emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }
        }
        OpCode call_op =
            (v->flags & XI_FLAG_TAIL)
                ? OP_TAILCALL
                : (emit_callee_is_plain_closure(ctx, v->args[0]) ? OP_CALL_STATIC : OP_CALL);
        emit_inst(ctx, CREATE_ABC(call_op, base, nargs, (uint8_t) nresults));
        emit_copy_call_results(ctx, dst, base, nresults);
    }
}

/* XI_TAIL_CALL: always emits OP_TAILCALL. Same layout as XI_CALL
 * but the op absorbs the tail semantics. */
XR_FUNC void xi_emit_tail_call(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t nargs = (uint16_t) (v->nargs - 1);

    for (uint16_t a = 0; a < v->nargs; a++) {
        (void) reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
    }
    XiEmitReg base = NO_REG;
    if (!emit_call_scratch_window(ctx, (uint32_t) nargs + 1, &base))
        return;

    XiEmitReg callee = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (callee != base) {
        emit_inst(ctx, CREATE_ABC(OP_MOVE, base, callee, 0));
    }
    for (uint16_t a = 1; a < v->nargs; a++) {
        XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (base + a);
        if (arg_reg != target) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
        }
    }
    emit_inst(ctx, CREATE_ABC(OP_TAILCALL, base, nargs, 1));
    emit_copy_call_results(ctx, dst, base, 1);
}

/* Method call: args[0]=receiver, args[1..n]=params, aux=method name
 *
 * OP_INVOKE calling convention:
 *   R[A]   = return value position
 *   R[A+1] = receiver (this)
 *   R[A+2..A+1+C] = user arguments
 *   B = method symbol, C = user arg count (excluding this) */
XR_FUNC void xi_emit_call_method(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t nargs = (uint16_t) (v->nargs - 1);

    for (uint16_t a = 0; a < v->nargs; a++) {
        (void) reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
    }
    XiEmitReg base = NO_REG;
    if (!emit_call_scratch_window(ctx, (uint32_t) nargs + 2, &base))
        return;

    XiEmitReg recv = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    /* R[base+1] = receiver */
    if (recv != (XiEmitReg) (base + 1))
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (base + 1), recv, 0));

    /* R[base+2...] = user arguments */
    for (uint16_t a = 1; a < v->nargs; a++) {
        XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (base + 1 + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }

    const char *method_name = (const char *) v->aux;
    bool is_super = (v->aux_int & 1) != 0;
    if (is_super) {
        /* OP_SUPERINVOKE B = constant pool index (string) */
        int ci = add_const_string(ctx, method_name);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t ci_arg = 0;
        if (!xi_emit_const_index_to_c(ctx, ci, &ci_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_SUPERINVOKE, base, ci_arg, nargs));
        emit_copy_call_results(ctx, dst, base, 1);
    } else {
        int sym = add_symbol(ctx, method_name);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t sym_arg = 0;
        if (!xi_emit_symbol_index_to_arg(ctx, sym, &sym_arg))
            return;
        OpCode invoke_op = (v->flags & XI_FLAG_TAIL) ? OP_INVOKE_TAIL : OP_INVOKE;
        emit_channel_method_transfer_annotation(ctx, v);
        emit_inst(ctx, CREATE_ABC(invoke_op, base, sym_arg, nargs));
        emit_copy_call_results(ctx, dst, base, 1);
    }
}

static bool semantic_intrinsic_operand_is_readonly_place(const XaIntrinsicDesc *desc,
                                                         uint16_t operand_index) {
    if (!desc || desc->family != XA_INTRINSIC_FAMILY_SIMD)
        return false;
    if (operand_index == 0)
        return (desc->flags & XA_INTRINSIC_FLAG_STATIC_RECEIVER) == 0;
    if (operand_index != 1)
        return false;
    switch (desc->lowering) {
        case XA_INTRINSIC_LOWERING_VEC_LOAD:
        case XA_INTRINSIC_LOWERING_VEC_ADD:
        case XA_INTRINSIC_LOWERING_VEC_SUB:
        case XA_INTRINSIC_LOWERING_VEC_MUL:
        case XA_INTRINSIC_LOWERING_VEC_BIT_AND:
        case XA_INTRINSIC_LOWERING_VEC_BIT_OR:
        case XA_INTRINSIC_LOWERING_VEC_BIT_XOR:
        case XA_INTRINSIC_LOWERING_VEC_WIDEN_MUL:
            return true;
        case XA_INTRINSIC_LOWERING_VEC_SHUFFLE:
            return (desc->flags & XA_INTRINSIC_FLAG_UNZIP) != 0;
        default:
            return false;
    }
}

XR_FUNC void xi_emit_semantic_intrinsic_call(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (!ctx || !v || v->xa_intrinsic_id == XA_INTRINSIC_NONE || v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    const XaIntrinsicDesc *desc = xa_intrinsic_by_id((XaIntrinsicId) v->xa_intrinsic_id);
    const char *source_member = xa_intrinsic_source_member(desc);
    if (!desc || !source_member) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t nargs = (uint16_t) (v->nargs - 1);
    for (uint16_t a = 0; a < v->nargs; a++) {
        (void) reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
    }

    uint16_t readonly_place_count = 0;
    for (uint16_t a = 0; a < v->nargs; a++) {
        if (semantic_intrinsic_operand_is_readonly_place(desc, a))
            readonly_place_count++;
    }
    XiEmitReg place_storage_base = NO_REG;
    if (readonly_place_count > 0 &&
        !emit_call_scratch_window(ctx, readonly_place_count, &place_storage_base))
        return;
    XiEmitReg base = NO_REG;
    if (!emit_call_scratch_window(ctx, (uint32_t) nargs + 2, &base))
        return;
    uint16_t readonly_place_index = 0;
    for (uint16_t a = 0; a < v->nargs; a++) {
        XiEmitReg source = reg_of(ctx, v->args[a]);
        XiEmitReg target = (XiEmitReg) (base + 1 + a);
        if (semantic_intrinsic_operand_is_readonly_place(desc, a)) {
            XiEmitReg storage = (XiEmitReg) (place_storage_base + readonly_place_index++);
            if (source != storage)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, storage, source, 0));
            emit_inst(ctx, CREATE_ABC(OP_LOCAL_ADDR, target, storage, 0));
        } else if (source != target) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, source, 0));
        }
    }

    int sym = add_symbol(ctx, source_member);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t sym_arg = 0;
    if (!xi_emit_symbol_index_to_arg(ctx, sym, &sym_arg))
        return;
    emit_inst(ctx, CREATE_ABC(OP_INVOKE, base, sym_arg, nargs));
    emit_copy_call_results(ctx, dst, base, 1);
}

XR_FUNC void xi_emit_call_method_direct(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1 || v->aux_int < 0 || (uint64_t) v->aux_int > MAXARG_B || v->nargs - 1 > 127) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t nargs = (uint16_t) (v->nargs - 1);

    for (uint16_t a = 0; a < v->nargs; a++) {
        (void) reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
    }
    XiEmitReg base = NO_REG;
    if (!emit_call_scratch_window(ctx, (uint32_t) nargs + 2, &base))
        return;

    XiEmitReg recv = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (recv != (XiEmitReg) (base + 1))
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (base + 1), recv, 0));

    for (uint16_t a = 1; a < v->nargs; a++) {
        XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (base + 1 + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }

    XiEmitReg c = nargs;
    if (v->flags & XI_FLAG_TAIL)
        c |= 0x80;
    uint16_t method_arg = 0;
    if (!xi_emit_index_to_arg(ctx, v->aux_int, XI_EMIT_ERR_INTERNAL, &method_arg))
        return;
    emit_inst(ctx, CREATE_ABC(OP_INVOKE_DIRECT, base, method_arg, c));
    emit_copy_call_results(ctx, dst, base, 1);
}

static void emit_builtin_byte_slice_load_op(EmitCtx *ctx, XiValue *v, XiEmitReg dst, OpCode op) {
    if (v->nargs != 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t expected_args = 3;
    if (ctx->next_reg + 1 + expected_args > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg = (base + 1 + expected_args);
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    for (uint16_t a = 0; a < expected_args; a++) {
        XiEmitReg src = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (base + 1 + a);
        if (src != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, src, 0));
    }
    emit_inst(ctx, CREATE_ABC(op, base, 0, 0));
    if (dst != NO_REG && dst != base)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
}

static void emit_builtin_byte_slice_store_op(EmitCtx *ctx, XiValue *v, XiEmitReg dst, OpCode op) {
    if (v->nargs != 4) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t expected_args = 4;
    if (ctx->next_reg + 1 + expected_args > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg = (base + 1 + expected_args);
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    for (uint16_t a = 0; a < expected_args; a++) {
        XiEmitReg src = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (base + 1 + a);
        if (src != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, src, 0));
    }
    emit_inst(ctx, CREATE_ABC(op, base, 0, 0));
    if (dst != NO_REG && dst != base)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
}

static void emit_builtin_contiguous_window_op(EmitCtx *ctx, XiValue *v, XiEmitReg dst, OpCode op,
                                              uint16_t expected_args) {
    if (v->nargs != expected_args) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    if (ctx->next_reg + expected_args > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg = (base + expected_args);
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    for (uint16_t a = 0; a < expected_args; a++) {
        XiEmitReg src = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (base + a);
        if (src != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, src, 0));
    }
    emit_inst(ctx, CREATE_ABC(op, base, 0, 0));
    if (dst != NO_REG && dst != base)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
}

static void emit_builtin_array_copy_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ARRAY_COPY_NEW, dst, src, (uint8_t) (v->aux_int & 0xFF)));
}

XR_FUNC void xi_emit_byte_slice_load_u16(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_load_op(ctx, v, dst, OP_BYTE_SLICE_LOAD_U16);
}

XR_FUNC void xi_emit_byte_slice_load_u32(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_load_op(ctx, v, dst, OP_BYTE_SLICE_LOAD_U32);
}

XR_FUNC void xi_emit_byte_slice_load_u64(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_load_op(ctx, v, dst, OP_BYTE_SLICE_LOAD_U64);
}

XR_FUNC void xi_emit_byte_slice_load_f32(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_load_op(ctx, v, dst, OP_BYTE_SLICE_LOAD_F32);
}

XR_FUNC void xi_emit_byte_slice_load_f64(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_load_op(ctx, v, dst, OP_BYTE_SLICE_LOAD_F64);
}

XR_FUNC void xi_emit_byte_slice_store_u16(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_store_op(ctx, v, dst, OP_BYTE_SLICE_STORE_U16);
}

XR_FUNC void xi_emit_byte_slice_store_u32(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_store_op(ctx, v, dst, OP_BYTE_SLICE_STORE_U32);
}

XR_FUNC void xi_emit_byte_slice_store_u64(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_store_op(ctx, v, dst, OP_BYTE_SLICE_STORE_U64);
}

XR_FUNC void xi_emit_byte_slice_store_f32(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_store_op(ctx, v, dst, OP_BYTE_SLICE_STORE_F32);
}

XR_FUNC void xi_emit_byte_slice_store_f64(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_byte_slice_store_op(ctx, v, dst, OP_BYTE_SLICE_STORE_F64);
}

XR_FUNC void xi_emit_byte_slice_fill(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_BYTE_SLICE_FILL, 2);
}

XR_FUNC void xi_emit_byte_slice_copy(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_BYTE_SLICE_COPY, 2);
}

XR_FUNC void xi_emit_byte_slice_compare(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_BYTE_SLICE_COMPARE, 2);
}

XR_FUNC void xi_emit_byte_slice_common_prefix(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_BYTE_SLICE_COMMON_PREFIX, 2);
}

XR_FUNC void xi_emit_byte_slice_repeat(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_BYTE_SLICE_REPEAT, 4);
}

XR_FUNC void xi_emit_span_window(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    XiEmitReg start_src = reg_of(ctx, v->args[1]);
    XiEmitReg count_src = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t span_slot = 0;
    if (!xi_emit_alloc_struct_area_bytes(ctx, (uint32_t) sizeof(XrSpanView), &span_slot))
        return;
    if (ctx->next_reg + 3 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg += 3;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    emit_inst(ctx, CREATE_ABC(OP_MOVE, base, start_src, 0));
    emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (base + 1), count_src, 0));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, (XiEmitReg) (base + 2), span_slot));
    emit_inst(ctx, CREATE_ABC(OP_SPAN_WINDOW, dst, src, base));
}

XR_FUNC void xi_emit_span_as_bytes(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t span_slot = 0;
    if (!xi_emit_alloc_struct_area_bytes(ctx, (uint32_t) sizeof(XrSpanView), &span_slot))
        return;
    if (ctx->next_reg + 1 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg slot_reg = (XiEmitReg) ctx->next_reg++;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, slot_reg, span_slot));
    emit_inst(ctx, CREATE_ABC(OP_SPAN_AS_BYTES, dst, src, slot_reg));
}

XR_FUNC void xi_emit_span_copy(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_SPAN_COPY, 2);
}

XR_FUNC void xi_emit_span_fill(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_SPAN_FILL, 2);
}

XR_FUNC void xi_emit_span_compare(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_SPAN_COMPARE, 2);
}

XR_FUNC void xi_emit_span_reinterpret(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t span_slot = 0;
    if (!xi_emit_alloc_struct_area_bytes(ctx, (uint32_t) sizeof(XrSpanView), &span_slot))
        return;
    if (ctx->next_reg + 4 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg = (XiEmitReg) (base + 4);
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    uint8_t elem_type = (uint8_t) (v->aux_int & 0xff);
    uint8_t elem_size = (uint8_t) ((v->aux_int >> 8) & 0xff);
    uint8_t elem_tid = (uint8_t) ((v->aux_int >> 16) & 0xff);
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, base, span_slot));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, (XiEmitReg) (base + 1), elem_type));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, (XiEmitReg) (base + 2), elem_size));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, (XiEmitReg) (base + 3), elem_tid));
    emit_inst(ctx, CREATE_ABC(OP_SPAN_REINTERPRET, dst, src, base));
}

/* Unsafe container data pointer: R[dst] = (uintptr_t)Array/Span.data. */
XR_FUNC void xi_emit_array_data_ptr(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg arr = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ARRAY_DATA_PTR, dst, arr, 0));
}

/* Direct byte literals enter the bytecode constant pool with their exact
 * length (including embedded NUL).  Loading the constant recreates/permanently
 * interns its storage after bytecode deserialization; ARRAY_DATA_PTR then
 * projects that stable storage instead of baking a compiler-process address. */
XR_FUNC void xi_emit_static_bytes_ptr(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (!ctx || !v || v->nargs != 0 || v->aux_int < 0 || (v->aux_int > 0 && !v->aux)) {
        if (ctx)
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    int kidx = add_const_string_n(ctx, (const char *) v->aux, (size_t) v->aux_int);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABx(OP_LOADK, dst, (uint32_t) kidx));
    emit_inst(ctx, CREATE_ABC(OP_ARRAY_DATA_PTR, dst, dst, 0));
}

static void emit_builtin_string_byte_slice(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg str = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t span_slot = 0;
    if (!xi_emit_alloc_struct_area_bytes(ctx, (uint32_t) sizeof(XrSpanView), &span_slot))
        return;
    if (ctx->next_reg + 1 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg slot_reg = (XiEmitReg) ctx->next_reg++;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, slot_reg, span_slot));
    emit_inst(ctx, CREATE_ABC(OP_STRING_BYTES_SPAN, dst, str, slot_reg));
}

/* FFI raw-pointer load. The VM consumes a compact contiguous argument window:
 * R[base] is the result, R[base+1] the address, R[base+2] the Endian value. */
XR_FUNC void xi_emit_ptr_load(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg addr = reg_of(ctx, v->args[0]);
    XiEmitReg endian = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (ctx->next_reg + 3 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg = (XiEmitReg) (base + 3);
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    if (addr != base + 1)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (base + 1), addr, 0));
    if (endian != base + 2)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (base + 2), endian, 0));
    emit_inst(ctx, CREATE_ABC(OP_PTR_LOAD, base, (XiEmitReg) (v->aux_int & 0xff), (XiEmitReg) 0));
    if (dst != NO_REG && dst != base)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
}

XR_FUNC void xi_emit_local_addr(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg source = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    /* A place descriptor outlives the ordinary register-allocation last use of
     * its source.  Keep the pointee in a dedicated frame slot so a later
     * argument/address value cannot recycle and overwrite that register before
     * or during the call. */
    if (ctx->next_reg + 1 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg storage = (XiEmitReg) ctx->next_reg++;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    if (source != storage)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, storage, source, 0));
    emit_inst(ctx, CREATE_ABC(OP_LOCAL_ADDR, dst, storage, 0));
}

XR_FUNC void xi_emit_place_load(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg place = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_PLACE_LOAD, dst, place, 0));
}

XR_FUNC void xi_emit_place_store(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs != 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg place = reg_of(ctx, v->args[0]);
    XiEmitReg value = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_PLACE_STORE, place, value, 0));
}

/* FFI raw-pointer store: R[base..base+2] = address, value, Endian. */
XR_FUNC void xi_emit_ptr_store(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs != 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg addr = reg_of(ctx, v->args[0]);
    XiEmitReg val = reg_of(ctx, v->args[1]);
    XiEmitReg endian = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (ctx->next_reg + 3 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg = (XiEmitReg) (base + 3);
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    if (addr != base)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, base, addr, 0));
    if (val != base + 1)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (base + 1), val, 0));
    if (endian != base + 2)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (base + 2), endian, 0));
    emit_inst(ctx, CREATE_ABC(OP_PTR_STORE, base, (XiEmitReg) (v->aux_int & 0xff), (XiEmitReg) 0));
}

/* Raw pointer memcpy: memcpy(dst, src, byte_count). */
XR_FUNC void xi_emit_ptr_copy_nonoverlap(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs != 3) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg dst_addr = reg_of(ctx, v->args[0]);
    XiEmitReg src_addr = reg_of(ctx, v->args[1]);
    XiEmitReg byte_count = reg_of(ctx, v->args[2]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_PTR_COPY_NONOVERLAP, dst_addr, src_addr, byte_count));
}

XR_FUNC void xi_emit_byte_array_copy_within(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_BYTE_ARRAY_COPY_WITHIN, 4);
}

XR_FUNC void xi_emit_byte_array_copy_from(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_BYTE_ARRAY_COPY_FROM, 5);
}

XR_FUNC void xi_emit_byte_array_append_from(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_BYTE_ARRAY_APPEND_FROM, 2);
}

XR_FUNC void xi_emit_byte_array_repeat_from(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_contiguous_window_op(ctx, v, dst, OP_BYTE_ARRAY_REPEAT_FROM, 3);
}

static void emit_builtin_array_filled_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs != 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg len = reg_of(ctx, v->args[0]);
    XiEmitReg fill = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (ctx->next_reg + 2 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg len_tmp = (XiEmitReg) ctx->next_reg++;
    XiEmitReg fill_tmp = (XiEmitReg) ctx->next_reg++;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    emit_inst(ctx, CREATE_ABC(OP_MOVE, len_tmp, len, 0));
    emit_inst(ctx, CREATE_ABC(OP_MOVE, fill_tmp, fill, 0));
    emit_inst(ctx, CREATE_ABC(OP_ARRAY_NEW_CAP, dst, len_tmp, (uint8_t) (v->aux_int & 0xFF)));
    emit_inst(ctx, CREATE_ABC(OP_ARRAY_RESIZE, dst, len_tmp, fill_tmp));
}

static void emit_builtin_unary_opcode(EmitCtx *ctx, XiValue *v, XiEmitReg dst, int opcode) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(opcode, dst, src, 0));
}

static void emit_builtin_unary_opcode_c(EmitCtx *ctx, XiValue *v, XiEmitReg dst, int opcode,
                                        uint8_t c) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(opcode, dst, src, c));
}

/* Builtin call: aux_int=builtin_id or aux=name string */
/* Array capacity/length builtins (name-based), split from
 * xi_emit_call_builtin. Returns false when `bname` is not one of them. */
static bool emit_builtin_array_shape_op(EmitCtx *ctx, XiValue *v, XiEmitReg dst,
                                        const char *bname) {
    if (strcmp(bname, "array_with_capacity") == 0) {
        if (v->nargs != 1) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return true;
        }
        XiEmitReg cap = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return true;
        emit_inst(ctx, CREATE_ABC(OP_ARRAY_NEW_CAP, dst, cap, (uint8_t) (v->aux_int & 0xFF)));
        return true;
    }
    if (strcmp(bname, "array_filled_new") == 0) {
        emit_builtin_array_filled_new(ctx, v, dst);
        return true;
    }
    if (strcmp(bname, "array_clear") == 0) {
        if (v->nargs != 1) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return true;
        }
        XiEmitReg arr = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return true;
        emit_inst(ctx, CREATE_ABC(OP_ARRAY_CLEAR, arr, 0, 0));
        if (dst != arr)
            emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
        return true;
    }
    if (strcmp(bname, "array_reserve") == 0) {
        if (v->nargs != 2) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return true;
        }
        XiEmitReg arr = reg_of(ctx, v->args[0]);
        XiEmitReg cap = reg_of(ctx, v->args[1]);
        if (ctx->status != XI_EMIT_OK)
            return true;
        emit_inst(ctx, CREATE_ABC(OP_ARRAY_RESERVE, arr, cap, 0));
        if (dst != arr)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, arr, 0));
        return true;
    }
    if (strcmp(bname, "array_resize") == 0) {
        if (v->nargs != 3) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return true;
        }
        XiEmitReg arr = reg_of(ctx, v->args[0]);
        XiEmitReg len = reg_of(ctx, v->args[1]);
        XiEmitReg fill = reg_of(ctx, v->args[2]);
        if (ctx->status != XI_EMIT_OK)
            return true;
        emit_inst(ctx, CREATE_ABC(OP_ARRAY_RESIZE, arr, len, fill));
        if (dst != arr)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, arr, 0));
        return true;
    }
    return false;
}

XR_FUNC void xi_emit_call_builtin(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    /* Name-based dispatch (aux is a string identifier) */
    const char *bname = (const char *) v->aux;
    if (bname && emit_builtin_array_shape_op(ctx, v, dst, bname))
        return;
    if (bname && strcmp(bname, "dump") == 0) {
        if (v->nargs < 1) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        XiEmitReg src = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint8_t indent = 0;
        if (v->nargs >= 2) {
            indent = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK)
                return;
        }
        emit_inst(ctx, CREATE_ABC(OP_DUMP, src, indent, 0));
        return;
    }
    if (bname && strcmp(bname, "copy") == 0) {
        emit_builtin_unary_opcode(ctx, v, dst, OP_COPY);
        return;
    }
    if (bname && strcmp(bname, "copy_shared") == 0) {
        emit_builtin_unary_opcode_c(ctx, v, dst, OP_COPY, XR_OBJ_STORAGE_SHARED);
        return;
    }
    if (bname && strcmp(bname, "copy_owned") == 0) {
        emit_builtin_unary_opcode_c(ctx, v, dst, OP_COPY, XR_OBJ_STORAGE_TRANSFER);
        return;
    }
    if (bname && strcmp(bname, "to_shared") == 0) {
        emit_builtin_unary_opcode(ctx, v, dst, OP_TO_SHARED);
        return;
    }
    if (bname && strcmp(bname, "chr") == 0) {
        emit_builtin_unary_opcode(ctx, v, dst, OP_CHR);
        return;
    }
    if (bname && strcmp(bname, "StringBuilder") == 0) {
        /* OP_NEWSTRINGBUILDER: A=dst, B=storage_mode (0=normal) */
        emit_inst(ctx, CREATE_ABC(OP_NEWSTRINGBUILDER, dst, (uint8_t) (v->aux_int & 0x03), 0));
        return;
    }
    if (bname && strcmp(bname, "array_copy_new") == 0) {
        emit_builtin_array_copy_new(ctx, v, dst);
        return;
    }
    if (bname && strcmp(bname, "string_byte_slice") == 0) {
        emit_builtin_string_byte_slice(ctx, v, dst);
        return;
    }
    /* Exception: no dedicated opcode; handled as a regular class via
     * the generic OP_INVOKE pipeline with a primitive constructor. */
    /* Hard fail for unrecognized name-based builtins */
    if (bname) {
        fprintf(stderr, "[xi_emit] unknown builtin name: '%s'\n", bname);
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* Numeric builtin_id dispatch */
    int builtin_id = (int) v->aux_int;
    if (builtin_id == 0) {
        /* cancelled() */
        emit_inst(ctx, CREATE_ABC(OP_CANCELLED, dst, 0, 0));
    } else if (builtin_id > 0 && builtin_id < 256 && v->nargs > 0) {
        /* Generic builtin method call: route through unified OP_INVOKE.
         * Encoding (A=base, B=proto-local symbol idx, C=nargs) uses the
         * unified OP_INVOKE path. The runtime dispatch resolves the
         * receiver class via native_type_classes[] and calls the
         * XMETHOD_PRIMITIVE method through the XrICMethod inline cache. */
        XiEmitReg base = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_INVOKE, base, (uint8_t) builtin_id, (uint8_t) v->nargs));
        if (dst != base)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
    } else {
        /* Hard fail: unrecognized numeric builtin ID */
        fprintf(stderr, "[xi_emit] unknown numeric builtin id: %d\n", builtin_id);
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
    }
}

/* String concatenation: STRBUF_NEW + STRBUF_APPEND*n + STRBUF_FINISH.
 *
 * STRBUF_NEW writes a StringBuilder into dst, destroying whatever was
 * there.  When the result is coalesced to the same register as one of
 * the operands (e.g. `result = result + "a"`), we must read that
 * operand into a temp register BEFORE STRBUF_NEW clobbers it. */
XR_FUNC void xi_emit_str_concat(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    uint16_t n = v->nargs;

    /* Pre-resolve all arg registers before STRBUF_NEW */
    XiEmitReg stack_parts[64];
    XiEmitReg *parts = stack_parts;
    if (n > 64) {
        parts = (XiEmitReg *) xr_malloc((size_t) n * sizeof(XiEmitReg));
        if (!parts) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
    }
    for (uint16_t a = 0; a < n; a++) {
        parts[a] = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK) {
            if (parts != stack_parts)
                xr_free(parts);
            return;
        }
    }

    /* Save args that alias dst to fresh temp registers */
    for (uint16_t a = 0; a < n; a++) {
        if (parts[a] == dst) {
            if (ctx->next_reg >= MAX_REGS) {
                emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
                if (parts != stack_parts)
                    xr_free(parts);
                return;
            }
            XiEmitReg tmp = (XiEmitReg) ctx->next_reg++;
            if (ctx->next_reg > ctx->max_reg)
                ctx->max_reg = ctx->next_reg;
            emit_inst(ctx, CREATE_ABC(OP_MOVE, tmp, dst, 0));
            parts[a] = tmp;
        }
    }

    /* Static uint values share the raw i64 slot representation with signed
     * integers. Convert them before dynamic StringBuilder append loses the
     * unsigned type view. */
    for (uint16_t a = 0; a < n; a++) {
        int hint = xi_emit_tostring_hint_for_type(v->args[a] ? v->args[a]->type : NULL);
        if (hint != 3)
            continue;
        if (ctx->next_reg >= MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            if (parts != stack_parts)
                xr_free(parts);
            return;
        }
        XiEmitReg tmp = (XiEmitReg) ctx->next_reg++;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
        emit_inst(ctx, CREATE_ABC(OP_TOSTRING, tmp, parts[a], hint));
        parts[a] = tmp;
    }

    emit_inst(ctx, CREATE_ABC(OP_STRBUF_NEW, dst, 0, 0));
    for (uint16_t a = 0; a < n; a++) {
        emit_inst(ctx, CREATE_ABC(OP_STRBUF_APPEND, dst, parts[a], 0));
    }
    emit_inst(ctx, CREATE_ABC(OP_STRBUF_FINISH, dst, 0, 0));
    if (parts != stack_parts)
        xr_free(parts);
}

/* Print */
XR_FUNC void xi_emit_print(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int flags = (int) v->aux_int;
    emit_inst(ctx,
              CREATE_ABC(OP_PRINT, src, (uint8_t) (flags & 1), (uint8_t) ((flags >> 1) & 0xFF)));
}
