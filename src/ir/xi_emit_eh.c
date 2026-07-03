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
#include "../shared/xr_elem_type.h"
#include <stdio.h>

static void emit_channel_transfer_annotation(EmitCtx *ctx, uint8_t mode) {
    if (mode == XR_TRANSFER_SHARE)
        return;
    emit_inst(ctx, CREATE_ABx(OP_NOP, 6, (uint32_t) mode));
}

static uint8_t xi_emit_await_all_result_elem_type(const XiValue *v) {
    XrType *elem =
        (v && v->type && XR_TYPE_IS_ARRAY(v->type)) ? v->type->container.element_type : NULL;
    return (uint8_t) xr_tid_to_elem_type(xr_type_to_tid(elem));
}

static uint8_t xi_emit_array_value_elem_type(const XiValue *v) {
    XrType *elem =
        (v && v->type && XR_TYPE_IS_ARRAY(v->type)) ? v->type->container.element_type : NULL;
    return (uint8_t) xr_tid_to_elem_type(xr_type_to_tid(elem));
}

static void xi_emit_load_array_zero_value(EmitCtx *ctx, XrArrayElemType elem_type, XiEmitReg dst) {
    switch (elem_type) {
        case XR_ELEM_F32:
        case XR_ELEM_F64:
            emit_inst(ctx, CREATE_AsBx(OP_LOADF, dst, 0));
            break;
        case XR_ELEM_CHAR:
            emit_inst(ctx, CREATE_AsBx(OP_LOADI, dst, 0));
            emit_inst(ctx, CREATE_ABC(OP_TOCHAR, dst, dst, 0));
            break;
        default:
            emit_inst(ctx, CREATE_AsBx(OP_LOADI, dst, 0));
            break;
    }
}

#define XI_EMIT_AWAIT_ALL_ONE_SHOT_FLAG 0x80
#define XI_EMIT_AWAIT_ALL_ELEM_MASK 0x7f

static int emit_jump_if_cmp(EmitCtx *ctx, OpCode op, XiEmitReg lhs, XiEmitReg rhs,
                            bool jump_when_true) {
    emit_inst(ctx, CREATE_ABC(op, lhs, rhs, jump_when_true ? 1 : 0));
    int jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    return jmp_pc;
}

static void patch_jump_to(EmitCtx *ctx, int jmp_pc, int target_pc) {
    PROTO_SET_CODE(ctx->proto, jmp_pc, CREATE_sJ(OP_JMP, target_pc - (jmp_pc + 1)));
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
    if (v->aux_int & XI_GO_AUX_DEFER_BATCH) {
        emit_inst(ctx, CREATE_ABx(OP_NOP, 6, 1));
    }
}

/* sys.Thread.spawn: same closure+argument register packing and transfer-mode
 * annotations as `go`, but returns a Thread<T> handle and is dispatched to a
 * real OS thread instead of the coroutine scheduler. */
