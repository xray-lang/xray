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
#include "../runtime/value/xtype.h"
#include <stdio.h>

static void emit_channel_transfer_annotation(EmitCtx *ctx, uint8_t mode) {
    if (mode == XR_TRANSFER_SHARE)
        return;
    emit_inst(ctx, CREATE_ABx(OP_NOP, 6, (uint32_t) mode));
}

/* ========== Exception Handling ========== */

/* Throw */
XR_FUNC void xi_emit_throw(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_THROW, src, 0, 0));
}

/* ========== Reference Counting (dup / drop / move) ========== */

/* dup(args[0]): emit OP_DUP on the value's register (no result). */
XR_FUNC void xi_emit_retain(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_DUP, src, 0, 0));
}

/* drop(args[0]): emit OP_DROP on the value's register (no result). */
XR_FUNC void xi_emit_release(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_DROP, src, 0, 0));
}

/* move(args[0]): ownership transfer. Result register = dst; if the source
 * already lives in dst (coalesced), the move is a no-op. */
XR_FUNC void xi_emit_move(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (src != dst)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
}

/* Try: emit OP_TRY, register for patching.
 * aux = catch block (panic target). */
XR_FUNC void xi_emit_try(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    XiBlock *catch_blk = (XiBlock *) v->aux;
    int try_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABx(OP_TRY, 0, 0)); /* patched later */

    uint32_t catch_bid = catch_blk ? catch_blk->id : 0;
    add_try_patch(ctx, try_pc, catch_bid);
}

/* Catch */
XR_FUNC void xi_emit_catch(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) v;
    emit_inst(ctx, CREATE_ABC(OP_CATCH, dst, 0, 0));
}

/* End try */
XR_FUNC void xi_emit_end_try(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) v;
    (void) dst;
    emit_inst(ctx, CREATE_ABC(OP_END_TRY, 0, 0, 0));
}

/* ========== Value-Return Error Channel ========== */

XR_FUNC void xi_emit_err_set(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ERR_SET, src, 0, 0));
}

XR_FUNC void xi_emit_err_return(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ERR_RETURN, src, 0, 0));
}

XR_FUNC void xi_emit_err_check(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    /* Two modes based on result type:
     * - bool type: used as IF condition (try body) → OP_ERR_HAS into dst
     * - unit type: unconditional propagation → OP_ERR_CHECK */
    if (v->type && v->type->kind == XR_KIND_BOOL) {
        emit_inst(ctx, CREATE_ABC(OP_ERR_HAS, dst, 0, 0));
    } else {
        (void) dst;
        emit_inst(ctx, CREATE_ABC(OP_ERR_CHECK, 0, 0, 0));
    }
}

XR_FUNC void xi_emit_err_catch(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) v;
    emit_inst(ctx, CREATE_ABC(OP_ERR_CATCH, dst, 0, 0));
}

/* Defer: args[0]=callee, args[1..n]=call arguments.
 * OP_DEFER A B — closure at R[A], arguments at R[A+1]..R[A+B]. */
XR_FUNC void xi_emit_defer(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t nargs = (uint16_t) (v->nargs - 1);

    /* Reserve consecutive register window: dst, dst+1, ..., dst+nargs */
    {
        uint32_t top = dst + nargs + 1;
        if (top > ctx->max_reg)
            ctx->max_reg = top;
    }

    /* Move callee into dst */
    XiEmitReg callee = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (callee != dst)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));

    /* Move arguments into consecutive slots after callee */
    for (uint16_t a = 1; a < v->nargs; a++) {
        XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (dst + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }

    emit_inst(ctx, CREATE_ABC(OP_DEFER, dst, nargs, 0));
}

XR_FUNC void xi_emit_defer_mark(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) v;
    emit_inst(ctx, CREATE_ABC(OP_DEFER_MARK, dst, 0, 0));
}

XR_FUNC void xi_emit_defer_run_to(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg mark = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_DEFER_RUN_TO, mark, 0, 0));
}

/* ========== Coroutine ========== */

/* Go: spawn coroutine, return Task handle for await.
 * Uses OP_GO which creates an XrTask and supports
 * scope tracking, link mode, and continuation stealing. */
