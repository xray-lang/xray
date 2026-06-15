/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_riscv64_call.c - Call instruction codegen (RISC-V 64)
 *
 * Handles: CALL_C, CALL_C_LEAF, CALL_SELF_DIRECT,
 *          CALL_KNOWN, CALL_KNOWN_REG, CALL_DIRECT, CALL (generic).
 *
 * CALL_C uses the shared call_c_stub (saves/restores all registers).
 * CALL_C_LEAF saves only caller-saved registers inline.
 * Cross-function calls (SELF/KNOWN/DIRECT) use the C bridge helpers
 * xr_jit_call_func/xr_jit_call_self with full safepoint + deopt.
 */

#ifdef __riscv

#include "xm_codegen_riscv64_internal.h"
#include "xm_helper_table.h"
#include "xm_offsets.h"
#include "xm_jit_runtime.h"

XR_FUNC bool rv64_emit_call_ins(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    RV64_CODEGEN_CHECK(ctx, ctx != NULL, "rv64_emit_call_ins: NULL ctx");
    RV64_CODEGEN_CHECK(ctx, ins != NULL, "rv64_emit_call_ins: NULL ins");

    switch (ins->op == XM_CALL_METHOD_KNOWN ? XM_CALL_KNOWN : ins->op) {
        case XM_CALL_C: {
            RV64_CODEGEN_CHECK(ctx, xm_helper_call_c_protocol_matches_flags(ctx->func, ins),
                               "CALL_C post-call protocol flags mismatch");
            rv64_emit_call_args_from_pool(ctx, ins);
            rv64_emit_ptr_spill_writeback(ctx);

            /* Record safepoint + store safepoint_id to jit_ctx */
            uint32_t smap_id = rv64_record_safepoint(ctx);
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) smap_id);
            rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                             (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET));

            /* Store extra_arg to jit_ctx scratch slot */
            if (!xm_ref_is_none(ins->args[1])) {
                if (xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    uint64_t arg_val = (uint64_t) ctx->func->consts[ci].val.raw;
                    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, arg_val);
                } else if (xm_ref_is_vreg(ins->args[1]) &&
                           XM_REF_INDEX(ins->args[1]) < ctx->func->nvreg &&
                           ctx->func->vregs[XM_REF_INDEX(ins->args[1])].rep == XR_REP_F64) {
                    /* FP vreg: extract raw bits via FMV.X.D */
                    Rv64Freg fp = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT11);
                    rv64_buf_emit(&ctx->buf, rv64_fmv_x_d(RV64_SCRATCH_REG, fp));
                } else {
                    Rv64Reg arg_reg = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG);
                    if (arg_reg != RV64_SCRATCH_REG)
                        rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, arg_reg));
                }
                rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                                 (int32_t) RV64_EXTRA_ARG_OFFSET));
            } else {
                rv64_buf_emit(&ctx->buf,
                              rv64_sd(RV64_X0, RV64_JIT_CTX_REG, (int32_t) RV64_EXTRA_ARG_OFFSET));
            }

            /* Clear deopt_id before call so helper can request deopt */
            rv64_buf_emit(&ctx->buf,
                          rv64_sw(RV64_X0, RV64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET));

            /* Load C function pointer into t6 (SCRATCH_REG) */
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                uint64_t fn_ptr = (uint64_t) ctx->func->consts[ci].val.raw;
                rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, fn_ptr);
            } else {
                Rv64Reg fn_reg = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
                if (fn_reg != RV64_SCRATCH_REG)
                    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, fn_reg));
            }

            /* JAL ra, call_c_stub (patched later) */
            rv64_add_patch(ctx, RV64_PATCH_CALL_C, 0, RV64_CC_EQ, 0, 0);
            rv64_buf_emit(&ctx->buf, rv64_call(0)); /* placeholder */
            ctx->has_call_c = true;
            bool rv64_cc_emitted_deopt = false;
            if (xm_helper_call_c_needs_deopt_check(ctx->func, ins)) {
                rv64_buf_emit(&ctx->buf, rv64_lw(RV64_SCRATCH_REG2, RV64_JIT_CTX_REG,
                                                 (int32_t) XM_JIT_DEOPT_ID_OFFSET));
                rv64_emit_deopt_branch(ctx, RV64_SCRATCH_REG2);
                rv64_cc_emitted_deopt = true;
            }
            xm_post_call_record(&ctx->post_call_tracker, ctx->buf.count * 4, ctx->func, ins,
                                rv64_cc_emitted_deopt ? (XM_HPC_DEOPT | XM_HPC_SUSPEND) : 0);

            /* Move result payload (a0) to dst register */
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XM_REF_INDEX(ins->dst);
                bool dst_fp = (dvi < ctx->func->nvreg && ctx->func->vregs[dvi].rep == XR_REP_F64);
                if (dst_fp) {
                    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
                    rv64_buf_emit(&ctx->buf, rv64_fmv_d_x(fd, RV64_A0));
                } else if (rd != RV64_A0) {
                    rv64_buf_emit(&ctx->buf, rv64_mv(rd, RV64_A0));
                }
            }

            /* Store tag (from jit_ctx->call_result_tag) to vreg_runtime_tags[vi] */
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XM_REF_INDEX(ins->dst);
                if (dvi < ctx->func->nvreg && dvi < XR_JIT_MAX_VREG_TAGS) {
                    int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) dvi;
                    rv64_buf_emit(&ctx->buf, rv64_lbu(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                                      (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET));
                    rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, tag_off));
                }
            }
            break;
        }

        case XM_CALL_C_LEAF: {
            rv64_emit_call_args_from_pool(ctx, ins);

            /* Resolve extra arg */
            uint64_t extra_val = 0;
            bool extra_is_const = false;
            Rv64Reg extra_reg = RV64_SCRATCH_REG;
            if (!xm_ref_is_none(ins->args[1])) {
                if (xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    extra_val = (uint64_t) ctx->func->consts[ci].val.raw;
                    extra_is_const = true;
                } else {
                    extra_reg = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG);
                }
            }

            /* Load function pointer */
            uint64_t fn_ptr = 0;
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                fn_ptr = (uint64_t) ctx->func->consts[ci].val.raw;
            }

            /* Save all caller-saved GP regs to stack.
             * RISC-V: 12 caller-saved alloc regs (a0-a7 + t0-t3).
             * 12 * 8 = 96, next 16-aligned = 96. */
            int nsave = RV64_NGPR_CALLER_SAVE;
            int32_t leaf_frame = ((nsave * 8 + 15) & ~15);
            rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, -leaf_frame));
            for (int i = 0; i < nsave && i < RV64_MAX_PHYS_REGS; i++)
                rv64_buf_emit(&ctx->buf, rv64_sd(rv64_alloc_regs[i], RV64_SP, i * 8));

            /* Set up LP64D ABI: a0=coro, a1=extra_arg */
            rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A0, RV64_CORO_REG));
            if (!xm_ref_is_none(ins->args[1])) {
                if (extra_is_const)
                    rv64_load_imm64(&ctx->buf, RV64_A1, extra_val);
                else
                    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A1, extra_reg));
            }

            /* Load function and call */
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, fn_ptr);
            rv64_buf_emit(&ctx->buf, rv64_jalr(RV64_RA, RV64_SCRATCH_REG, 0));

            /* Save result payload (a0) to t6 */
            rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, RV64_A0));

            /* Restore caller-saved GP regs */
            for (int i = 0; i < nsave && i < RV64_MAX_PHYS_REGS; i++)
                rv64_buf_emit(&ctx->buf, rv64_ld(rv64_alloc_regs[i], RV64_SP, i * 8));
            rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, leaf_frame));

            /* Move result to dst */
            if (xm_ref_is_vreg(ins->dst)) {
                if (rd != RV64_SCRATCH_REG)
                    rv64_buf_emit(&ctx->buf, rv64_mv(rd, RV64_SCRATCH_REG));
            }
            break;
        }

        case XM_CALL_SELF_DIRECT: {
            rv64_emit_call_args_from_pool(ctx, ins);
            rv64_emit_ptr_spill_writeback(ctx);

            uint32_t smap_id = rv64_record_safepoint(ctx);
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) smap_id);
            rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                             (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET));

            /* Collect live caller-saved regs */
            Rv64Reg live_gp[RV64_NGPR_CALLER_SAVE];
            int ngp = rv64_live_gp(ctx, live_gp, rd);
            Rv64Freg live_fp_arr[20];
            int nfp = rv64_live_fp(ctx, live_fp_arr);

            /* Save caller-saved to stack (16-aligned) */
            int total_saves = ngp + nfp;
            int32_t save_frame = ((total_saves * 8 + 15) & ~15);
            if (save_frame > 0) {
                rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, -save_frame));
                int off = 0;
                for (int i = 0; i < ngp; i++) {
                    rv64_buf_emit(&ctx->buf, rv64_sd(live_gp[i], RV64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    rv64_buf_emit(&ctx->buf, rv64_fsd(live_fp_arr[f], RV64_SP, off));
                    off += 8;
                }
            }

            /* Memory passing: a0=coro, a1=&call_args */
            rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A0, RV64_CORO_REG));
            rv64_buf_emit(&ctx->buf,
                          rv64_addi(RV64_A1, RV64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_ARGS_OFFSET));

            /* JAL ra, entry (patched to self fast_entry or normal entry) */
            rv64_add_patch(ctx, RV64_PATCH_CALL_SELF, 0, RV64_CC_EQ, 0, 0);
            rv64_buf_emit(&ctx->buf, rv64_call(0)); /* placeholder */

            /* Restore caller's active smap after JIT→JIT return */
            rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_FP,
                                             -(int32_t) RV64_FRAME_SMAP_PTR_OFFSET));
            rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                             (int32_t) XM_JIT_ACTIVE_SMAP_OFFSET));
            rv64_buf_emit(&ctx->buf,
                          rv64_sd(RV64_FP, RV64_JIT_CTX_REG, (int32_t) XM_JIT_FRAME_SP_OFFSET));

            /* Save return value (a0) to t6 */
            rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, RV64_A0));

            /* Store return tag (a1) to vreg_runtime_tags[vi] */
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS) {
                    int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
                    rv64_buf_emit(&ctx->buf, rv64_sb(RV64_A1, RV64_JIT_CTX_REG, tag_off));
                }
            }

            /* Deopt propagation: if result == DEOPT_MARKER, propagate */
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) XM_DEOPT_MARKER);
            /* BNE t6, t5, skip_deopt (forward branch, 2 instructions for deopt path) */
            rv64_buf_emit(&ctx->buf, rv64_bne(RV64_SCRATCH_REG, RV64_SCRATCH_REG2, 4 * 5));

            /* Deopt path: clean up save frame + store propagation marker + jump to deopt */
            if (save_frame > 0)
                rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, save_frame));
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, 0xFFFF);
            rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG2, RV64_JIT_CTX_REG,
                                             (int32_t) XM_JIT_DEOPT_ID_OFFSET));
            rv64_emit_deopt_jmp(ctx);

            /* skip_deopt: Restore caller-saved regs */
            if (save_frame > 0) {
                int off = 0;
                for (int i = 0; i < ngp; i++) {
                    rv64_buf_emit(&ctx->buf, rv64_ld(live_gp[i], RV64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    rv64_buf_emit(&ctx->buf, rv64_fld(live_fp_arr[f], RV64_SP, off));
                    off += 8;
                }
                rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, save_frame));
            }

            /* Move result to dst */
            if (xm_ref_is_vreg(ins->dst) && rd != RV64_SCRATCH_REG)
                rv64_buf_emit(&ctx->buf, rv64_mv(rd, RV64_SCRATCH_REG));
            break;
        }

        /* CALL_KNOWN, CALL_KNOWN_REG, CALL_METHOD_KNOWN, CALL_DIRECT, CALL:
         * All of these route through C bridge helpers.
         * Bridge stores/restores state via jit_ctx, handles JIT→interp
         * fallback, type feedback, and deopt. For now we implement them
         * uniformly via the call_c_stub trampoline, using the appropriate
         * helper function pointer. */
        case XM_CALL_KNOWN:
        case XM_CALL_KNOWN_REG:
        case XM_CALL_DIRECT:
        case XM_CALL: {
            rv64_emit_call_args_from_pool(ctx, ins);
            rv64_emit_ptr_spill_writeback(ctx);

            uint32_t smap_id = rv64_record_safepoint(ctx);
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) smap_id);
            rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                             (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET));

            /* Select the correct C bridge helper */
            void *helper_fn = NULL;
            uint64_t extra_val = 0;
            if (ins->op == XM_CALL_KNOWN || ins->op == XM_CALL_KNOWN_REG ||
                ins->op == XM_CALL_METHOD_KNOWN) {
                helper_fn =
                    xm_helper_func(ins->op == XM_CALL_METHOD_KNOWN ? XM_HELPER_call_method_known
                                                                   : XM_HELPER_call_func);
                /* Store callee proto to jit_ctx */
                if (xm_ref_is_const(ins->args[0])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[0]);
                    uint64_t proto_ptr = (uint64_t) ctx->func->consts[ci].val.raw;
                    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, proto_ptr);
                    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                                     (int32_t) XM_JIT_CALL_PROTO_OFFSET));
                }
                if (ins->op == XM_CALL_METHOD_KNOWN && xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    uint64_t closure_ptr = (uint64_t) ctx->func->consts[ci].val.raw;
                    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, closure_ptr);
                    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                                     (int32_t) XM_JIT_CALL_CLOSURE_OFFSET));
                }
                /* Extra arg: nargs encoded */
                if (ins->op == XM_CALL_METHOD_KNOWN) {
                    if (xm_ref_is_vreg(ins->dst)) {
                        uint32_t dvi = XM_REF_INDEX(ins->dst);
                        if (dvi < ctx->func->nvreg)
                            extra_val = ctx->func->vregs[dvi].call_nargs;
                    }
                } else if (!xm_ref_is_none(ins->args[1]) && xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    extra_val = (uint64_t) ctx->func->consts[ci].val.raw;
                }
            } else if (ins->op == XM_CALL_DIRECT) {
                helper_fn = xm_helper_func(XM_HELPER_invoke_direct);
                if (!xm_ref_is_none(ins->args[1]) && xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    extra_val = (uint64_t) ctx->func->consts[ci].val.raw;
                }
            } else {
                helper_fn = xm_helper_func(XM_HELPER_call_func);
                if (!xm_ref_is_none(ins->args[1]) && xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    extra_val = (uint64_t) ctx->func->consts[ci].val.raw;
                }
            }

            /* Store extra_arg */
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, extra_val);
            rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                             (int32_t) RV64_EXTRA_ARG_OFFSET));

            /* Clear deopt_id */
            rv64_buf_emit(&ctx->buf,
                          rv64_sw(RV64_X0, RV64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET));

            /* Load helper function pointer into t6 */
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) (uintptr_t) helper_fn);

            /* JAL ra, call_c_stub */
            rv64_add_patch(ctx, RV64_PATCH_CALL_C, 0, RV64_CC_EQ, 0, 0);
            rv64_buf_emit(&ctx->buf, rv64_call(0));
            ctx->has_call_c = true;

            /* Check deopt */
            rv64_buf_emit(&ctx->buf, rv64_lw(RV64_SCRATCH_REG2, RV64_JIT_CTX_REG,
                                             (int32_t) XM_JIT_DEOPT_ID_OFFSET));
            rv64_emit_deopt_branch(ctx, RV64_SCRATCH_REG2);

            /* Move result to dst */
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XM_REF_INDEX(ins->dst);
                bool dst_fp = (dvi < ctx->func->nvreg && ctx->func->vregs[dvi].rep == XR_REP_F64);
                if (dst_fp) {
                    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
                    rv64_buf_emit(&ctx->buf, rv64_fmv_d_x(fd, RV64_A0));
                } else if (rd != RV64_A0) {
                    rv64_buf_emit(&ctx->buf, rv64_mv(rd, RV64_A0));
                }
            }

            /* Store tag to vreg_runtime_tags[vi] */
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XM_REF_INDEX(ins->dst);
                if (dvi < ctx->func->nvreg && dvi < XR_JIT_MAX_VREG_TAGS) {
                    int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) dvi;
                    rv64_buf_emit(&ctx->buf, rv64_lbu(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                                      (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET));
                    rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, tag_off));
                }
            }
            break;
        }

        default:
            return false;
    }
    return true;
}