XR_FUNC void xi_emit_thread_spawn(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
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

    emit_inst(ctx, CREATE_ABC(OP_THREAD_SPAWN, dst, dst, nargs));

    const char *thread_name = xi_thread_spawn_name(v);
    if (thread_name) {
        int name_k = add_const_string(ctx, thread_name);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABx(OP_NOP, 1, (uint32_t) name_k));
    }

    int64_t stack_size = xi_thread_spawn_stack_size(v);
    if (stack_size > 0) {
        int stack_k = add_const_int(ctx, stack_size);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABx(OP_NOP, 7, (uint32_t) stack_k));
    }

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
    bool into_result = (flags & XI_AWAIT_AUX_INTO_RESULT) != 0;
    if (is_any_success) {
        emit_inst(ctx, CREATE_ABC(OP_AWAIT_ANY, dst, task, 1));
    } else if (is_any) {
        emit_inst(ctx, CREATE_ABC(OP_AWAIT_ANY, dst, task, 0));
    } else if (is_all) {
        uint8_t await_all_c =
            (into_result && v->nargs >= 2 ? xi_emit_array_value_elem_type(v->args[1])
                                          : xi_emit_await_all_result_elem_type(v)) &
            XI_EMIT_AWAIT_ALL_ELEM_MASK;
        if ((flags & XI_AWAIT_AUX_AGGREGATE_ONE_SHOT) != 0)
            await_all_c |= XI_EMIT_AWAIT_ALL_ONE_SHOT_FLAG;
        if (into_result) {
            if (v->nargs < 2) {
                emit_error(ctx, XI_EMIT_ERR_INTERNAL);
                return;
            }
            XiEmitReg out = reg_of(ctx, v->args[1]);
            if (ctx->status != XI_EMIT_OK)
                return;
            emit_inst(ctx, CREATE_ABC(OP_AWAIT_ALL_INTO, out, task, await_all_c));
            if (dst != out)
                emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
        } else {
            emit_inst(ctx, CREATE_ABC(OP_AWAIT_ALL, dst, task, await_all_c));
        }
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

/* xi.par.for VM fallback: execute the batch body sequentially.
 * The VM path is a semantic oracle, not the performance implementation; AOT can
 * later replace the same IR op with persistent-worker range dispatch. */
XR_FUNC void xi_emit_par_for(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    (void) dst;
    if (!ctx || !v || v->nargs < 4 || v->aux_kind != XI_AUX_KIND_PAR_FOR || !v->aux) {
        if (ctx)
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    const XiParallelForData *data = (const XiParallelForData *) v->aux;
    if (!data->body_func) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XiEmitReg start = reg_of_cell_deref(ctx, v->args[0]);
    XiEmitReg end = reg_of_cell_deref(ctx, v->args[1]);
    XiEmitReg workers = reg_of_cell_deref(ctx, v->args[2]);
    XiEmitReg closure = reg_of(ctx, v->args[3]);
    (void) workers;
    if (ctx->status != XI_EMIT_OK)
        return;

    if (ctx->next_reg + (data->range_body ? 17 : 16) > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg end_excl = (XiEmitReg) ctx->next_reg++;
    XiEmitReg count = (XiEmitReg) ctx->next_reg++;
    XiEmitReg participants = (XiEmitReg) ctx->next_reg++;
    XiEmitReg one = (XiEmitReg) ctx->next_reg++;
    XiEmitReg max_participants = (XiEmitReg) ctx->next_reg++;
    XiEmitReg lane = (XiEmitReg) ctx->next_reg++;
    XiEmitReg base = (XiEmitReg) ctx->next_reg++;
    XiEmitReg rem = (XiEmitReg) ctx->next_reg++;
    XiEmitReg lane_count = (XiEmitReg) ctx->next_reg++;
    XiEmitReg extra_before = (XiEmitReg) ctx->next_reg++;
    XiEmitReg offset = (XiEmitReg) ctx->next_reg++;
    XiEmitReg iter = (XiEmitReg) ctx->next_reg++;
    XiEmitReg limit = (XiEmitReg) ctx->next_reg++;
    XiEmitReg call_base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg += data->range_body ? 4 : 3;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;

    emit_inst(ctx, CREATE_ABC(OP_MOVE, end_excl, end, 0));
    if (data->inclusive_end)
        emit_inst(ctx, CREATE_ABC(OP_ADDI, end_excl, end_excl, 1));
    emit_inst(ctx, CREATE_ABC(OP_SUB, count, end_excl, start));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, one, 1));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, max_participants, 256));

    int empty_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, start, end_excl, false);

    emit_inst(ctx, CREATE_ABC(OP_MOVE, participants, workers, 0));
    int participants_gt_one_jmp = emit_jump_if_cmp(ctx, OP_LT, one, participants, false);
    int participants_one_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int participants_set_one_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_MOVE, participants, one, 0));
    int participants_one_done_pc = current_pc(ctx);
    patch_jump_to(ctx, participants_gt_one_jmp, participants_set_one_pc);
    patch_jump_to(ctx, participants_one_done_jmp, participants_one_done_pc);

    int count_lt_participants_jmp = emit_jump_if_cmp(ctx, OP_LT, count, participants, true);
    int count_clamp_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int count_clamp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_MOVE, participants, count, 0));
    int count_clamp_done_pc = current_pc(ctx);
    patch_jump_to(ctx, count_lt_participants_jmp, count_clamp_pc);
    patch_jump_to(ctx, count_clamp_done_jmp, count_clamp_done_pc);

    int max_lt_participants_jmp =
        emit_jump_if_cmp(ctx, OP_LT, max_participants, participants, true);
    int max_clamp_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int max_clamp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_MOVE, participants, max_participants, 0));
    int max_clamp_done_pc = current_pc(ctx);
    patch_jump_to(ctx, max_lt_participants_jmp, max_clamp_pc);
    patch_jump_to(ctx, max_clamp_done_jmp, max_clamp_done_pc);

    emit_inst(ctx, CREATE_ABC(OP_DIV, base, count, participants));
    emit_inst(ctx, CREATE_ABC(OP_MOD, rem, count, participants));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, lane, 0));

    int outer_pc = current_pc(ctx);
    int outer_exit_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, lane, participants, false);

    emit_inst(ctx, CREATE_ABC(OP_MOVE, lane_count, base, 0));
    int lane_lt_rem_for_count_jmp = emit_jump_if_cmp(ctx, OP_LT, lane, rem, true);
    int lane_count_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int lane_count_inc_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_ADDI, lane_count, lane_count, 1));
    int lane_count_done_pc = current_pc(ctx);
    patch_jump_to(ctx, lane_lt_rem_for_count_jmp, lane_count_inc_pc);
    patch_jump_to(ctx, lane_count_done_jmp, lane_count_done_pc);

    emit_inst(ctx, CREATE_ABC(OP_MOVE, extra_before, rem, 0));
    int lane_lt_rem_for_extra_jmp = emit_jump_if_cmp(ctx, OP_LT, lane, rem, true);
    int extra_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int extra_set_lane_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_MOVE, extra_before, lane, 0));
    int extra_done_pc = current_pc(ctx);
    patch_jump_to(ctx, lane_lt_rem_for_extra_jmp, extra_set_lane_pc);
    patch_jump_to(ctx, extra_done_jmp, extra_done_pc);

    emit_inst(ctx, CREATE_ABC(OP_MUL, offset, lane, base));
    emit_inst(ctx, CREATE_ABC(OP_ADD, offset, offset, extra_before));
    emit_inst(ctx, CREATE_ABC(OP_ADD, iter, start, offset));
    emit_inst(ctx, CREATE_ABC(OP_ADD, limit, iter, lane_count));

    if (data->range_body) {
        emit_inst(ctx, CREATE_ABC(OP_MOVE, call_base, closure, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 1), iter, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 2), limit, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 3), lane, 0));
        emit_inst(ctx, CREATE_ABC(OP_CALL_STATIC, call_base, 3, 1));
    } else {
        int loop_pc = current_pc(ctx);
        int inner_exit_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, iter, limit, false);

        emit_inst(ctx, CREATE_ABC(OP_MOVE, call_base, closure, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 1), iter, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 2), lane, 0));
        emit_inst(ctx, CREATE_ABC(OP_CALL_STATIC, call_base, 2, 1));
        emit_inst(ctx, CREATE_ABC(OP_ADDI, iter, iter, 1));

        int back_jmp_pc = current_pc(ctx);
        emit_inst(ctx, CREATE_sJ(OP_JMP, loop_pc - (back_jmp_pc + 1)));

        int inner_exit_pc = current_pc(ctx);
        patch_jump_to(ctx, inner_exit_jmp_pc, inner_exit_pc);
    }
    emit_inst(ctx, CREATE_ABC(OP_ADDI, lane, lane, 1));
    int outer_back_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, outer_pc - (outer_back_jmp_pc + 1)));

    int exit_pc = current_pc(ctx);
    patch_jump_to(ctx, empty_jmp_pc, exit_pc);
    patch_jump_to(ctx, outer_exit_jmp_pc, exit_pc);
}

