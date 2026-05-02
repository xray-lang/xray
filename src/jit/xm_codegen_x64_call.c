/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_x64_call.c - Xm codegen for call instructions (x86-64)
 *
 * Split from xm_codegen_x64.c to stay within the 3000-line limit.
 * Handles: CALL_C, CALL_C_LEAF, CALL_SELF_DIRECT,
 *          CALL_KNOWN, CALL_KNOWN_REG, CALL_DIRECT, CALL (generic).
 */

#if defined(__x86_64__) || defined(_M_X64)

#include "xm_codegen_x64_internal.h"
#include "xm_offsets.h"
#include "xm_jit_runtime.h"
#include <string.h>

bool x64_emit_call_ins(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    CODEGEN_CHECK(ctx, ctx != NULL, "x64_emit_call_ins: NULL ctx");
    CODEGEN_CHECK(ctx, ins != NULL, "x64_emit_call_ins: NULL ins");

    switch (ins->op) {
        case XM_CALL_C: {
            x64_emit_call_args_from_pool(ctx, ins);
            x64_emit_ptr_spill_writeback(ctx);

            /* Record safepoint + store safepoint_id to jit_ctx */
            uint32_t smap_id_cc = x64_record_safepoint(ctx);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_cc);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_SCRATCH_REG);

            /* Store extra_arg to jit_ctx scratch slot */
            if (!xm_ref_is_none(ins->args[1])) {
                if (xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    uint64_t arg_val = (uint64_t) ctx->func->consts[ci].val.raw;
                    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, arg_val);
                } else if (xm_ref_is_vreg(ins->args[1]) &&
                           XM_REF_INDEX(ins->args[1]) < ctx->func->nvreg &&
                           ctx->func->vregs[XM_REF_INDEX(ins->args[1])].rep == XR_REP_F64) {
                    /* FP vreg: extract raw bits via MOVQ xmm→gp */
                    X64Xmm fp = x64_get_fp_operand(ctx, ins->args[1], (X64Xmm) X64_SCRATCH_XMM);
                    x64_movq_gp_xmm(&ctx->buf, X64_SCRATCH_REG, fp);
                } else {
                    X64Reg arg_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                    if (arg_reg != X64_SCRATCH_REG)
                        x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, arg_reg);
                }
                x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET,
                           X64_SCRATCH_REG);
            } else {
                /* No extra arg — store 0 */
                x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
                x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET,
                           X64_SCRATCH_REG);
            }

            /* Clear deopt_id before call so helper can request deopt */
            x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET,
                         X64_SCRATCH_REG);

            /* Load C function pointer into R11 (scratch) */
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                uint64_t fn_ptr = (uint64_t) ctx->func->consts[ci].val.raw;
                x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, fn_ptr);
            } else {
                X64Reg fn_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                if (fn_reg != X64_SCRATCH_REG)
                    x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, fn_reg);
            }

            /* CALL call_c_stub (patched later as rel32) */
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *p = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8); /* CALL rel32 */
            p->emit_pos = ctx->buf.pos;
            p->target_blk = 0; /* unused; patched to call_c_stub */
            p->type = X64_PATCH_CALL_C;
            p->cc = X64_CC_E; /* unused */
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0); /* placeholder rel32 */
            ctx->has_call_c = true;

            /* Check if C helper requested deopt (e.g. yieldable cfunc).
             * If jit_ctx->deopt_id != 0, jump to deopt stub. */
            x64_mov_rm32(&ctx->buf, X64_RCX, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET);
            x64_test_rr(&ctx->buf, X64_RCX, X64_RCX);
            x64_emit_deopt_jcc(ctx, X64_CC_NE);

            /* Move result payload (RAX) to dst register */
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XM_REF_INDEX(ins->dst);
                bool dst_fp = (dvi < ctx->func->nvreg && ctx->func->vregs[dvi].rep == XR_REP_F64);
                if (dst_fp) {
                    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
                    x64_movq_xmm_gp(&ctx->buf, fd, X64_RAX);
                } else if (rd != X64_RAX) {
                    x64_mov_rr(&ctx->buf, rd, X64_RAX);
                }
            }

            /* Store tag (from jit_ctx->call_result_tag) to slot_runtime_tags */
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XM_REF_INDEX(ins->dst);
                if (dvi < ctx->func->nvreg) {
                    int16_t bc_slot = ctx->func->vregs[dvi].bc_slot;
                    if (bc_slot >= 0 && bc_slot < 256) {
                        int32_t tag_off = (int32_t) XM_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot;
                        x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG,
                                      (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET);
                        x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, tag_off, X64_SCRATCH_REG);
                    }
                }
            }
            break;
        }

        case XM_CALL_C_LEAF: {
            x64_emit_call_args_from_pool(ctx, ins);

            /* Resolve extra arg value */
            uint64_t extra_arg_val = 0;
            bool extra_is_const = false;
            X64Reg extra_reg = X64_SCRATCH_REG;
            if (!xm_ref_is_none(ins->args[1])) {
                if (xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    extra_arg_val = (uint64_t) ctx->func->consts[ci].val.raw;
                    extra_is_const = true;
                } else {
                    extra_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                }
            }

            /* Load function pointer */
            uint64_t fn_ptr_val = 0;
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                fn_ptr_val = (uint64_t) ctx->func->consts[ci].val.raw;
            }

            /* Save all caller-saved GP regs.
             * SysV:  first 8 alloc regs  (rax,rcx,rdx,rsi,rdi,r8,r9,r10)
             * Win64: first 6 alloc regs  (rax,rcx,rdx,r8,r9,r10) */
            int nsave_gp = X64_NGPR_CALLER_SAVE;
            int pad_gp = 0;
            if (nsave_gp % 2 == 0) {
                /* Push a dummy for 16-byte alignment before inner CALL */
                x64_push_r(&ctx->buf, X64_SCRATCH_REG);
                pad_gp = 1;
            }
            for (int i = 0; i < nsave_gp && i < X64_MAX_PHYS_REGS; i++)
                x64_push_r(&ctx->buf, x64_alloc_regs[i]);

