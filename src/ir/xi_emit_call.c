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
    while (callee && callee->op == XI_COPY && callee->nargs >= 1)
        callee = callee->args[0];
    if (!callee)
        return false;
    if (callee->op == XI_CLOSURE_NEW)
        return true;
    if (callee->op == XI_GET_SHARED)
        return emit_shared_slot_is_function(ctx, callee->aux_int);
    return false;
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

    /* Account for arg and result registers in maxstacksize */
    {
        uint32_t span = (uint32_t) nargs + 1;
        uint32_t result_count = (uint32_t) nresults;
        if (result_count > span)
            span = result_count;
        uint32_t call_top = (uint32_t) dst + span;
        if (call_top > MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return;
        }
        if (call_top > ctx->max_reg)
            ctx->max_reg = call_top;
    }

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
        /* Recursive self-call: OP_CALLSELF uses frame->closure.
         * nresults == 0 signals tail call (reuse current frame). */
        int self_nresults = (v->flags & XI_FLAG_TAIL) ? 0 : nresults;
        for (uint16_t a = 1; a < v->nargs; a++) {
            XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK)
                return;
            XiEmitReg target = (XiEmitReg) (dst + a);
            if (arg_reg != target) {
                emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }
        }
        emit_inst(ctx, CREATE_ABC(OP_CALLSELF, dst, nargs, (uint8_t) self_nresults));
    } else {
        XiEmitReg callee = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        if (callee != dst) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));
        }
        for (uint16_t a = 1; a < v->nargs; a++) {
            XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK)
                return;
            XiEmitReg target = (XiEmitReg) (dst + a);
            if (arg_reg != target) {
                emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }
        }
        OpCode call_op =
            (v->flags & XI_FLAG_TAIL)
                ? OP_TAILCALL
                : (emit_callee_is_plain_closure(ctx, v->args[0]) ? OP_CALL_STATIC : OP_CALL);
        emit_inst(ctx, CREATE_ABC(call_op, dst, nargs, (uint8_t) nresults));
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

    {
        uint32_t span = (uint32_t) nargs + 1;
        uint32_t call_top = (uint32_t) dst + span;
        if (call_top > MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return;
        }
        if (call_top > ctx->max_reg)
            ctx->max_reg = call_top;
    }

    XiEmitReg callee = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (callee != dst) {
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));
    }
    for (uint16_t a = 1; a < v->nargs; a++) {
        XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (dst + a);
        if (arg_reg != target) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
        }
    }
    emit_inst(ctx, CREATE_ABC(OP_TAILCALL, dst, nargs, 1));
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
    XiEmitReg recv = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t nargs = (uint16_t) (v->nargs - 1);

    /* Account for: dst, dst+1 (recv), dst+2..dst+1+nargs */
    {
        uint32_t call_top = (uint32_t) dst + nargs + 2;
        if (call_top > MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return;
        }
        if (call_top > ctx->max_reg)
            ctx->max_reg = call_top;
    }

    /* R[dst+1] = receiver */
    if (recv != (XiEmitReg) (dst + 1))
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (dst + 1), recv, 0));

    /* R[dst+2...] = user arguments */
    for (uint16_t a = 1; a < v->nargs; a++) {
        XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (dst + 1 + a);
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
        emit_inst(ctx, CREATE_ABC(OP_SUPERINVOKE, dst, ci_arg, nargs));
    } else {
        int sym = add_symbol(ctx, method_name);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t sym_arg = 0;
        if (!xi_emit_symbol_index_to_arg(ctx, sym, &sym_arg))
            return;
        OpCode invoke_op = (v->flags & XI_FLAG_TAIL) ? OP_INVOKE_TAIL : OP_INVOKE;
        emit_inst(ctx, CREATE_ABC(invoke_op, dst, sym_arg, nargs));
        /* Record IC-relevant instruction offset for JIT */
        if (v->id < ctx->reg_map_size)
            ctx->value_pc[v->id] = current_pc(ctx) - 1;
    }
}

XR_FUNC void xi_emit_call_method_direct(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1 || v->aux_int < 0 || (uint64_t) v->aux_int > MAXARG_B || v->nargs - 1 > 127) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg recv = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t nargs = (uint16_t) (v->nargs - 1);

    {
        uint32_t call_top = (uint32_t) dst + nargs + 2;
        if (call_top > MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return;
        }
        if (call_top > ctx->max_reg)
            ctx->max_reg = call_top;
    }

    if (recv != (XiEmitReg) (dst + 1))
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (dst + 1), recv, 0));

    for (uint16_t a = 1; a < v->nargs; a++) {
        XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (dst + 1 + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }

    XiEmitReg c = nargs;
    if (v->flags & XI_FLAG_TAIL)
        c |= 0x80;
    uint16_t method_arg = 0;
    if (!xi_emit_index_to_arg(ctx, v->aux_int, XI_EMIT_ERR_INTERNAL, &method_arg))
        return;
    emit_inst(ctx, CREATE_ABC(OP_INVOKE_DIRECT, dst, method_arg, c));
}

static void emit_builtin_bytes_load_op(EmitCtx *ctx, XiValue *v, XiEmitReg dst, OpCode op) {
    if (v->nargs != 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg bytes = reg_of(ctx, v->args[0]);
    XiEmitReg offset = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(op, dst, bytes, offset));
}

static void emit_builtin_bytes_window_op(EmitCtx *ctx, XiValue *v, XiEmitReg dst, OpCode op,
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
    if (dst != base)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
}