/* xi.par.collect VM fallback: execute stable lanes sequentially and collect
 * body results into a deterministic Array<T>. */
/* Shared register set for the sequential VM lane loops emitted by
 * xi.par.collect / xi.par.reduce. AOT runs real workers; the VM keeps the
 * same lane split semantics so lane ids and per-lane chunk bounds match. */
typedef struct {
    XiEmitReg start, end_excl, count, workers;
    XiEmitReg participants, one, max_participants;
    XiEmitReg lane, base, rem, lane_count, extra_before, offset, iter, limit;
} ParLaneRegs;

/* Clamp participants to [1, min(count, 256)] and derive base/rem. */
static void emit_par_participants_clamp(EmitCtx *ctx, const ParLaneRegs *r) {
    emit_inst(ctx, CREATE_ABC(OP_MOVE, r->participants, r->workers, 0));
    int participants_gt_one_jmp = emit_jump_if_cmp(ctx, OP_LT, r->one, r->participants, false);
    int participants_one_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int participants_set_one_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_MOVE, r->participants, r->one, 0));
    int participants_one_done_pc = current_pc(ctx);
    patch_jump_to(ctx, participants_gt_one_jmp, participants_set_one_pc);
    patch_jump_to(ctx, participants_one_done_jmp, participants_one_done_pc);

    int count_lt_participants_jmp = emit_jump_if_cmp(ctx, OP_LT, r->count, r->participants, true);
    int count_clamp_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int count_clamp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_MOVE, r->participants, r->count, 0));
    int count_clamp_done_pc = current_pc(ctx);
    patch_jump_to(ctx, count_lt_participants_jmp, count_clamp_pc);
    patch_jump_to(ctx, count_clamp_done_jmp, count_clamp_done_pc);

    int max_lt_participants_jmp =
        emit_jump_if_cmp(ctx, OP_LT, r->max_participants, r->participants, true);
    int max_clamp_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int max_clamp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_MOVE, r->participants, r->max_participants, 0));
    int max_clamp_done_pc = current_pc(ctx);
    patch_jump_to(ctx, max_lt_participants_jmp, max_clamp_pc);
    patch_jump_to(ctx, max_clamp_done_jmp, max_clamp_done_pc);

    emit_inst(ctx, CREATE_ABC(OP_DIV, r->base, r->count, r->participants));
    emit_inst(ctx, CREATE_ABC(OP_MOD, r->rem, r->count, r->participants));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, r->lane, 0));
}