XR_FUNC void xi_emit_go(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t nargs = (uint16_t) (v->nargs - 1);

    {
        uint32_t call_top = (dst + nargs + 1);
        if (call_top > ctx->max_reg)
            ctx->max_reg = call_top;
    }

    XiEmitReg callee = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (callee != dst)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));

    for (uint16_t a = 1; a < v->nargs; a++) {
        XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (dst + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }

    /* C field: bits[0:6] = nargs, bit 7 = fire-and-forget */
    uint8_t c_field = nargs;
    if (v->flags & XI_FLAG_FIRE_AND_FORGET)
        c_field |= 0x80;
    emit_inst(ctx, CREATE_ABC(OP_GO, dst, dst, c_field));

    bool has_transfer_modes = false;
    for (uint16_t i = 0; i < nargs; i++) {
        if (xi_go_arg_transfer_mode(v, i) != XR_TRANSFER_SHARE) {
            has_transfer_modes = true;
            break;
        }
    }
    if (has_transfer_modes) {
        for (uint16_t base = 0; base < nargs; base += XR_TRANSFER_MODES_PER_U32) {
            uint32_t packed = 0;
            for (uint16_t slot = 0; slot < XR_TRANSFER_MODES_PER_U32 && base + slot < nargs;
                 slot++) {
                packed =
                    xr_transfer_pack_mode(packed, slot, xi_go_arg_transfer_mode(v, base + slot));
            }
            emit_inst(ctx, CREATE_ABx(OP_NOP, 5, packed));
        }
    }

    /* NOP A=3: link_mode annotation (read by vm_go) */
    int link_mode = (int) v->aux_int & XI_GO_AUX_LINK_MASK;
    if (link_mode != 0) {
        emit_inst(ctx, CREATE_ABx(OP_NOP, 3, link_mode));
    }
    if (v->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) {
        emit_inst(ctx, CREATE_ABx(OP_NOP, 4, 1));
    }
}

/* Generator call: build a coroutine-backed iterator from a generator closure.
 * Same register-packing convention as OP_GO (closure at dst, args at
 * dst+1..dst+nargs), but the coroutine is pull-driven (never scheduled) and the
 * result is an Iterator instance rather than a Task. */
XR_FUNC void xi_emit_gen_call(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t nargs = (uint16_t) (v->nargs - 1);

    {
        uint32_t call_top = (dst + nargs + 1);
        if (call_top > ctx->max_reg)
            ctx->max_reg = call_top;
    }

    XiEmitReg callee = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (callee != dst)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, callee, 0));

    for (uint16_t a = 1; a < v->nargs; a++) {
        XiEmitReg arg_reg = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (dst + a);
        if (arg_reg != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, arg_reg, 0));
    }

    emit_inst(ctx, CREATE_ABC(OP_GEN_START, dst, dst, (uint8_t) nargs));
}

/* Await */
XR_FUNC void xi_emit_await(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg task = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int flags = (int) v->aux_int;
    bool is_any = (flags & XI_AWAIT_AUX_ANY) != 0;
    bool is_all = (flags & XI_AWAIT_AUX_ALL) != 0;
    bool is_any_success = (flags & XI_AWAIT_AUX_ANY_SUCCESS) != 0;
    bool one_shot_go = (flags & XI_AWAIT_AUX_ONE_SHOT_GO) != 0;
    if (is_any_success) {
        emit_inst(ctx, CREATE_ABC(OP_AWAIT_ANY, dst, task, 1));
    } else if (is_any) {
        emit_inst(ctx, CREATE_ABC(OP_AWAIT_ANY, dst, task, 0));
    } else if (is_all) {
        emit_inst(ctx, CREATE_ABx(OP_AWAIT_ALL, dst, task));
    } else if (v->nargs >= 2) {
        XiEmitReg timeout = reg_of(ctx, v->args[1]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_AWAIT_TIMEOUT, dst, task, timeout));
    } else {
        uint8_t await_c = one_shot_go ? 0x02 : 0;
        emit_inst(ctx, CREATE_ABC(OP_AWAIT, dst, task, await_c));
    }
}

/* Yield */
XR_FUNC void xi_emit_yield(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    int64_t hint = v ? v->aux_int : XI_YIELD_AUX_IMMEDIATE;
    if (hint < 0)
        hint = XI_YIELD_AUX_IMMEDIATE;
    if (hint > MAXARG_A)
        hint = MAXARG_A;
    emit_inst(ctx, CREATE_ABC(OP_YIELD, (int) hint, 0, 0));
}

/* Generator value yield: `yield expr`. A = register holding the value to hand
 * to the driving iterator; the generator coroutine suspends. */
XR_FUNC void xi_emit_gen_yield(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (!v || v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_GEN_YIELD, src, 0, 0));
}