/* ========== Call Args Helper ========== */

/* Store call arguments from the pool to jit_ctx->call_args[] and
 * compile-time type tags to jit_ctx->call_arg_tags[]. */
XR_FUNC void rv64_emit_call_args_from_pool(Rv64CodegenCtx *ctx, XmIns *ins) {
    RV64_CODEGEN_CHECK(ctx, ctx != NULL && ins != NULL, "call_args: NULL");
    uint16_t call_nargs = 0;
    XmRef *pool = NULL;
    uint32_t start = 0;

    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t vi = XM_REF_INDEX(ins->dst);
        if (vi < ctx->func->nvreg) {
            XmVReg *vreg = &ctx->func->vregs[vi];
            call_nargs = vreg->call_nargs;
            pool = ctx->func->call_arg_pool;
            start = vreg->call_arg_start;
        }
    }

    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, call_nargs);
    rv64_buf_emit(&ctx->buf,
                  rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_NARGS_OFFSET));
    if (call_nargs == 0 || !pool)
        return;

    for (uint16_t i = 0; i < call_nargs; i++) {
        XmRef arg = pool[start + i];
        int32_t off = (int32_t) (XM_JIT_CALL_ARGS_OFFSET + i * 8);
        int32_t tag_off = (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET + (int32_t) i;
        uint8_t tag = XR_RTAG_UNKNOWN;
        if (xm_ref_is_none(arg))
            goto write_static_tag;
        if (xm_ref_is_const(arg)) {
            uint32_t ci = XM_REF_INDEX(arg);
            uint64_t val = (uint64_t) ctx->func->consts[ci].val.raw;
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, val);
            rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, off));
            tag = rv64_const_rep_to_value_tag(ctx->func->consts[ci].rep);
        } else {
            Rv64Reg reg = rv64_get_operand(ctx, arg, RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_sd(reg, RV64_JIT_CTX_REG, off));
            XmType ct = xm_ref_ctype(ctx->func, arg);
            tag = vtag_to_value_tag(type_kind_to_vtag(ct.kind));
        }
        if (tag == XR_RTAG_UNKNOWN && xm_ref_is_vreg(arg)) {
            uint32_t ai = XM_REF_INDEX(arg);
            if (ai < ctx->func->nvreg && ai < XR_JIT_MAX_VREG_TAGS) {
                int32_t src_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) ai;
                rv64_buf_emit(&ctx->buf, rv64_lbu(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, src_off));
                rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, tag_off));
                continue;
            }
        }
    write_static_tag:
        rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, tag);
        rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, tag_off));
    }
}

#endif /* __riscv */