#ifdef _WIN32
            /* Win64: XrJitResult (16B) via hidden pointer.
             * Allocate 16 (return buf) + 32 (shadow) = 48 bytes. */
            x64_sub_ri(&ctx->buf, X64_RSP, 48);
            x64_lea(&ctx->buf, X64_RCX, X64_RSP, 32); /* hidden return ptr */
            x64_mov_rr(&ctx->buf, X64_RDX, X64_CORO_REG);
            if (!xm_ref_is_none(ins->args[1])) {
                if (extra_is_const)
                    x64_load_imm64(&ctx->buf, X64_R8, extra_arg_val);
                else
                    x64_mov_rr(&ctx->buf, X64_R8, extra_reg);
            }
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, fn_ptr_val);
            x64_call_r(&ctx->buf, X64_SCRATCH_REG);
            /* Read payload from return buffer */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_RSP, 32);
            x64_add_ri(&ctx->buf, X64_RSP, 48);
#else
            /* System V: RDI=coro, RSI=extra_arg */
            x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
            if (!xm_ref_is_none(ins->args[1])) {
                if (extra_is_const)
                    x64_load_imm64(&ctx->buf, X64_RSI, extra_arg_val);
                else
                    x64_mov_rr(&ctx->buf, X64_RSI, extra_reg);
            }
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, fn_ptr_val);
            x64_call_r(&ctx->buf, X64_SCRATCH_REG);
            /* Save result to R11 */
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_RAX);
#endif

            /* Restore caller-saved GP regs (reverse) */
            for (int i = nsave_gp - 1; i >= 0 && i < X64_MAX_PHYS_REGS; i--)
                x64_pop_r(&ctx->buf, x64_alloc_regs[i]);
            if (pad_gp)
                x64_pop_r(&ctx->buf, X64_SCRATCH_REG); /* pop dummy */

            /* Move result to dst */
            if (xm_ref_is_vreg(ins->dst)) {
                if (rd != X64_SCRATCH_REG)
                    x64_mov_rr(&ctx->buf, rd, X64_SCRATCH_REG);
            }
            break;
        }

        /* ========== Cross-function calls ========== */
        case XM_CALL_SELF_DIRECT: {
            x64_emit_call_args_from_pool(ctx, ins);
            x64_emit_ptr_spill_writeback(ctx);

            /* Record safepoint + store safepoint_id to jit_ctx */
            uint32_t smap_id_self = x64_record_safepoint(ctx);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_self);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_SCRATCH_REG);

            bool reg_passing = !xm_ref_is_none(ins->args[0]);

            /* Resolve arg registers before save */
            X64Reg arg_regs[2] = {X64_SCRATCH_REG, X64_SCRATCH_REG};
            int nargs_reg = 0;
            if (reg_passing) {
                X64Reg scratches[2] = {X64_SCRATCH_REG, X64_RCX};
                for (int a = 0; a < 2; a++) {
                    if (xm_ref_is_none(ins->args[a]))
                        break;
                    arg_regs[a] = x64_get_operand(ctx, ins->args[a], scratches[a]);
                    nargs_reg++;
                }
            }

            /* Collect live caller-saved regs */
            X64Reg live_gp[8];
            int ngp = x64_live_gp(ctx, live_gp, rd);
            X64Xmm live_fp_arr[14];
            int nfp = x64_live_fp(ctx, live_fp_arr);

            /* Save caller-saved regs to stack (16-byte aligned) */
            int total_saves = ngp + nfp;
            int32_t save_frame = ((total_saves * 8 + 15) & ~15);
            if (save_frame > 0) {
                x64_sub_ri(&ctx->buf, X64_RSP, save_frame);
                int off = 0;
                for (int i = 0; i < ngp; i++) {
                    x64_mov_mr(&ctx->buf, X64_RSP, off, live_gp[i]);
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    x64_movsd_mr(&ctx->buf, X64_RSP, off, live_fp_arr[f]);
                    off += 8;
                }
            }

            /* Setup args and CALL self */
            if (reg_passing) {
                /* Move args to fixed ABI registers (alloc_regs[0..N]). */
                X64Reg p0 = x64_alloc_regs[0];
                if (nargs_reg >= 1 && arg_regs[0] != p0)
                    x64_mov_rr(&ctx->buf, p0, arg_regs[0]);
                if (nargs_reg >= 2) {
                    X64Reg p1 = x64_alloc_regs[1];
                    if (arg_regs[1] != p1)
                        x64_mov_rr(&ctx->buf, p1, arg_regs[1]);
                }
                /* Load extra args from call_args[] to alloc_regs[] */
                int extra = XM_FLAG_EXTRA_ARGS_GET(ins->flags);
                for (int ei = 0; ei < extra; ei++) {
                    X64Reg dst = x64_alloc_regs[2 + ei];
                    int32_t off = (int32_t) (XM_JIT_CALL_ARGS_OFFSET + (2 + ei) * 8);
                    x64_mov_rm(&ctx->buf, dst, X64_JIT_CTX_REG, off);
                }
                /* ABI_ARG1 = coro (platform calling convention) */
                x64_mov_rr(&ctx->buf, X64_ABI_ARG1, X64_CORO_REG);
                /* CALL to fast_entry (after param loading) */
                CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
                X64BranchPatch *p = &ctx->patches[ctx->npatch];
                x64_emit8(&ctx->buf, 0xE8);
                p->emit_pos = ctx->buf.pos;
                p->target_blk = 0;
                p->type = X64_PATCH_CALL_SELF_FAST;
                p->cc = X64_CC_E;
                ctx->npatch++;
                x64_emit32(&ctx->buf, 0);
            } else {
                /* Memory passing: ABI_ARG1=coro, ABI_ARG2=&call_args */
                x64_mov_rr(&ctx->buf, X64_ABI_ARG1, X64_CORO_REG);
                x64_lea(&ctx->buf, X64_ABI_ARG2, X64_JIT_CTX_REG,
                        (int32_t) XM_JIT_CALL_ARGS_OFFSET);
                /* CALL to entry (offset 0) */
                CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
                X64BranchPatch *p = &ctx->patches[ctx->npatch];
                x64_emit8(&ctx->buf, 0xE8);
                p->emit_pos = ctx->buf.pos;
                p->target_blk = 0;
                p->type = X64_PATCH_CALL_SELF;
                p->cc = X64_CC_E;
                ctx->npatch++;
                x64_emit32(&ctx->buf, 0);
            }

            /* Restore active stack map in jit_ctx */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_RBP,
                       -(int32_t) X64_JIT_FRAME_BASE); /* TODO: frame smap ptr slot */
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_FRAME_SP_OFFSET, X64_RBP);

            /* Save return value (RAX) to R11 before deopt check */
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_RAX);

            /* Store return tag (RCX = callee tag) to slot_runtime_tags if needed */
            int16_t self_bc_slot = -1;
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg)
                    self_bc_slot = ctx->func->vregs[vi].bc_slot;
            }
            if (self_bc_slot >= 0 && self_bc_slot < 256) {
                int32_t stag_off = (int32_t) XM_JIT_SLOT_RUNTIME_TAGS_OFFSET + self_bc_slot;
                x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, stag_off, X64_RCX);
            }

            /* Deopt propagation check: if result == DEOPT_MARKER, propagate */
            x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t) XM_DEOPT_MARKER);
            x64_cmp_rr(&ctx->buf, X64_SCRATCH_REG, X64_RCX);
            /* JNE skip_deopt */
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_NE));
            uint32_t jne_pos = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0); /* placeholder */

            /* Deopt path: clean up save frame and jump to deopt_stub */
            if (save_frame > 0)
                x64_add_ri(&ctx->buf, X64_RSP, save_frame);
            /* Store max deopt_id to indicate propagation */
            x64_load_imm64(&ctx->buf, X64_RCX, 0xFFFF);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET, X64_RCX);
            /* JMP to deopt_stub */
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *dp = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE9);
            dp->emit_pos = ctx->buf.pos;
            dp->target_blk = 0;
            dp->type = X64_PATCH_DEOPT_JMP;
            dp->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_deopt = true;

            /* Skip target for JNE (not deopt) */
            x64_patch_rel32(&ctx->buf, jne_pos, ctx->buf.pos);

            /* Restore caller-saved regs */
            if (save_frame > 0) {
                int off = 0;
                for (int i = 0; i < ngp; i++) {
                    x64_mov_rm(&ctx->buf, live_gp[i], X64_RSP, off);
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    x64_movsd_rm(&ctx->buf, live_fp_arr[f], X64_RSP, off);
                    off += 8;
                }
                x64_add_ri(&ctx->buf, X64_RSP, save_frame);
            }

            /* Move result to dst */
            if (xm_ref_is_vreg(ins->dst) && rd != X64_SCRATCH_REG)
                x64_mov_rr(&ctx->buf, rd, X64_SCRATCH_REG);
            break;
        }

        /* CALL_KNOWN: cross-function direct CALL with known callee proto.
         * args[0] = const_ptr(callee XrProto*), args[1] = const(nargs). */
        case XM_CALL_KNOWN: {
            x64_emit_call_args_from_pool(ctx, ins);
            x64_emit_ptr_spill_writeback(ctx);

            /* Record safepoint + store safepoint_id to jit_ctx */
            uint32_t smap_id_k = x64_record_safepoint(ctx);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_k);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_SCRATCH_REG);

            /* Extract callee proto pointer and nargs from consts */
            uint64_t callee_proto_ptr = 0;
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                callee_proto_ptr = (uint64_t) ctx->func->consts[ci].val.raw;
            }
            uint64_t nargs_val = 0;
            if (!xm_ref_is_none(ins->args[1]) && xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                nargs_val = (uint64_t) ctx->func->consts[ci].val.i64;
            }

            /* Save live caller-saved regs to stack (16-byte aligned) */
            X64Reg live_gp[8];
            int ngp = x64_live_gp(ctx, live_gp, rd);
            X64Xmm live_fp_arr[14];
            int nfp = x64_live_fp(ctx, live_fp_arr);
            int32_t save_frame = (((ngp + nfp) * 8 + 15) & ~15);
            if (save_frame > 0) {
                x64_sub_ri(&ctx->buf, X64_RSP, save_frame);
                int off = 0;
                for (int i = 0; i < ngp; i++) {
                    x64_mov_mr(&ctx->buf, X64_RSP, off, live_gp[i]);
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    x64_movsd_mr(&ctx->buf, X64_RSP, off, live_fp_arr[f]);
                    off += 8;
                }
            }

            /* Fast path: load proto → jit_ctx->call_proto, then jit_entry */
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, callee_proto_ptr);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_PROTO_OFFSET,
                       X64_SCRATCH_REG);
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
                       (int32_t) XM_PROTO_JIT_ENTRY_OFFSET);
            /* TEST r11, r11; JE slow_path (placeholder) */
            x64_test_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_E));
            uint32_t je_slow_pos = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* Set call_closure from call_args[0] so callee can access upvalues */
            x64_mov_rm(&ctx->buf, X64_RCX, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_ARGS_OFFSET);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_CLOSURE_OFFSET, X64_RCX);

            /* Fast path direct CALL: ABI_ARG1=coro, ABI_ARG2=&call_args[1] */
            x64_mov_rr(&ctx->buf, X64_ABI_ARG1, X64_CORO_REG);
            x64_lea(&ctx->buf, X64_ABI_ARG2, X64_JIT_CTX_REG,
                    (int32_t) (XM_JIT_CALL_ARGS_OFFSET + 8));
            x64_call_r(&ctx->buf, X64_SCRATCH_REG);

            /* Store RCX (callee tag from epilogue) to jit_ctx->call_result_tag */
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET,
                       X64_RCX);

            /* Nested deopt guard: if RAX == DEOPT_MARKER, cascade to slow path */
            x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t) XM_DEOPT_MARKER);
            x64_cmp_rr(&ctx->buf, X64_RAX, X64_RCX);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_E));
            uint32_t je_cascade_pos = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* JMP done (over slow path) */
            x64_emit8(&ctx->buf, 0xE9);
            uint32_t jmp_done_pos = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* Cascade target: clear stale deopt_id before C bridge retry */
            uint32_t cascade_pos = ctx->buf.pos;
            x64_xor_rr(&ctx->buf, X64_RCX, X64_RCX);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET, X64_RCX);

            /* Slow path: CALL_C to xr_jit_call_func(coro, nargs) */
            uint32_t slow_pos = ctx->buf.pos;
            /* Patch je_slow → slow_pos */
            x64_patch_rel32(&ctx->buf, je_slow_pos, slow_pos);
            /* Patch je_cascade → cascade_pos */
            x64_patch_rel32(&ctx->buf, je_cascade_pos, cascade_pos);

            /* Store extra_arg (nargs) via RCX; call_c_stub expects fn ptr in R11 */
            x64_load_imm64(&ctx->buf, X64_RCX, nargs_val);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET, X64_RCX);
            /* Load helper function pointer to R11 (call_c_stub calls R11) */
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_call_func);
            /* CALL rel32 → call_c_stub */
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *cp = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            cp->emit_pos = ctx->buf.pos;
            cp->target_blk = 0;
            cp->type = X64_PATCH_CALL_C;
            cp->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_call_c = true;

            /* Done label */
            uint32_t done_pos = ctx->buf.pos;
            x64_patch_rel32(&ctx->buf, jmp_done_pos, done_pos);

            /* Load call_result_tag → slot_runtime_tags[bc_slot] */
            int16_t bc_slot_k = -1;
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg)
                    bc_slot_k = ctx->func->vregs[vi].bc_slot;
            }
            if (bc_slot_k >= 0 && bc_slot_k < 256) {
                x64_movzx_rm8(&ctx->buf, X64_RCX, X64_JIT_CTX_REG,
                              (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET);
                int32_t tag_off = (int32_t) XM_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot_k;
                x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, tag_off, X64_RCX);
            }

            /* Save RAX to R11 before restoring caller-saved regs */
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_RAX);

            /* Restore caller-saved regs */
            if (save_frame > 0) {
                int off = 0;
                for (int i = 0; i < ngp; i++) {
                    x64_mov_rm(&ctx->buf, live_gp[i], X64_RSP, off);
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    x64_movsd_rm(&ctx->buf, live_fp_arr[f], X64_RSP, off);
                    off += 8;
                }
                x64_add_ri(&ctx->buf, X64_RSP, save_frame);
            }

            /* Move result (R11) → dst */
            if (xm_ref_is_vreg(ins->dst) && rd != X64_SCRATCH_REG)
                x64_mov_rr(&ctx->buf, rd, X64_SCRATCH_REG);
            break;
        }

        /* CALL_KNOWN_REG: register-passing variant of CALL_KNOWN (nargs <= 2).
         * args[0] = param0 Xm ref, args[1] = param1 Xm ref (or NONE).
         * Builder pre-stored callee proto in jit_ctx->call_proto. */
        case XM_CALL_KNOWN_REG: {
            x64_emit_call_args_from_pool(ctx, ins);
            x64_emit_ptr_spill_writeback(ctx);

            /* Record safepoint */
            uint32_t smap_id_kr = x64_record_safepoint(ctx);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_kr);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_SCRATCH_REG);

            /* Resolve arg vreg → physical reg mapping (before spill save) */
            int nargs_reg = 0;
            X64Reg arg_regs[2] = {X64_SCRATCH_REG, X64_SCRATCH_REG};
            {
                X64Reg scratches[2] = {X64_SCRATCH_REG, X64_RCX};
                for (int a = 0; a < 2; a++) {
                    if (xm_ref_is_none(ins->args[a]))
                        break;
                    arg_regs[a] = x64_get_operand(ctx, ins->args[a], scratches[a]);
                    nargs_reg++;
                }
            }

            /* Save live caller-saved regs */
            X64Reg live_gp[8];
            int ngp = x64_live_gp(ctx, live_gp, rd);
            X64Xmm live_fp_arr[14];
            int nfp = x64_live_fp(ctx, live_fp_arr);
            int32_t save_frame = (((ngp + nfp) * 8 + 15) & ~15);
            if (save_frame > 0) {
                x64_sub_ri(&ctx->buf, X64_RSP, save_frame);
                int off = 0;
                for (int i = 0; i < ngp; i++) {
                    x64_mov_mr(&ctx->buf, X64_RSP, off, live_gp[i]);
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    x64_movsd_mr(&ctx->buf, X64_RSP, off, live_fp_arr[f]);
                    off += 8;
                }
            }

            /* Load callee proto->jit_fast_entry to R11 */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG,
                       (int32_t) XM_JIT_CALL_PROTO_OFFSET);
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
                       (int32_t) XM_PROTO_JIT_FAST_ENTRY_OFFSET);
            /* TEST R11, R11; JE slow_path */
            x64_test_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_E));
            uint32_t je_slow_kr = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* Set call_closure from call_args[0] */
            x64_mov_rm(&ctx->buf, X64_RCX, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_ARGS_OFFSET);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_CLOSURE_OFFSET, X64_RCX);

            /* Move args to alloc_regs[0..nargs-1], handling collisions */
            if (nargs_reg == 1) {
                X64Reg p0 = x64_alloc_regs[0];
                if (arg_regs[0] != p0)
                    x64_mov_rr(&ctx->buf, p0, arg_regs[0]);
            } else if (nargs_reg == 2) {
                X64Reg p0 = x64_alloc_regs[0];
                X64Reg p1 = x64_alloc_regs[1];
                if (arg_regs[0] == p1 && arg_regs[1] == p0) {
                    /* Swap via RCX scratch (already clobbered above) */
                    x64_mov_rr(&ctx->buf, X64_RCX, arg_regs[0]);
                    x64_mov_rr(&ctx->buf, p0, arg_regs[1]);
                    x64_mov_rr(&ctx->buf, p1, X64_RCX);
                } else if (arg_regs[1] == p0) {
                    /* Write p1 first to avoid clobbering arg_regs[1] */
                    if (arg_regs[1] != p1)
                        x64_mov_rr(&ctx->buf, p1, arg_regs[1]);
                    if (arg_regs[0] != p0)
                        x64_mov_rr(&ctx->buf, p0, arg_regs[0]);
                } else {
                    if (arg_regs[0] != p0)
                        x64_mov_rr(&ctx->buf, p0, arg_regs[0]);
                    if (arg_regs[1] != p1)
                        x64_mov_rr(&ctx->buf, p1, arg_regs[1]);
                }
            }
            /* ABI_ARG1 = coro (platform calling convention) */
            x64_mov_rr(&ctx->buf, X64_ABI_ARG1, X64_CORO_REG);
            /* CALL R11 (jit_fast_entry) */
            x64_call_r(&ctx->buf, X64_SCRATCH_REG);

            /* Store callee tag (RCX) to call_result_tag */
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET,
                       X64_RCX);

            /* Nested deopt check */
            x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t) XM_DEOPT_MARKER);
            x64_cmp_rr(&ctx->buf, X64_RAX, X64_RCX);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_E));
            uint32_t je_cascade_kr = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* JMP done */
            x64_emit8(&ctx->buf, 0xE9);
            uint32_t jmp_done_kr = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* Cascade: clear stale deopt_id */
            uint32_t cascade_kr_pos = ctx->buf.pos;
            x64_xor_rr(&ctx->buf, X64_RCX, X64_RCX);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET, X64_RCX);

            /* Slow path */
            uint32_t slow_kr_pos = ctx->buf.pos;
            x64_patch_rel32(&ctx->buf, je_slow_kr, slow_kr_pos);
            x64_patch_rel32(&ctx->buf, je_cascade_kr, cascade_kr_pos);

            /* Store nargs via RCX; call_c_stub calls R11 as func ptr */
            x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t) nargs_reg);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET, X64_RCX);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_call_func);
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *cp_kr = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            cp_kr->emit_pos = ctx->buf.pos;
            cp_kr->target_blk = 0;
            cp_kr->type = X64_PATCH_CALL_C;
            cp_kr->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_call_c = true;

            /* Done */
            uint32_t done_kr_pos = ctx->buf.pos;
            x64_patch_rel32(&ctx->buf, jmp_done_kr, done_kr_pos);

            /* Save RAX to R11 before restoring caller-saved regs */
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_RAX);

            /* Restore caller-saved regs */
            if (save_frame > 0) {
                int off = 0;
                for (int i = 0; i < ngp; i++) {
                    x64_mov_rm(&ctx->buf, live_gp[i], X64_RSP, off);
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    x64_movsd_rm(&ctx->buf, live_fp_arr[f], X64_RSP, off);
                    off += 8;
                }
                x64_add_ri(&ctx->buf, X64_RSP, save_frame);
            }

            if (xm_ref_is_vreg(ins->dst) && rd != X64_SCRATCH_REG)
                x64_mov_rr(&ctx->buf, rd, X64_SCRATCH_REG);
            break;
        }

        /* CALL_DIRECT: cross-function JIT→JIT call with inline closure dispatch.
         * args[0] = nargs (vreg/const), args[1] = const_ptr(xr_jit_call_func)
         * Fast path: verify closure→proto→jit_entry chain inline, CALL via stub.
         * Slow path: fall through to xr_jit_call_func C bridge. */
        case XM_CALL_DIRECT: {
            x64_emit_call_args_from_pool(ctx, ins);
            x64_emit_ptr_spill_writeback(ctx);

            uint32_t smap_id_d = x64_record_safepoint(ctx);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_d);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_SCRATCH_REG);

            /* Fast path: load closure from call_args[0] into R11 */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG,
                       (int32_t) XM_JIT_CALL_ARGS_OFFSET);

            /* GC type guard: only XR_TFUNCTION (5) uses fast path.
             * Classes have different layout and need C bridge. */
            x64_movzx_rm8(&ctx->buf, X64_RCX, X64_SCRATCH_REG, (int32_t) XM_GC_TYPE_OFFSET);
            x64_cmp_ri(&ctx->buf, X64_RCX, 5);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_NE));
            uint32_t jne_type_d = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* Set call_closure for callee's upvalue access */
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_CLOSURE_OFFSET,
                       X64_SCRATCH_REG);

            /* R11 = closure->proto */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
                       (int32_t) XM_CLOSURE_PROTO_OFFSET);
            /* Null proto guard */
            x64_test_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_E));
            uint32_t je_proto_d = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* Save proto ptr to jit_ctx->call_proto */
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_PROTO_OFFSET,
                       X64_SCRATCH_REG);

            /* R11 = proto->jit_entry */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
                       (int32_t) XM_PROTO_JIT_ENTRY_OFFSET);
            /* Null jit_entry guard */
            x64_test_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_E));
            uint32_t je_entry_d = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* Fast path: CALL via call_c_stub with R11=jit_entry,
             * extra_arg = &call_args[1] */
            x64_lea(&ctx->buf, X64_RCX, X64_JIT_CTX_REG, (int32_t) (XM_JIT_CALL_ARGS_OFFSET + 8));
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET, X64_RCX);
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *cp_fast = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            cp_fast->emit_pos = ctx->buf.pos;
            cp_fast->target_blk = 0;
            cp_fast->type = X64_PATCH_CALL_C;
            cp_fast->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_call_c = true;

            /* Nested deopt cascade check */
            x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t) XM_DEOPT_MARKER);
            x64_cmp_rr(&ctx->buf, X64_RAX, X64_RCX);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_E));
            uint32_t je_cascade_d = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* JMP done */
            x64_emit8(&ctx->buf, 0xE9);
            uint32_t jmp_done_d = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* Cascade: clear stale deopt_id */
            uint32_t cascade_d_pos = ctx->buf.pos;
            x64_xor_rr(&ctx->buf, X64_RCX, X64_RCX);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET, X64_RCX);

            /* Slow path: xr_jit_call_func(coro, nargs) via call_c_stub */
            uint32_t slow_d_pos = ctx->buf.pos;
            x64_patch_rel32(&ctx->buf, jne_type_d, slow_d_pos);
            x64_patch_rel32(&ctx->buf, je_proto_d, slow_d_pos);
            x64_patch_rel32(&ctx->buf, je_entry_d, slow_d_pos);
            x64_patch_rel32(&ctx->buf, je_cascade_d, cascade_d_pos);

            /* Extract nargs for the helper call */
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                uint64_t nargs_const = (uint64_t) ctx->func->consts[ci].val.raw;
                x64_load_imm64(&ctx->buf, X64_RCX, nargs_const);
            } else {
                X64Reg nreg = x64_get_operand(ctx, ins->args[0], X64_RCX);
                if (nreg != X64_RCX)
                    x64_mov_rr(&ctx->buf, X64_RCX, nreg);
            }
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET, X64_RCX);

            /* Load xr_jit_call_func into R11 (or from args[1] const) */
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                uint64_t fn = (uint64_t) ctx->func->consts[ci].val.raw;
                x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, fn);
            } else {
                x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_call_func);
            }
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *cp_slow = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            cp_slow->emit_pos = ctx->buf.pos;
            cp_slow->target_blk = 0;
            cp_slow->type = X64_PATCH_CALL_C;
            cp_slow->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);

            /* Done */
            uint32_t done_d_pos = ctx->buf.pos;
            x64_patch_rel32(&ctx->buf, jmp_done_d, done_d_pos);

            /* Load call_result_tag → slot_runtime_tags[bc_slot] */
            int16_t bc_slot_d = -1;
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg)
                    bc_slot_d = ctx->func->vregs[vi].bc_slot;
            }
            if (bc_slot_d >= 0 && bc_slot_d < 256) {
                x64_movzx_rm8(&ctx->buf, X64_RCX, X64_JIT_CTX_REG,
                              (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET);
                int32_t tag_off = (int32_t) XM_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot_d;
                x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, tag_off, X64_RCX);
            }

            /* Move result (RAX) → dst */
            if (xm_ref_is_vreg(ins->dst) && rd != X64_RAX)
                x64_mov_rr(&ctx->buf, rd, X64_RAX);
            break;
        }

        case XM_CALL:
            /* CALL (generic) falls through to xr_jit_call_func via CALL_C */
            xr_log_warning("x64-cg", "generic XM_CALL op not yet implemented");
            ctx->had_error = true;
            break;

        default:
            return false; /* not a call op */
    }
    return true;
}

#endif /* __x86_64__ || _M_X64 */