/* Channel new */
XR_FUNC void xi_emit_chan_new(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    uint8_t elem_tid = (uint8_t) v->aux_int;
    if (elem_tid == 0 && v->nargs >= 1 && v->args[0]->op == XI_CONST) {
        int64_t cap = v->args[0]->aux_int;
        if (cap >= 0 && (uint64_t) cap <= MAXARG_Bx) {
            emit_inst(ctx, CREATE_ABx(OP_CHAN_NEW, dst, (int) cap));
            return;
        }
    }
    if (v->nargs >= 1) {
        XiEmitReg cap_reg = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_CHAN_NEW_CAP, dst, cap_reg, elem_tid));
        return;
    }
    emit_inst(ctx, CREATE_ABx(OP_CHAN_NEW, dst, 0));
}

/* Channel send */
XR_FUNC void xi_emit_chan_send(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg ch = reg_of(ctx, v->args[0]);
    XiEmitReg val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_channel_transfer_annotation(ctx, xi_chan_send_transfer_mode(v));
    emit_inst(ctx, CREATE_ABC(OP_CHAN_SEND, dst, ch, val));
}

/* Channel recv */
static XiEmitReg chan_recv_protect_input(EmitCtx *ctx, XiEmitReg ch, XiEmitReg dst) {
    if (ch != dst && ch != (XiEmitReg) (dst + 1))
        return ch;
    if (ctx->next_reg >= MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return ch;
    }
    XiEmitReg tmp = (XiEmitReg) ctx->next_reg++;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    emit_inst(ctx, CREATE_ABC(OP_MOVE, tmp, ch, 0));
    return tmp;
}

XR_FUNC void xi_emit_chan_recv(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg ch = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if ((uint32_t) dst + 2 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    ch = chan_recv_protect_input(ctx, ch, dst);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_CHAN_RECV, dst, ch, 0));
    if (dst + 2 > ctx->next_reg)
        ctx->next_reg = dst + 2;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
}

/* Project the adjacent status slot produced by OP_CHAN_RECV / OP_CHAN_TRY_RECV.
 * The payload op reserves R[payload+1], so this is a register move only. */
XR_FUNC void xi_emit_chan_recv_status(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg recv = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if ((uint32_t) recv + 2 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg status = (XiEmitReg) (recv + 1);
    if (dst != status)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, status, 0));
}

/* Non-blocking channel try-send: returns bool (success/failure) */
XR_FUNC void xi_emit_chan_try_send(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg ch = reg_of(ctx, v->args[0]);
    XiEmitReg val = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_channel_transfer_annotation(ctx, xi_chan_send_transfer_mode(v));
    emit_inst(ctx, CREATE_ABC(OP_CHAN_TRY_SEND, dst, ch, val));
}

/* Non-blocking channel try-recv: returns value or null.
 * OP_CHAN_TRY_RECV writes R[dst] (value) and R[dst+1] (ok bool).
 * Reserve dst+1 so subsequent allocations do not reuse it. */
XR_FUNC void xi_emit_chan_try_recv(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg ch = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if ((uint32_t) dst + 2 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    ch = chan_recv_protect_input(ctx, ch, dst);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_CHAN_TRY_RECV, dst, ch, 0));
    if (dst + 2 > ctx->next_reg)
        ctx->next_reg = dst + 2;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
}

/* ch.isClosed: read the channel closed flag into dst. */
XR_FUNC void xi_emit_chan_is_closed(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg ch = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_CHAN_IS_CLOSED, dst, ch, 0));
}

/* time.after(args[0]): create a timer channel in dst. */
XR_FUNC void xi_emit_time_after(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg timeout = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_TIME_AFTER, dst, timeout, 0));
}

/* Dispose a select-owned timer channel (emitted at the select merge for the
 * `after` case). Releases the channel the compiler cannot drop across the
 * select.block suspend. R[A] = timer channel. See design/885. */
XR_FUNC void xi_emit_chan_timer_dispose(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg chan = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_CHAN_TIMER_DISPOSE, chan, 0, 0));
}

/* Blocking select wait. Channel operands are copied into a contiguous
 * register window because OP_SELECT_BLOCK uses base/count encoding. */
XR_FUNC void xi_emit_select_block(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs == 0 || v->nargs > MAXARG_B || v->aux_int < 0 ||
        (uint64_t) v->aux_int > MAXARG_C) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    uint16_t count = v->nargs;
    uint32_t top = (uint32_t) dst + count;
    if (top > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    if (top > ctx->max_reg) {
        ctx->max_reg = top;
    }

    for (uint16_t a = 0; a < count; a++) {
        XiEmitReg src = reg_of(ctx, v->args[a]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (dst + a);
        if (src != target) {
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, src, 0));
        }
    }

    emit_inst(ctx, CREATE_ABC(OP_SELECT_BLOCK, dst, count, (uint16_t) v->aux_int));
}

