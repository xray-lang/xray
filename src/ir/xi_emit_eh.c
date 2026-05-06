/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit_eh.c - Bytecode emission for exception handling, coroutine,
 *                assert, scope, defer operations
 */

#include "xi_emit_internal.h"
#include <stdio.h>

/* ========== Exception Handling ========== */

/* Throw */
XR_FUNC void xi_emit_throw(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_THROW, src, 0, 0));
}

/* Try: emit OP_TRY + NOP (finally placeholder), register for patching.
 * aux = catch block (NULL for try-finally without catch).
 * aux_int = finally block ID (>=0) or -1 if no finally. */
XR_FUNC void xi_emit_try(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    XiBlock *catch_blk = (XiBlock *)v->aux;  /* NULL when no catch clause */
    int try_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABx(OP_TRY, 0, 0));     /* patched later */
    emit_inst(ctx, CREATE_ABx(OP_NOP, 0, 0));      /* finally placeholder */

    /* aux_int = finally block ID (>=0) or -1 if no finally */
    uint32_t fin_bid = (v->aux_int >= 0) ? (uint32_t)v->aux_int : 0;
    uint32_t catch_bid = catch_blk ? catch_blk->id : 0;
    add_try_patch(ctx, try_pc, catch_bid, fin_bid);
}

/* Catch */
XR_FUNC void xi_emit_catch(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)v;
    emit_inst(ctx, CREATE_ABC(OP_CATCH, dst, 0, 0));
}

/* Finally */
XR_FUNC void xi_emit_finally(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)v; (void)dst;
    emit_inst(ctx, CREATE_ABC(OP_FINALLY, 0, 0, 0));
}

/* End try */
XR_FUNC void xi_emit_end_try(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)v; (void)dst;
    emit_inst(ctx, CREATE_ABC(OP_END_TRY, 0, 0, 0));
}

/* Defer: args[0]=callee, args[1..n]=call arguments.
 * OP_DEFER A B — closure at R[A], arguments at R[A+1]..R[A+B]. */
XR_FUNC void xi_emit_defer(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t nargs = (uint8_t)(v->nargs - 1);

    /* Reserve consecutive register window: dst, dst+1, ..., dst+nargs */
    {
        uint8_t top = (uint8_t)(dst + nargs + 1);
        if (top > ctx->max_reg) ctx->max_reg = top;
    }

    /* Move callee into dst */
    uint8_t callee = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    if (callee != dst)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));

    /* Move arguments into consecutive slots after callee */
    for (uint16_t a = 1; a < v->nargs; a++) {
        uint8_t arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK) return;
        uint8_t target = (uint8_t)(dst + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }

    emit_inst(ctx, CREATE_ABC(OP_DEFER, dst, nargs, 0));
}

/* ========== Coroutine ========== */

/* Go: spawn coroutine, return Task handle for await.
 * Uses OP_GO which creates an XrTask and supports
 * scope tracking, link mode, and continuation stealing. */
XR_FUNC void xi_emit_go(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t nargs = (uint8_t)(v->nargs - 1);

    {
        uint8_t call_top = (uint8_t)(dst + nargs + 1);
        if (call_top > ctx->max_reg) ctx->max_reg = call_top;
    }

    uint8_t callee = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    if (callee != dst)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));

    for (uint16_t a = 1; a < v->nargs; a++) {
        uint8_t arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK) return;
        uint8_t target = (uint8_t)(dst + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }

    /* C field: bits[0:6] = nargs, bit 7 = fire-and-forget (0 for now) */
    emit_inst(ctx, CREATE_ABC(OP_GO, dst, dst, nargs));

    /* NOP A=3: link_mode annotation (read by vm_go) */
    int link_mode = (int)v->aux_int;
    if (link_mode != 0) {
        emit_inst(ctx, CREATE_ABx(OP_NOP, 3, link_mode));
    }
}

/* Await */
XR_FUNC void xi_emit_await(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t task = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    int flags = (int)v->aux_int;
    bool is_any  = (flags & 1) != 0;
    bool is_all  = (flags & 2) != 0;
    bool is_any_success = (flags & 4) != 0;
    if (is_any_success) {
        emit_inst(ctx, CREATE_ABC(OP_AWAIT_ANY, dst, task, 1));
    } else if (is_any) {
        emit_inst(ctx, CREATE_ABC(OP_AWAIT_ANY, dst, task, 0));
    } else if (is_all) {
        emit_inst(ctx, CREATE_ABx(OP_AWAIT_ALL, dst, task));
    } else if (v->nargs >= 2) {
        uint8_t timeout = reg_of(ctx, v->args[1]);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_AWAIT_TIMEOUT, dst, task, timeout));
    } else {
        emit_inst(ctx, CREATE_ABC(OP_AWAIT, dst, task, 0));
    }
}

/* Yield */
XR_FUNC void xi_emit_yield(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)v; (void)dst;
    emit_inst(ctx, CREATE_ABC(OP_YIELD, 0, 0, 0));
}

/* Channel new */
XR_FUNC void xi_emit_chan_new(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    uint8_t buf = 0;
    if (v->nargs >= 1 && v->args[0]->op == XI_CONST) {
        buf = (uint8_t)v->args[0]->aux_int;
    }
    emit_inst(ctx, CREATE_ABx(OP_CHAN_NEW, dst, buf));
}

/* Channel send */
XR_FUNC void xi_emit_chan_send(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t ch = reg_of(ctx, v->args[0]);
    uint8_t val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_CHAN_SEND, 0, ch, val));
}

/* Channel recv */
XR_FUNC void xi_emit_chan_recv(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t ch = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_CHAN_RECV, dst, ch, 0));
}

