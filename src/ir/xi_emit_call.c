/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit_call.c - Bytecode emission for function/method/builtin calls,
 *                  multi-return, string concat, print
 */

#include "xi_emit_internal.h"

/* Function call: args[0]=callee, args[1..n]=params
 * aux_int bits 0-7: flags (1=self_call)
 * aux_int bits 8-15: nresults (0 means 1) */
XR_FUNC void xi_emit_call(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t nargs = (uint8_t)(v->nargs - 1);
    bool self_call = ((v->aux_int & 0xFF) == 1);
    int nresults = (int)((v->aux_int >> 8) & 0xFF);
    if (nresults == 0) nresults = 1;

    /* Account for arg and result registers in maxstacksize */
    {
        int span = nargs + 1;
        if (nresults > span) span = nresults;
        uint8_t call_top = (uint8_t)(dst + span);
        if (call_top > ctx->max_reg) ctx->max_reg = call_top;
    }

    /* Reserve result registers so the allocator won't reuse them */
    if (nresults > 1 && ctx->next_reg < (uint8_t)(dst + nresults)) {
        ctx->next_reg = (uint8_t)(dst + nresults);
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
    }

    if (self_call) {
        /* Recursive self-call: OP_CALLSELF uses frame->closure.
         * nresults == 0 signals tail call (reuse current frame). */
        int self_nresults = (v->flags & XI_FLAG_TAIL) ? 0 : nresults;
        for (uint16_t a = 1; a < v->nargs; a++) {
            uint8_t arg_reg = reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK) return;
            uint8_t target = (uint8_t)(dst + a);
            if (arg_reg != target) {
                emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }
        }
        emit_inst(ctx, CREATE_ABC(OP_CALLSELF, dst, nargs,
                                  (uint8_t)self_nresults));
    } else {
        uint8_t callee = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK) return;
        if (callee != dst) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));
        }
        for (uint16_t a = 1; a < v->nargs; a++) {
            uint8_t arg_reg = reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK) return;
            uint8_t target = (uint8_t)(dst + a);
            if (arg_reg != target) {
                emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }
        }
        OpCode call_op = (v->flags & XI_FLAG_TAIL) ? OP_TAILCALL : OP_CALL;
        emit_inst(ctx, CREATE_ABC(call_op, dst, nargs,
                                  (uint8_t)nresults));
    }
}

/* Extract i-th result from a multi-return call */
XR_FUNC void xi_emit_extract(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t base = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    int idx = (int)v->aux_int;
    uint8_t src = (uint8_t)(base + idx);
    if (dst != src)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
}

/* Multi-value return: place args in consecutive registers */
XR_FUNC void xi_emit_multi_ret(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    uint8_t top = (uint8_t)(dst + v->nargs);
    if (top > ctx->max_reg) ctx->max_reg = top;
    for (uint16_t a = 0; a < v->nargs; a++) {
        uint8_t arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK) return;
        uint8_t target = (uint8_t)(dst + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }
}

/* Method call: args[0]=receiver, args[1..n]=params, aux=method name
 *
 * OP_INVOKE calling convention:
 *   R[A]   = return value position
 *   R[A+1] = receiver (this)
 *   R[A+2..A+1+C] = user arguments
 *   B = method symbol, C = user arg count (excluding this) */
XR_FUNC void xi_emit_call_method(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t recv = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    uint8_t nargs = (uint8_t)(v->nargs - 1);

    /* Account for: dst, dst+1 (recv), dst+2..dst+1+nargs */
    {
        uint8_t call_top = (uint8_t)(dst + nargs + 2);
        if (call_top > ctx->max_reg) ctx->max_reg = call_top;
    }

    /* R[dst+1] = receiver */
    if (recv != (uint8_t)(dst + 1))
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst + 1, recv, 0));

    /* R[dst+2...] = user arguments */
    for (uint16_t a = 1; a < v->nargs; a++) {
        uint8_t arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK) return;
        uint8_t target = (uint8_t)(dst + 1 + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }

    const char *method_name = (const char *)v->aux;
    bool is_super = (v->aux_int & 1) != 0;
    if (is_super) {
        /* OP_SUPERINVOKE B = constant pool index (string) */
        int ci = add_const_string(ctx, method_name);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_SUPERINVOKE, dst, (uint8_t)ci, nargs));
    } else {
        int sym = add_symbol(ctx, method_name);
        if (ctx->status != XI_EMIT_OK) return;
        OpCode invoke_op = (v->flags & XI_FLAG_TAIL) ? OP_INVOKE_TAIL : OP_INVOKE;
        emit_inst(ctx, CREATE_ABC(invoke_op, dst, (uint8_t)sym, nargs));
        /* Record IC-relevant instruction offset for JIT */
        if (v->id < ctx->reg_map_size)
            ctx->value_pc[v->id] = current_pc(ctx) - 1;
    }
}