/* ========== Coro Built-in Module ========== */

/* Coro.method() → dedicated opcodes or OP_CORO_CTRL.
 * aux_int encodes the sub-type: 0..4 = dedicated opcodes,
 * >= XI_CORO_SUB_CTRL_BASE = OP_CORO_CTRL sub-opcode. */
XR_FUNC void xi_emit_coro_op(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    int sub = (int) v->aux_int;

    switch (sub) {
        case XI_CORO_SUB_SET_LOCAL: {
            /* Coro.setLocal(key, val) → OP_SET_LOCAL A=key B=val */
            XR_DCHECK(v->nargs >= 2, "emit coro_op: setLocal needs 2 args");
            XiEmitReg key = reg_of(ctx, v->args[0]);
            XiEmitReg val = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK)
                return;
            emit_inst(ctx, CREATE_ABC(OP_SET_LOCAL, key, val, 0));
            return;
        }
        case XI_CORO_SUB_GET_LOCAL: {
            /* Coro.getLocal(key) → OP_GET_LOCAL A=dst B=key */
            XR_DCHECK(v->nargs >= 1, "emit coro_op: getLocal needs 1 arg");
            XiEmitReg key = reg_of(ctx, v->args[0]);
            if (ctx->status != XI_EMIT_OK)
                return;
            emit_inst(ctx, CREATE_ABC(OP_GET_LOCAL, dst, key, 0));
            return;
        }
        case XI_CORO_SUB_LOCK_THREAD: {
            /* Coro.lockThread() → OP_LOCK_THREAD */
            emit_inst(ctx, CREATE_ABC(OP_LOCK_THREAD, 0, 0, 0));
            return;
        }
        case XI_CORO_SUB_UNLOCK_THREAD: {
            /* Coro.unlockThread() → OP_UNLOCK_THREAD */
            emit_inst(ctx, CREATE_ABC(OP_UNLOCK_THREAD, 0, 0, 0));
            return;
        }
    }

    /* OP_CORO_CTRL sub-opcodes: A=dst, B=first_arg_reg, C=sub_opcode */
    XR_DCHECK(sub >= XI_CORO_SUB_CTRL_BASE, "emit coro_op: unexpected sub-type");
    int ctrl_sub = sub - XI_CORO_SUB_CTRL_BASE;
    XiEmitReg b_reg = 0;
    if (v->nargs >= 1) {
        b_reg = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
    }
    emit_inst(ctx, CREATE_ABC(OP_CORO_CTRL, dst, b_reg, (uint8_t) ctrl_sub));
}

/* ========== Scope ========== */

/* Scope enter */
XR_FUNC void xi_emit_scope_enter(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    emit_inst(ctx, CREATE_ABC(OP_SCOPE_ENTER, (uint8_t) v->aux_int, 0, 0));
}

/* Scope exit */
XR_FUNC void xi_emit_scope_exit(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    emit_inst(ctx, CREATE_ABC(OP_SCOPE_EXIT, (uint8_t) v->aux_int, dst, 0));
}

/* ========== Assert ========== */

XR_FUNC void xi_emit_assert(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg cond = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int loc_k = add_const_string(ctx, (const char *) v->aux);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ASSERT, cond, (uint8_t) loc_k, (uint8_t) v->aux_int));
}

XR_FUNC void xi_emit_assert_eq(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg actual = reg_of(ctx, v->args[0]);
    XiEmitReg expected = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int loc_k = add_const_string(ctx, (const char *) v->aux);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ASSERT_EQ, actual, expected, (uint8_t) loc_k));
}

XR_FUNC void xi_emit_assert_ne(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg actual = reg_of(ctx, v->args[0]);
    XiEmitReg unexpected = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    int loc_k = add_const_string(ctx, (const char *) v->aux);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ASSERT_NE, actual, unexpected, (uint8_t) loc_k));
}

/* assert_throws(fn): pass iff fn() faults — either via the value-return
 * error channel (throw <enum>) OR via a panic (div-by-zero, expr!, …).
 * An OP_TRY panic handler wraps the call so panics are caught too.
 *
 * Layout (relative to try_pc):
 *   +0  TRY  Bx=Lpanic            ; panic handler (catch_offset patched)
 *   +1  NOP                       ; finally placeholder (consumed by OP_TRY)
 *   +2  MOVE call_reg, fn_reg
 *   +3  CALL call_reg, 0, 1       ; call fn()
 *   +4  END_TRY                   ; no panic → pop handler
 *   +5  ERR_HAS check_reg         ; pending error?
 *   +6  TEST  check_reg, 1        ; has error → exec next; no error → skip
 *   +7  JMP  -> Lpass
 *   +8  LOADK err_reg, msg        ; no fault at all → assertion fails
 *   +9  ERR_RETURN err_reg
 *  +10  Lpass: ERR_CATCH call_reg ; error-channel fault → clear, pass
 *  +11  JMP  -> Lend
 *  +12  Lpanic: CATCH call_reg    ; panic unwind lands here; clear panic
 *  +13  END_TRY                   ; pop handler
 *  +14  Lend: (continue) */