static void emit_builtin_bytes_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    uint16_t nargs = (uint16_t) v->nargs;
    if (ctx->next_reg + 1 + nargs > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg += 1 + nargs;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    for (uint16_t a = 0; a < nargs; a++) {
        XiEmitReg arg_r = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (base + 1 + a), arg_r, 0));
    }
    emit_inst(ctx, CREATE_ABC(OP_BYTES_NEW, base, nargs, 0));
    if (dst != base)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
}

XR_FUNC void xi_emit_bytes_load_u32_le(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_bytes_load_op(ctx, v, dst, OP_BYTES_LOAD_U32_LE);
}

XR_FUNC void xi_emit_bytes_load_u64_le(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_bytes_load_op(ctx, v, dst, OP_BYTES_LOAD_U64_LE);
}

XR_FUNC void xi_emit_bytes_copy_within(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_bytes_window_op(ctx, v, dst, OP_BYTES_COPY_WITHIN, 4);
}

XR_FUNC void xi_emit_bytes_copy_from(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_bytes_window_op(ctx, v, dst, OP_BYTES_COPY_FROM, 5);
}

XR_FUNC void xi_emit_bytes_repeat_from(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_builtin_bytes_window_op(ctx, v, dst, OP_BYTES_REPEAT_FROM, 4);
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

/* Builtin call: aux_int=builtin_id or aux=name string */
XR_FUNC void xi_emit_call_builtin(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    /* Name-based dispatch (aux is a string identifier) */
    const char *bname = (const char *) v->aux;
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
        if (v->nargs < 1) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        XiEmitReg src = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_COPY, dst, src, 0));
        return;
    }
    if (bname && strcmp(bname, "chr") == 0) {
        if (v->nargs < 1) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        XiEmitReg src = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_CHR, dst, src, 0));
        return;
    }
    if (bname && strcmp(bname, "StringBuilder") == 0) {
        /* OP_NEWSTRINGBUILDER: A=dst, B=storage_mode (0=normal) */
        emit_inst(ctx, CREATE_ABC(OP_NEWSTRINGBUILDER, dst, 0, 0));
        return;
    }
    if (bname && strcmp(bname, "Bytes") == 0) {
        emit_builtin_bytes_new(ctx, v, dst);
        return;
    }
    if (bname && strcmp(bname, "array_with_capacity") == 0) {
        if (v->nargs != 1) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        XiEmitReg cap = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_ARRAY_NEW_CAP, dst, cap, (uint8_t) (v->aux_int & 0xFF)));
        return;
    }
    if (bname && strcmp(bname, "array_filled_new") == 0) {
        emit_builtin_array_filled_new(ctx, v, dst);
        return;
    }
    if (bname && strcmp(bname, "array_reserve") == 0) {
        if (v->nargs != 2) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        XiEmitReg arr = reg_of(ctx, v->args[0]);
        XiEmitReg cap = reg_of(ctx, v->args[1]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_ARRAY_RESERVE, arr, cap, 0));
        if (dst != arr)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, arr, 0));
        return;
    }
    if (bname && strcmp(bname, "array_resize") == 0) {
        if (v->nargs != 3) {
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
            return;
        }
        XiEmitReg arr = reg_of(ctx, v->args[0]);
        XiEmitReg len = reg_of(ctx, v->args[1]);
        XiEmitReg fill = reg_of(ctx, v->args[2]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_ARRAY_RESIZE, arr, len, fill));
        if (dst != arr)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, arr, 0));
        return;
    }
    if (bname && strcmp(bname, "bytes_load_u32_le") == 0) {
        emit_builtin_bytes_load_op(ctx, v, dst, OP_BYTES_LOAD_U32_LE);
        return;
    }
    if (bname && strcmp(bname, "bytes_load_u64_le") == 0) {
        emit_builtin_bytes_load_op(ctx, v, dst, OP_BYTES_LOAD_U64_LE);
        return;
    }
    if (bname && strcmp(bname, "bytes_copy_within") == 0) {
        emit_builtin_bytes_window_op(ctx, v, dst, OP_BYTES_COPY_WITHIN, 4);
        return;
    }
    if (bname && strcmp(bname, "bytes_copy_from") == 0) {
        emit_builtin_bytes_window_op(ctx, v, dst, OP_BYTES_COPY_FROM, 5);
        return;
    }
    if (bname && strcmp(bname, "bytes_repeat_from") == 0) {
        emit_builtin_bytes_window_op(ctx, v, dst, OP_BYTES_REPEAT_FROM, 4);
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
    XR_DCHECK(v->nargs <= 64, "xi_emit_str_concat: too many parts");
    uint16_t n = v->nargs > 64 ? 64 : v->nargs;

    /* Pre-resolve all arg registers before STRBUF_NEW */
    XiEmitReg parts[64];
    for (uint16_t a = 0; a < n; a++) {
        parts[a] = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
    }

    /* Save args that alias dst to fresh temp registers */
    for (uint16_t a = 0; a < n; a++) {
        if (parts[a] == dst) {
            if (ctx->next_reg >= MAX_REGS) {
                emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
                return;
            }
            XiEmitReg tmp = (XiEmitReg) ctx->next_reg++;
            if (ctx->next_reg > ctx->max_reg)
                ctx->max_reg = ctx->next_reg;
            emit_inst(ctx, CREATE_ABC(OP_MOVE, tmp, dst, 0));
            parts[a] = tmp;
        }
    }

    emit_inst(ctx, CREATE_ABC(OP_STRBUF_NEW, dst, 0, 0));
    for (uint16_t a = 0; a < n; a++) {
        emit_inst(ctx, CREATE_ABC(OP_STRBUF_APPEND, dst, parts[a], 0));
    }
    emit_inst(ctx, CREATE_ABC(OP_STRBUF_FINISH, dst, 0, 0));
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