/* Per-lane chunk bounds: lanes below `rem` take one extra item; iter/limit
 * bracket this lane's [start+offset, start+offset+lane_count) range. */
static void emit_par_lane_bounds(EmitCtx *ctx, const ParLaneRegs *r) {
    emit_inst(ctx, CREATE_ABC(OP_MOVE, r->lane_count, r->base, 0));
    int lane_lt_rem_for_count_jmp = emit_jump_if_cmp(ctx, OP_LT, r->lane, r->rem, true);
    int lane_count_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int lane_count_inc_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_ADDI, r->lane_count, r->lane_count, 1));
    int lane_count_done_pc = current_pc(ctx);
    patch_jump_to(ctx, lane_lt_rem_for_count_jmp, lane_count_inc_pc);
    patch_jump_to(ctx, lane_count_done_jmp, lane_count_done_pc);

    emit_inst(ctx, CREATE_ABC(OP_MOVE, r->extra_before, r->rem, 0));
    int lane_lt_rem_for_extra_jmp = emit_jump_if_cmp(ctx, OP_LT, r->lane, r->rem, true);
    int extra_done_jmp = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int extra_set_lane_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_ABC(OP_MOVE, r->extra_before, r->lane, 0));
    int extra_done_pc = current_pc(ctx);
    patch_jump_to(ctx, lane_lt_rem_for_extra_jmp, extra_set_lane_pc);
    patch_jump_to(ctx, extra_done_jmp, extra_done_pc);

    emit_inst(ctx, CREATE_ABC(OP_MUL, r->offset, r->lane, r->base));
    emit_inst(ctx, CREATE_ABC(OP_ADD, r->offset, r->offset, r->extra_before));
    emit_inst(ctx, CREATE_ABC(OP_ADD, r->iter, r->start, r->offset));
    emit_inst(ctx, CREATE_ABC(OP_ADD, r->limit, r->iter, r->lane_count));
}