XR_FUNC void xi_emit_assert_throws(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg fn_reg = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;

    const char *loc = v->aux ? (const char *) v->aux : "unknown";
    char msg[128];
    snprintf(msg, sizeof(msg), "assertion failed at %s: expected throw", loc);
    int msg_k = add_const_string(ctx, msg);
    if (ctx->status != XI_EMIT_OK)
        return;

    if (ctx->next_reg + 3 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg call_reg = (XiEmitReg) ctx->next_reg++;
    XiEmitReg err_reg = (XiEmitReg) ctx->next_reg++;
    XiEmitReg check_reg = (XiEmitReg) ctx->next_reg++;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;

    int try_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABx(OP_TRY, 0, 0)); /* +0 catch offset patched below */
    emit_inst(ctx, CREATE_ABx(OP_NOP, 0, 0)); /* +1 finally placeholder */

    emit_inst(ctx, CREATE_ABC(OP_MOVE, call_reg, fn_reg, 0)); /* +2 */
    emit_inst(ctx, CREATE_ABC(OP_CALL, call_reg, 0, 1));      /* +3 */

    emit_inst(ctx, CREATE_ABC(OP_END_TRY, 0, 0, 0));         /* +4 no panic → pop */
    emit_inst(ctx, CREATE_ABC(OP_ERR_HAS, check_reg, 0, 0)); /* +5 */
    emit_inst(ctx, CREATE_ABC(OP_TEST, check_reg, 1, 0));    /* +6 */
    emit_inst(ctx, CREATE_sJ(OP_JMP, 2));                    /* +7 → Lpass(+10) */

    emit_inst(ctx, CREATE_ABx(OP_LOADK, err_reg, msg_k));     /* +8 */
    emit_inst(ctx, CREATE_ABC(OP_ERR_RETURN, err_reg, 0, 0)); /* +9 */

    emit_inst(ctx, CREATE_ABC(OP_ERR_CATCH, call_reg, 0, 0)); /* +10 Lpass */
    emit_inst(ctx, CREATE_sJ(OP_JMP, 2));                     /* +11 → Lend(+14) */

    emit_inst(ctx, CREATE_ABC(OP_CATCH, call_reg, 0, 0)); /* +12 Lpanic */
    emit_inst(ctx, CREATE_ABC(OP_END_TRY, 0, 0, 0));      /* +13 pop */
    /* +14 Lend */

    /* Patch OP_TRY catch_offset to the absolute pc of Lpanic (+12). */
    XrInstruction *code = PROTO_CODE_BASE(ctx->proto);
    code[try_pc] = CREATE_ABx(OP_TRY, 0, try_pc + 12);

    free_reg(ctx, call_reg);
    free_reg(ctx, err_reg);
    free_reg(ctx, check_reg);
}

/* ========== Regex Literal ========== */

XR_FUNC void xi_emit_regex_compile(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    /* args[0] = pattern string constant, args[1] = flags string constant */
    XiValue *pat = v->args[0];
    XiValue *flg = v->args[1];
    XR_DCHECK(pat->op == XI_CONST, "regex pattern must be XI_CONST");
    XR_DCHECK(flg->op == XI_CONST, "regex flags must be XI_CONST");

    const char *pattern = (const char *) pat->aux;
    const char *flags = (const char *) flg->aux;

    int ki_pat = add_const_string(ctx, pattern ? pattern : "");
    if (ctx->status != XI_EMIT_OK)
        return;
    int ki_flg = add_const_string(ctx, flags ? flags : "");
    if (ctx->status != XI_EMIT_OK)
        return;

    uint16_t pat_arg;
    uint16_t flg_arg;
    if (!xi_emit_const_index_to_c(ctx, ki_pat, &pat_arg) ||
        !xi_emit_const_index_to_c(ctx, ki_flg, &flg_arg))
        return;

    emit_inst(ctx, CREATE_ABC(OP_REGEX_COMPILE, dst, pat_arg, flg_arg));
}