/* Non-blocking channel try-send: returns bool (success/failure) */
XR_FUNC void xi_emit_chan_try_send(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t ch = reg_of(ctx, v->args[0]);
    uint8_t val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_CHAN_TRY_SEND, dst, ch, val));
}

/* Non-blocking channel try-recv: returns value or null.
 * OP_CHAN_TRY_RECV writes R[dst] (value) and R[dst+1] (ok bool).
 * Reserve dst+1 so subsequent allocations do not reuse it. */
XR_FUNC void xi_emit_chan_try_recv(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t ch = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_CHAN_TRY_RECV, dst, ch, 0));
    if (dst + 2 > ctx->next_reg) ctx->next_reg = dst + 2;
    if (ctx->next_reg > ctx->max_reg) ctx->max_reg = ctx->next_reg;
}

/* ========== Scope ========== */

/* Scope enter */
XR_FUNC void xi_emit_scope_enter(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    emit_inst(ctx, CREATE_ABC(OP_SCOPE_ENTER, (uint8_t)v->aux_int, 0, 0));
}

/* Scope exit */
XR_FUNC void xi_emit_scope_exit(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    emit_inst(ctx, CREATE_ABC(OP_SCOPE_EXIT, (uint8_t)v->aux_int, dst, 0));
}

/* ========== Assert ========== */

XR_FUNC void xi_emit_assert(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t cond = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    int loc_k = add_const_string(ctx, (const char *)v->aux);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_ASSERT, cond, (uint8_t)loc_k,
                               (uint8_t)v->aux_int));
}

XR_FUNC void xi_emit_assert_eq(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t actual = reg_of(ctx, v->args[0]);
    uint8_t expected = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK) return;
    int loc_k = add_const_string(ctx, (const char *)v->aux);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_ASSERT_EQ, actual, expected,
                               (uint8_t)loc_k));
}

XR_FUNC void xi_emit_assert_ne(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t actual = reg_of(ctx, v->args[0]);
    uint8_t unexpected = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK) return;
    int loc_k = add_const_string(ctx, (const char *)v->aux);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_ASSERT_NE, actual, unexpected,
                               (uint8_t)loc_k));
}

/* assert_throws(fn): emit inline try-catch sequence.
 *   OP_TRY catch_offset=+6, finally=0
 *   MOVE tmp, fn_reg          ; prepare call window
 *   CALL tmp, 0, 1            ; call fn() with 0 args
 *   LOADK err_reg, "assert fail: expected throw at ..."
 *   THROW err_reg             ; fn returned normally → assertion failed
 *   CATCH tmp                 ; exception thrown → pass
 *   END_TRY */
XR_FUNC void xi_emit_assert_throws(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    (void)dst;
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t fn_reg = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;

    /* Build assertion failure message */
    const char *loc = v->aux ? (const char *)v->aux : "unknown";
    char msg[128];
    snprintf(msg, sizeof(msg), "assertion failed at %s: expected throw", loc);
    int msg_k = add_const_string(ctx, msg);
    if (ctx->status != XI_EMIT_OK) return;

    /* Allocate temp registers for the call window and error value */
    if (ctx->next_reg + 2 >= MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS); return;
    }
    uint8_t call_reg = ctx->next_reg++;
    uint8_t err_reg  = ctx->next_reg++;
    if (ctx->next_reg > ctx->max_reg) ctx->max_reg = ctx->next_reg;

    /* OP_TRY Bx = absolute catch PC.
     * Layout: TRY(+0), NOP(+1), MOVE(+2), CALL(+3), LOADK(+4), THROW(+5),
     *         CATCH(+6), END_TRY(+7)
     * catch_pc = try_pc + 6 */
    int try_pc = current_pc(ctx);
    int catch_pc = try_pc + 6;
    emit_inst(ctx, CREATE_ABx(OP_TRY, 0, catch_pc));
    emit_inst(ctx, CREATE_ABx(OP_NOP, 0, 0));    /* finally = 0 (none) */

    /* Call fn() */
    emit_inst(ctx, CREATE_ABC(OP_MOVE, call_reg, fn_reg, 0));
    emit_inst(ctx, CREATE_ABC(OP_CALL, call_reg, 0, 1));

    /* fn() returned normally → throw AssertError */
    emit_inst(ctx, CREATE_ABx(OP_LOADK, err_reg, msg_k));
    emit_inst(ctx, CREATE_ABC(OP_THROW, err_reg, 0, 0));

    /* Catch: exception was thrown → assertion passes */
    emit_inst(ctx, CREATE_ABC(OP_CATCH, call_reg, 0, 0));
    emit_inst(ctx, CREATE_ABC(OP_END_TRY, 0, 0, 0));

    /* Free temp registers */
    free_reg(ctx, call_reg);
    free_reg(ctx, err_reg);
}

/* ========== Regex Literal ========== */

XR_FUNC void xi_emit_regex_compile(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

    /* args[0] = pattern string constant, args[1] = flags string constant */
    XiValue *pat = v->args[0];
    XiValue *flg = v->args[1];
    XR_DCHECK(pat->op == XI_CONST, "regex pattern must be XI_CONST");
    XR_DCHECK(flg->op == XI_CONST, "regex flags must be XI_CONST");

    const char *pattern = (const char *)pat->aux;
    const char *flags   = (const char *)flg->aux;

    int ki_pat = add_const_string(ctx, pattern ? pattern : "");
    if (ctx->status != XI_EMIT_OK) return;
    int ki_flg = add_const_string(ctx, flags ? flags : "");
    if (ctx->status != XI_EMIT_OK) return;

    emit_inst(ctx, CREATE_ABC(OP_REGEX_COMPILE, dst, (uint8_t)ki_pat, (uint8_t)ki_flg));
}