/* Allocate the lane loop registers from ctx (extra = additional scratch
 * regs the caller needs after the shared set). Returns false when the
 * register file cannot fit. */
static bool par_lane_regs_alloc(EmitCtx *ctx, ParLaneRegs *r, int extra) {
    if (ctx->next_reg + 13 + extra > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return false;
    }
    r->end_excl = (XiEmitReg) ctx->next_reg++;
    r->count = (XiEmitReg) ctx->next_reg++;
    r->participants = (XiEmitReg) ctx->next_reg++;
    r->one = (XiEmitReg) ctx->next_reg++;
    r->max_participants = (XiEmitReg) ctx->next_reg++;
    r->lane = (XiEmitReg) ctx->next_reg++;
    r->base = (XiEmitReg) ctx->next_reg++;
    r->rem = (XiEmitReg) ctx->next_reg++;
    r->lane_count = (XiEmitReg) ctx->next_reg++;
    r->extra_before = (XiEmitReg) ctx->next_reg++;
    r->offset = (XiEmitReg) ctx->next_reg++;
    r->iter = (XiEmitReg) ctx->next_reg++;
    r->limit = (XiEmitReg) ctx->next_reg++;
    return true;
}

/* Direct-lane-write collect: the body writes result lanes itself, so each
 * lane array is resized up front and the closure receives (iter[, limit],
 * lane) with no per-item collect store. */