/* Builtin call: aux_int=builtin_id or aux=name string */
XR_FUNC void xi_emit_call_builtin(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    /* Name-based dispatch (aux is a string identifier) */
    const char *bname = (const char *)v->aux;
    if (bname && strcmp(bname, "dump") == 0) {
        if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        uint8_t src = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK) return;
        uint8_t indent = 0;
        if (v->nargs >= 2) {
            indent = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK) return;
        }
        emit_inst(ctx, CREATE_ABC(OP_DUMP, src, indent, 0));
        return;
    }
    if (bname && strcmp(bname, "copy") == 0) {
        if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        uint8_t src = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_COPY, dst, src, 0));
        return;
    }
    if (bname && strcmp(bname, "chr") == 0) {
        if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
        uint8_t src = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_CHR, dst, src, 0));
        return;
    }
    if (bname && strcmp(bname, "StringBuilder") == 0) {
        /* OP_NEWSTRINGBUILDER: A=dst, B=storage_mode (0=normal) */
        emit_inst(ctx, CREATE_ABC(OP_NEWSTRINGBUILDER, dst, 0, 0));
        return;
    }
    if (bname && strcmp(bname, "Bytes") == 0) {
        uint8_t nargs = (uint8_t)v->nargs;
        if (ctx->next_reg + 1 + nargs >= MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS); return;
        }
        uint8_t base = ctx->next_reg;
        ctx->next_reg += 1 + nargs;
        if (ctx->next_reg > ctx->max_reg)
            ctx->max_reg = ctx->next_reg;
        for (uint16_t a = 0; a < nargs; a++) {
            uint8_t arg_r = reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_MOVE, (uint8_t)(base + 1 + a), arg_r, 0));
        }
        emit_inst(ctx, CREATE_ABC(OP_BYTES_NEW, base, nargs, 0));
        if (dst != base)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
        return;
    }
    /* Hard fail for unrecognized name-based builtins */
    if (bname) {
        fprintf(stderr, "[xi_emit] unknown builtin name: '%s'\n", bname);
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* Numeric builtin_id dispatch */
    int builtin_id = (int)v->aux_int;
    if (builtin_id == 0) {
        /* cancelled() */
        emit_inst(ctx, CREATE_ABC(OP_CANCELLED, dst, 0, 0));
    } else if (builtin_id > 0 && builtin_id < 256 && v->nargs > 0) {
        /* Generic: INVOKE_BUILTIN A=base, B=builtin_idx, C=nargs */
        uint8_t base = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_INVOKE_BUILTIN,
                                  base, (uint8_t)builtin_id,
                                  (uint8_t)v->nargs));
        if (dst != base)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
    } else {
        /* Hard fail: unrecognized numeric builtin ID */
        fprintf(stderr, "[xi_emit] unknown numeric builtin id: %d\n",
                builtin_id);
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
    }
}

/* String concatenation: STRBUF_NEW + STRBUF_APPEND*n + STRBUF_FINISH.
 *
 * STRBUF_NEW writes a StringBuilder into dst, destroying whatever was
 * there.  When the result is coalesced to the same register as one of
 * the operands (e.g. `result = result + "a"`), we must read that
 * operand into a temp register BEFORE STRBUF_NEW clobbers it. */
XR_FUNC void xi_emit_str_concat(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    XR_DCHECK(v->nargs <= 64, "xi_emit_str_concat: too many parts");
    uint16_t n = v->nargs > 64 ? 64 : v->nargs;

    /* Pre-resolve all arg registers before STRBUF_NEW */
    uint8_t parts[64];
    for (uint16_t a = 0; a < n; a++) {
        parts[a] = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK) return;
    }

    /* Save args that alias dst to fresh temp registers */
    for (uint16_t a = 0; a < n; a++) {
        if (parts[a] == dst) {
            if (ctx->next_reg >= MAX_REGS - 1) {
                emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
                return;
            }
            uint8_t tmp = ctx->next_reg++;
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
XR_FUNC void xi_emit_print(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    int flags = (int)v->aux_int;
    emit_inst(ctx, CREATE_ABC(OP_PRINT, src, (uint8_t)(flags & 1),
                              (uint8_t)((flags >> 1) & 0xFF)));
}
