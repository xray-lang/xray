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
        /* Recursive self-call: OP_CALLSELF uses frame->closure */
        for (uint16_t a = 1; a < v->nargs; a++) {
            uint8_t arg_reg = reg_of(ctx, v->args[a]);
            if (ctx->status != XI_EMIT_OK) return;
            uint8_t target = (uint8_t)(dst + a);
            if (arg_reg != target) {
                emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
            }
        }
        emit_inst(ctx, CREATE_ABC(OP_CALLSELF, dst, nargs,
                                  (uint8_t)nresults));
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
        emit_inst(ctx, CREATE_ABC(OP_CALL, dst, nargs,
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
    bool is_super = (v->aux_int == 1);
    if (is_super) {
        /* OP_SUPERINVOKE B = constant pool index (string) */
        int ci = add_const_string(ctx, method_name);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_SUPERINVOKE, dst, (uint8_t)ci, nargs));
    } else {
        int sym = add_symbol(ctx, method_name);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_INVOKE, dst, (uint8_t)sym, nargs));
        /* Record IC-relevant instruction offset for JIT */
        if (v->id < ctx->reg_map_size)
            ctx->value_pc[v->id] = current_pc(ctx) - 1;
    }
}

/* Builtin call: aux_int=builtin_id or aux=name string */
XR_FUNC void xi_emit_call_builtin(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    /* Name-based dispatch (aux is a string identifier) */
    const char *bname = (const char *)v->aux;
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
    /* Numeric builtin_id dispatch */
    int builtin_id = (int)v->aux_int;
    if (builtin_id == 0 && !bname) {
        /* cancelled() */
        emit_inst(ctx, CREATE_ABC(OP_CANCELLED, dst, 0, 0));
    } else if (!bname) {
        /* Generic: INVOKE_BUILTIN A=base, B=builtin_idx, C=nargs */
        if (v->nargs > 0) {
            uint8_t base = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK) return;
            emit_inst(ctx, CREATE_ABC(OP_INVOKE_BUILTIN,
                                      base, (uint8_t)builtin_id,
                                      (uint8_t)v->nargs));
            if (dst != base)
                emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, base, 0));
        }
    }
}

/* String concatenation: STRBUF_NEW + STRBUF_APPEND*n + STRBUF_FINISH */
XR_FUNC void xi_emit_str_concat(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    emit_inst(ctx, CREATE_ABC(OP_STRBUF_NEW, dst, 0, 0));
    for (uint16_t a = 0; a < v->nargs; a++) {
        uint8_t part = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_STRBUF_APPEND, dst, part, 0));
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