static void emit_par_collect_direct_lanes(EmitCtx *ctx, XiValue *v, XiEmitReg dst, XiEmitReg start,
                                          XiEmitReg end, XiEmitReg workers, XiEmitReg closure) {
    const XiParallelCollectData *data = (const XiParallelCollectData *) v->aux;
    if (data->lane_count < 1 || data->lane_count > 16 ||
        v->nargs < (uint16_t) (4u + data->lane_count)) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg targets[16];
    for (uint16_t i = 0; i < data->lane_count; i++)
        targets[i] = reg_of(ctx, v->args[4 + i]);
    if (ctx->status != XI_EMIT_OK)
        return;
    bool body_range = data->body_func && data->body_func->nparams == 3;
    if (!body_range && (!data->body_func || data->body_func->nparams != 2)) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    ParLaneRegs r;
    memset(&r, 0, sizeof(r));
    r.start = start;
    r.workers = workers;
    if (!par_lane_regs_alloc(ctx, &r, 5))
        return;
    XiEmitReg fill_value = (XiEmitReg) ctx->next_reg++;
    XiEmitReg call_base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg += 4;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;

    emit_inst(ctx, CREATE_ABC(OP_MOVE, r.end_excl, end, 0));
    if (data->inclusive_end)
        emit_inst(ctx, CREATE_ABC(OP_ADDI, r.end_excl, r.end_excl, 1));
    emit_inst(ctx, CREATE_ABC(OP_SUB, r.count, r.end_excl, r.start));
    for (uint16_t i = 0; i < data->lane_count; i++) {
        uint8_t elem_tid = 0;
        if (v->args[4 + i] && v->args[4 + i]->type && XR_TYPE_IS_ARRAY(v->args[4 + i]->type) &&
            v->args[4 + i]->type->container.element_type)
            elem_tid = xr_type_to_tid(v->args[4 + i]->type->container.element_type);
        XrArrayElemType elem_type = xr_tid_to_elem_type(elem_tid);
        if (elem_type == XR_ELEM_ANY)
            emit_inst(ctx, CREATE_ABC(OP_LOADNULL, fill_value, 0, 0));
        else
            xi_emit_load_array_zero_value(ctx, elem_type, fill_value);
        emit_inst(ctx, CREATE_ABC(OP_ARRAY_RESIZE, targets[i], r.count, fill_value));
    }
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, r.one, 1));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, r.max_participants, 256));

    int empty_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, r.start, r.end_excl, false);

    emit_par_participants_clamp(ctx, &r);

    int outer_pc = current_pc(ctx);
    int outer_exit_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, r.lane, r.participants, false);

    emit_par_lane_bounds(ctx, &r);

    if (body_range) {
        emit_inst(ctx, CREATE_ABC(OP_MOVE, call_base, closure, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 1), r.iter, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 2), r.limit, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 3), r.lane, 0));
        emit_inst(ctx, CREATE_ABC(OP_CALL_STATIC, call_base, 3, 0));
    } else {
        int loop_pc = current_pc(ctx);
        int inner_exit_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, r.iter, r.limit, false);

        emit_inst(ctx, CREATE_ABC(OP_MOVE, call_base, closure, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 1), r.iter, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 2), r.lane, 0));
        emit_inst(ctx, CREATE_ABC(OP_CALL_STATIC, call_base, 2, 0));
        emit_inst(ctx, CREATE_ABC(OP_ADDI, r.iter, r.iter, 1));

        int back_jmp_pc = current_pc(ctx);
        emit_inst(ctx, CREATE_sJ(OP_JMP, loop_pc - (back_jmp_pc + 1)));

        int inner_exit_pc = current_pc(ctx);
        patch_jump_to(ctx, inner_exit_jmp_pc, inner_exit_pc);
    }
    emit_inst(ctx, CREATE_ABC(OP_ADDI, r.lane, r.lane, 1));
    int outer_back_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, outer_pc - (outer_back_jmp_pc + 1)));

    int exit_pc = current_pc(ctx);
    patch_jump_to(ctx, empty_jmp_pc, exit_pc);
    patch_jump_to(ctx, outer_exit_jmp_pc, exit_pc);
    if (data->into_result) {
        emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
    } else {
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, targets[0], 0));
    }
}

