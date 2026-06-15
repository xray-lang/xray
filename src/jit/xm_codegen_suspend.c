/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_suspend.c - ARM64 JIT suspend-point emission
 */

#ifdef __aarch64__

#include "xm_codegen_internal.h"
#include "xm_helper_table.h"
#include "../base/xchecks.h"

bool xm_emit_suspend_op(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    XR_DCHECK(ctx != NULL, "emit_suspend_op: NULL ctx");
    XR_DCHECK(ins != NULL, "emit_suspend_op: NULL ins");

    uint32_t suspend_id = 0;
    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t vi = XM_REF_INDEX(ins->dst);
        if (vi < ctx->func->nvreg)
            suspend_id = ctx->func->vregs[vi].call_arg_start;
    }

    int64_t discard_result = 0;
    if (xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        discard_result = ctx->func->consts[ci].val.i64;
    }

    A64Reg fast_result_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    a64_load_imm64(&ctx->buf, SCRATCH_REG2, (uint64_t) XM_DEOPT_MARKER);
    a64_buf_emit(&ctx->buf, a64_cmp(fast_result_reg, SCRATCH_REG2));
    uint32_t bne_fast_done = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_nop());
    a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_CALL_RESULT_TAG_OFFSET));
    a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG2, 0));
    uint32_t beq_slow_path = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_nop());

    uint32_t fast_done_idx = ctx->buf.count;
    {
        int32_t off =
            a64_patch_offset(ctx, bne_fast_done, fast_done_idx, true, "suspend fast result");
        ctx->buf.code[bne_fast_done] = a64_b_cond(A64_CC_NE, off);
    }
    if (rd != A64_XZR && rd != fast_result_reg)
        a64_buf_emit(&ctx->buf, a64_mov(rd, fast_result_reg));
    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t vi = XM_REF_INDEX(ins->dst);
        if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS) {
            int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
            a64_buf_emit(&ctx->buf,
                         a64_ldrb(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_CALL_RESULT_TAG_OFFSET));
            a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, tag_off));
        }
    }
    uint32_t b_done_fast = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_nop());

    uint32_t slow_path_idx = ctx->buf.count;
    {
        int32_t off =
            a64_patch_offset(ctx, beq_slow_path, slow_path_idx, true, "suspend slow result");
        ctx->buf.code[beq_slow_path] = a64_b_cond(A64_CC_EQ, off);
    }

    uint32_t smap_id = record_safepoint(ctx);

    a64_emit_load_jit_state(ctx, SCRATCH_REG);
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG, XM_JIT_STATE_SUSPEND_PTR_OFFSET));
    if (suspend_id >= XM_MAX_SUSPEND_ENTRIES) {
        ctx->had_error = true;
        return true;
    }

    a64_buf_emit(&ctx->buf, a64_stp(A64_X1, A64_X2, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X3, A64_X4, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 16));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X5, A64_X6, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 32));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X7, A64_X8, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 48));
    a64_buf_emit(&ctx->buf,
                 a64_stp(A64_X9, A64_X10, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 64));
    a64_buf_emit(&ctx->buf,
                 a64_stp(A64_X11, A64_X12, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 80));
    a64_buf_emit(&ctx->buf,
                 a64_stp(A64_X13, A64_X14, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 96));
    a64_buf_emit(&ctx->buf, a64_str(A64_X15, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 112));

    a64_buf_emit(&ctx->buf, a64_stp(A64_X20, A64_X21, SCRATCH_REG, XM_SUSPEND_CALLEE_SAVED_OFF));
    a64_buf_emit(&ctx->buf,
                 a64_stp(A64_X22, A64_X23, SCRATCH_REG, XM_SUSPEND_CALLEE_SAVED_OFF + 16));
    a64_buf_emit(&ctx->buf,
                 a64_stp(A64_X24, A64_X25, SCRATCH_REG, XM_SUSPEND_CALLEE_SAVED_OFF + 32));
    a64_buf_emit(&ctx->buf,
                 a64_stp(A64_X26, A64_X27, SCRATCH_REG, XM_SUSPEND_CALLEE_SAVED_OFF + 48));

    uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
    if (ns > XM_SUSPEND_SPILL_MAX)
        ns = XM_SUSPEND_SPILL_MAX;
    for (uint32_t s = 0; s < ns; s++) {
        int32_t frame_off = SPILL_BASE + (int32_t) s * 8;
        int32_t regs_off = XM_SUSPEND_SPILL_OFF + (int32_t) s * 8;
        a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, A64_SP, frame_off));
        a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, SCRATCH_REG, regs_off));
    }

    a64_emit_load_jit_state(ctx, SCRATCH_REG);
    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) suspend_id, 0));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, SCRATCH_REG, XM_JIT_STATE_SUSPEND_ID_OFFSET));
    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) smap_id, 0));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, SCRATCH_REG, XM_JIT_STATE_SUSPEND_SMAP_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));

    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_CALL_PROTO_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, SCRATCH_REG, XM_JIT_STATE_RESUME_PROTO_OFFSET));
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, SCRATCH_REG2, XM_PROTO_JIT_RESUME_ENTRY_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, SCRATCH_REG, XM_JIT_STATE_RESUME_ENTRY_OFFSET));

    void *block_helper = ctx->func->suspend_block_helpers[suspend_id];
    int64_t helper_extra_arg = 0;
    if (!block_helper) {
        block_helper = xm_helper_func(XM_HELPER_await_block);
        helper_extra_arg = discard_result;
    }
    a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
    a64_load_imm64(&ctx->buf, A64_X1, (uint64_t) helper_extra_arg);
    a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) block_helper);
    a64_buf_emit(&ctx->buf, a64_blr(SCRATCH_REG));

    a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) XM_DEOPT_MARKER);
    a64_buf_emit(&ctx->buf, a64_cmp(A64_X0, SCRATCH_REG));
    uint32_t bne_not_deopt = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_nop());
    a64_load_imm64(&ctx->buf, A64_X0, (uint64_t) XM_DEOPT_MARKER);
    a64_buf_emit(&ctx->buf, a64_movz(A64_X1, 0, 0));
    emit_epilogue(ctx);
    a64_buf_emit(&ctx->buf, a64_ret());
    {
        uint32_t here = ctx->buf.count;
        int32_t off = a64_patch_offset(ctx, bne_not_deopt, here, true, "suspend deopt skip");
        ctx->buf.code[bne_not_deopt] = a64_b_cond(A64_CC_NE, off);
    }

    uint32_t cbnz_not_blocked = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_nop());

    a64_load_imm64(&ctx->buf, A64_X0, (uint64_t) XM_SUSPEND_MARKER);
    a64_buf_emit(&ctx->buf, a64_movz(A64_X1, 0, 0));
    emit_epilogue(ctx);
    a64_buf_emit(&ctx->buf, a64_ret());

    {
        uint32_t here = ctx->buf.count;
        int32_t off = a64_patch_offset(ctx, cbnz_not_blocked, here, true, "suspend CBNZ");
        ctx->buf.code[cbnz_not_blocked] = a64_cbnz(A64_X0, off);
    }

    a64_emit_load_jit_state(ctx, SCRATCH_REG);
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG, XM_JIT_STATE_SUSPEND_PTR_OFFSET));

    a64_buf_emit(&ctx->buf, a64_ldp(A64_X1, A64_X2, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X3, A64_X4, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 16));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X5, A64_X6, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 32));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X7, A64_X8, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 48));
    a64_buf_emit(&ctx->buf,
                 a64_ldp(A64_X9, A64_X10, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 64));
    a64_buf_emit(&ctx->buf,
                 a64_ldp(A64_X11, A64_X12, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 80));
    a64_buf_emit(&ctx->buf,
                 a64_ldp(A64_X13, A64_X14, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 96));
    a64_buf_emit(&ctx->buf, a64_ldr(A64_X15, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 112));

    if (rd != A64_XZR)
        a64_buf_emit(&ctx->buf, a64_ldr(rd, SCRATCH_REG, XM_SUSPEND_RESULT_OFF));

    int32_t res_vreg_off = -1;
    int32_t res_bc_slot = -1;
    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t vi = XM_REF_INDEX(ins->dst);
        if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS)
            res_vreg_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
        if (vi < ctx->func->nvreg)
            res_bc_slot = ctx->func->vregs[vi].bc_slot;
    }
    if (res_vreg_off >= 0) {
        a64_buf_emit(&ctx->buf,
                     a64_ldrb(SCRATCH_REG2, SCRATCH_REG, (int32_t) XM_SUSPEND_RESULT_TAG_OFF));
        a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, res_vreg_off));
    }
    if (suspend_id < XM_MAX_SUSPEND_ENTRIES) {
        ctx->suspend_result_bc_slots[suspend_id] = res_bc_slot;
        ctx->suspend_result_tag_offs[suspend_id] = res_vreg_off;
        ctx->suspend_cont_offsets[suspend_id] = ctx->buf.count;
        ctx->suspend_smap_ids[suspend_id] = smap_id;
        ctx->suspend_result_regs[suspend_id] = (uint8_t) rd;
        if (suspend_id >= ctx->nsuspend)
            ctx->nsuspend = suspend_id + 1;
    }

    uint32_t done_fast_idx = ctx->buf.count;
    {
        int32_t off = a64_patch_offset(ctx, b_done_fast, done_fast_idx, false, "suspend done");
        ctx->buf.code[b_done_fast] = a64_b(off);
    }

    return true;
}

#endif  // __aarch64__