XR_FUNC void xi_emit_par_collect(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (!ctx || !v || v->nargs < 4 || v->aux_kind != XI_AUX_KIND_PAR_COLLECT || !v->aux) {
        if (ctx)
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    const XiParallelCollectData *data = (const XiParallelCollectData *) v->aux;
    if (!data->body_func || (data->into_result && v->nargs < 5)) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XiEmitReg start = reg_of_cell_deref(ctx, v->args[0]);
    XiEmitReg end = reg_of_cell_deref(ctx, v->args[1]);
    XiEmitReg workers = reg_of_cell_deref(ctx, v->args[2]);
    XiEmitReg closure = reg_of(ctx, v->args[3]);
    if (data->direct_lane_writes) {
        emit_par_collect_direct_lanes(ctx, v, dst, start, end, workers, closure);
        return;
    }

    XiEmitReg target_array = data->into_result ? reg_of(ctx, v->args[4]) : dst;
    if (ctx->status != XI_EMIT_OK)
        return;

    ParLaneRegs r;
    memset(&r, 0, sizeof(r));
    r.start = start;
    r.workers = workers;
    if (!par_lane_regs_alloc(ctx, &r, 6))
        return;
    XiEmitReg collect_idx = (XiEmitReg) ctx->next_reg++;
    XiEmitReg item_result = (XiEmitReg) ctx->next_reg++;
    XiEmitReg fill_value = (XiEmitReg) ctx->next_reg++;
    XiEmitReg call_base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg += 3;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;

    uint8_t elem_tid = xr_type_to_tid(data->element_type);
    if (elem_tid == 0 && data->into_result && v->args[4] && v->args[4]->type &&
        XR_TYPE_IS_ARRAY(v->args[4]->type) && v->args[4]->type->container.element_type)
        elem_tid = xr_type_to_tid(v->args[4]->type->container.element_type);
    if (elem_tid == 0 && v->type && XR_TYPE_IS_ARRAY(v->type) && v->type->container.element_type)
        elem_tid = xr_type_to_tid(v->type->container.element_type);
    uint8_t array_c = (uint8_t) (elem_tid << 2);
    XrArrayElemType elem_type = xr_tid_to_elem_type(elem_tid);

    emit_inst(ctx, CREATE_ABC(OP_MOVE, r.end_excl, end, 0));
    if (data->inclusive_end)
        emit_inst(ctx, CREATE_ABC(OP_ADDI, r.end_excl, r.end_excl, 1));
    emit_inst(ctx, CREATE_ABC(OP_SUB, r.count, r.end_excl, r.start));
    if (!data->into_result)
        emit_inst(ctx, CREATE_ABC(OP_ARRAY_NEW_CAP, target_array, r.count, array_c));
    if (elem_type == XR_ELEM_ANY)
        emit_inst(ctx, CREATE_ABC(OP_LOADNULL, fill_value, 0, 0));
    else
        xi_emit_load_array_zero_value(ctx, elem_type, fill_value);
    emit_inst(ctx, CREATE_ABC(OP_ARRAY_RESIZE, target_array, r.count, fill_value));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, r.one, 1));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, r.max_participants, 256));

    int empty_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, r.start, r.end_excl, false);

    emit_par_participants_clamp(ctx, &r);

    int outer_pc = current_pc(ctx);
    int outer_exit_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, r.lane, r.participants, false);

    emit_par_lane_bounds(ctx, &r);

    int loop_pc = current_pc(ctx);
    int inner_exit_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, r.iter, r.limit, false);

    emit_inst(ctx, CREATE_ABC(OP_MOVE, call_base, closure, 0));
    emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 1), r.iter, 0));
    emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 2), r.lane, 0));
    emit_inst(ctx, CREATE_ABC(OP_CALL_STATIC, call_base, 2, 1));
    emit_inst(ctx, CREATE_ABC(OP_MOVE, item_result, call_base, 0));
    emit_inst(ctx, CREATE_ABC(OP_SUB, collect_idx, r.iter, r.start));
    emit_inst(ctx, CREATE_ABC(OP_INDEX_SET, target_array, collect_idx, item_result));
    emit_inst(ctx, CREATE_ABC(OP_ADDI, r.iter, r.iter, 1));

    int back_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, loop_pc - (back_jmp_pc + 1)));

    int inner_exit_pc = current_pc(ctx);
    patch_jump_to(ctx, inner_exit_jmp_pc, inner_exit_pc);
    emit_inst(ctx, CREATE_ABC(OP_ADDI, r.lane, r.lane, 1));
    int outer_back_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, outer_pc - (outer_back_jmp_pc + 1)));

    int exit_pc = current_pc(ctx);
    patch_jump_to(ctx, empty_jmp_pc, exit_pc);
    patch_jump_to(ctx, outer_exit_jmp_pc, exit_pc);
    if (data->into_result)
        emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
}

/* xi.par.reduce VM fallback: execute the range sequentially and combine each
 * item result into the accumulator.  AOT lowers the same op to the native
 * range reducer; VM keeps the semantic contract aligned. */
XR_FUNC void xi_emit_par_reduce(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (!ctx || !v || v->nargs < 6 || v->aux_kind != XI_AUX_KIND_PAR_REDUCE || !v->aux) {
        if (ctx)
            emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    const XiParallelReduceData *data = (const XiParallelReduceData *) v->aux;
    if (!data->body_func || !data->combine_func) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XiEmitReg start = reg_of_cell_deref(ctx, v->args[0]);
    XiEmitReg end = reg_of_cell_deref(ctx, v->args[1]);
    XiEmitReg workers = reg_of_cell_deref(ctx, v->args[2]);
    XiEmitReg initial = reg_of_cell_deref(ctx, v->args[3]);
    XiEmitReg body_closure = reg_of(ctx, v->args[4]);
    XiEmitReg combine_closure = reg_of(ctx, v->args[5]);
    (void) workers;
    if (ctx->status != XI_EMIT_OK)
        return;

    ParLaneRegs r;
    memset(&r, 0, sizeof(r));
    r.start = start;
    r.workers = workers;
    if (!par_lane_regs_alloc(ctx, &r, 5))
        return;
    XiEmitReg item_result = (XiEmitReg) ctx->next_reg++;
    XiEmitReg call_base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg += 4;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;

    emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, initial, 0));
    emit_inst(ctx, CREATE_ABC(OP_MOVE, r.end_excl, end, 0));
    if (data->inclusive_end)
        emit_inst(ctx, CREATE_ABC(OP_ADDI, r.end_excl, r.end_excl, 1));
    emit_inst(ctx, CREATE_ABC(OP_SUB, r.count, r.end_excl, r.start));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, r.one, 1));
    emit_inst(ctx, CREATE_AsBx(OP_LOADI, r.max_participants, 256));

    int empty_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, r.start, r.end_excl, false);

    emit_par_participants_clamp(ctx, &r);

    int outer_pc = current_pc(ctx);
    int outer_exit_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, r.lane, r.participants, false);

    emit_par_lane_bounds(ctx, &r);

    if (data->range_body) {
        emit_inst(ctx, CREATE_ABC(OP_MOVE, call_base, body_closure, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 1), r.iter, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 2), r.limit, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 3), r.lane, 0));
        emit_inst(ctx, CREATE_ABC(OP_CALL_STATIC, call_base, 3, 1));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, item_result, call_base, 0));

        emit_inst(ctx, CREATE_ABC(OP_MOVE, call_base, combine_closure, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 1), dst, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 2), item_result, 0));
        emit_inst(ctx, CREATE_ABC(OP_CALL_STATIC, call_base, 2, 1));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, call_base, 0));
    } else {
        int loop_pc = current_pc(ctx);
        int inner_exit_jmp_pc = emit_jump_if_cmp(ctx, OP_LT, r.iter, r.limit, false);

        emit_inst(ctx, CREATE_ABC(OP_MOVE, call_base, body_closure, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 1), r.iter, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 2), r.lane, 0));
        emit_inst(ctx, CREATE_ABC(OP_CALL_STATIC, call_base, 2, 1));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, item_result, call_base, 0));

        emit_inst(ctx, CREATE_ABC(OP_MOVE, call_base, combine_closure, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 1), dst, 0));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, (XiEmitReg) (call_base + 2), item_result, 0));
        emit_inst(ctx, CREATE_ABC(OP_CALL_STATIC, call_base, 2, 1));
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, call_base, 0));

        emit_inst(ctx, CREATE_ABC(OP_ADDI, r.iter, r.iter, 1));

        int back_jmp_pc = current_pc(ctx);
        emit_inst(ctx, CREATE_sJ(OP_JMP, loop_pc - (back_jmp_pc + 1)));

        int inner_exit_pc = current_pc(ctx);
        patch_jump_to(ctx, inner_exit_jmp_pc, inner_exit_pc);
    }
    emit_inst(ctx, CREATE_ABC(OP_ADDI, r.lane, r.lane, 1));
    int outer_back_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, outer_pc - (outer_back_jmp_pc + 1)));

    int exit_pc = current_pc(ctx);
    patch_jump_to(ctx, empty_jmp_pc, exit_pc);
    patch_jump_to(ctx, outer_exit_jmp_pc, exit_pc);
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
