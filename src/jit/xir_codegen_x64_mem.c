/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_codegen_x64_mem.c - x86-64 codegen: tag/box/guard/RT ops + safepoint
 *
 * KEY CONCEPT:
 *   Handles higher-level XIR opcodes that operate on tagged values,
 *   runtime type dispatch, and GC safepoints.  Split from
 *   xir_codegen_x64.c to keep each file under the 3000-line limit.
 *
 * OPCODES:
 *   TAG_LOAD, TAG_CHECK, BOX_I64/F64, UNBOX_I64/F64,
 *   GUARD_TAG, GUARD_BOUNDS, GUARD_NONNULL, GUARD_CLASS, GUARD_KLASS,
 *   GUARD_SHAPE, DEOPT,
 *   SAFEPOINT,
 *   RT_ADD/SUB/MUL/DIV/MOD, RT_UNM, RT_LT/LE/EQ,
 *   RT_PRINT, RT_ARRAY_x, RT_MAP_x, RT_INDEX_x, RT_ISNULL
 */

#if defined(__x86_64__) || defined(_M_X64)

#include "xir_codegen_x64_internal.h"
#include "xir_offsets.h"
#include "xir_jit_runtime.h"

/* Dispatch function for tag/guard/RT/safepoint opcodes.
 * Returns true if the opcode was handled, false otherwise. */
bool x64_emit_mem_ins(X64CodegenCtx *ctx, XirIns *ins, X64Reg rd) {
    CODEGEN_CHECK(ctx, ctx != NULL, "emit_mem_ins: NULL ctx");
    CODEGEN_CHECK(ctx, ins != NULL, "emit_mem_ins: NULL ins");

    switch (ins->op) {
        /* ========== Tag load/check ========== */
        case XIR_TAG_LOAD: {
            X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            int32_t offset = 0;
            if (!xir_ref_is_none(ins->args[1]) && xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            x64_movzx_rm8(&ctx->buf, rd, ptr, offset);
            break;
        }

        /* ========== BOX/UNBOX ========== */
        case XIR_BOX_I64:
        case XIR_BOX_F64: {
            /* BOX is a no-op inside JIT (values are always raw/untagged) */
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (rd != rn)
                x64_mov_rr(&ctx->buf, rd, rn);
            break;
        }

        case XIR_UNBOX_I64: {
            CODEGEN_CHECK(ctx, !xir_ref_is_none(ins->args[0]), "UNBOX_I64: missing src");
            uint8_t src_type = XR_REP_I64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                if (vi < ctx->func->nvreg)
                    src_type = ctx->func->vregs[vi].rep;
            }
            if (src_type == XR_REP_PTR) {
                /* Source is pointer to XrValue — load payload from [ptr+8] */
                X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                x64_mov_rm(&ctx->buf, rd, ptr, XIR_XRVALUE_PAYLOAD_OFFSET);
            } else {
                X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                if (rd != rn)
                    x64_mov_rr(&ctx->buf, rd, rn);
            }
            break;
        }

        case XIR_UNBOX_F64: {
            CODEGEN_CHECK(ctx, !xir_ref_is_none(ins->args[0]), "UNBOX_F64: missing src");
            uint8_t src_type = XR_REP_F64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                if (vi < ctx->func->nvreg)
                    src_type = ctx->func->vregs[vi].rep;
            }
            if (src_type == XR_REP_PTR) {
                X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
                x64_movsd_rm(&ctx->buf, fd, ptr, XIR_XRVALUE_PAYLOAD_OFFSET);
            } else {
                X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                if (rd != rn)
                    x64_mov_rr(&ctx->buf, rd, rn);
            }
            break;
        }

        /* ========== Guard ops ========== */
        case XIR_GUARD_TAG: {
            /* args[0] = tagged value ptr, args[1] = expected tag (const i64) */
            X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            /* Load tag byte: MOVZX r64, byte [val_reg + tag_offset] */
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, val_reg, XIR_XRVALUE_TAG_OFFSET);
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                int32_t expected = (int32_t) ctx->func->consts[ci].val.raw;
                x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, expected);
            } else {
                X64Reg exp_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                x64_cmp_rr(&ctx->buf, X64_SCRATCH_REG, exp_reg);
            }
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            break;
        }

        case XIR_GUARD_BOUNDS: {
            /* deopt if (unsigned)index >= (unsigned)length */
            X64Reg idx_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg len_reg = x64_get_operand(
                ctx, ins->args[1], idx_reg == X64_SCRATCH_REG ? X64_RCX : X64_SCRATCH_REG);
            x64_cmp_rr(&ctx->buf, idx_reg, len_reg);
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_AE); /* unsigned >= */
            break;
        }

        case XIR_GUARD_NONNULL: {
            X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_test_rr(&ctx->buf, val_reg, val_reg);
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_E); /* ZF=1 → null → deopt */
            break;
        }

        case XIR_GUARD_CLASS: {
            /* Check shape_id in GC header (uint16 at gc_extra_offset) */
            X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_movzx_rm16(&ctx->buf, X64_SCRATCH_REG, obj, (int32_t) XIR_GC_EXTRA_OFFSET);
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                int32_t expected = (int32_t) ctx->func->consts[ci].val.raw;
                x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, expected);
            } else {
                X64Reg exp_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                x64_cmp_rr(&ctx->buf, X64_SCRATCH_REG, exp_reg);
            }
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            break;
        }

        case XIR_GUARD_KLASS: {
            /* Check inst->klass pointer (at offset XIR_INSTANCE_KLASS_OFFSET) */
            X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, obj, (int32_t) XIR_INSTANCE_KLASS_OFFSET);
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                uint64_t expected = (uint64_t) ctx->func->consts[ci].val.i64;
                /* CMP r64,imm64 doesn't exist: save actual klass to RCX,
                 * load expected to R11, then CMP RCX, R11. */
                x64_mov_rr(&ctx->buf, X64_RCX, X64_SCRATCH_REG);
                x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, expected);
                x64_cmp_rr(&ctx->buf, X64_RCX, X64_SCRATCH_REG);
            } else {
                X64Reg exp_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                x64_cmp_rr(&ctx->buf, X64_SCRATCH_REG, exp_reg);
            }
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            break;
        }

        case XIR_GUARD_SHAPE: {
            /* Check obj null, alignment, type==XR_TJSON, then shape_id */
            X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_emit_deopt_id(ctx, ins);
            /* Null check */
            x64_test_rr(&ctx->buf, obj, obj);
            x64_emit_deopt_jcc(ctx, X64_CC_E);
            /* Alignment check: obj & 7 != 0 → deopt */
            x64_test_ri(&ctx->buf, obj, 0x7);
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            /* Type check: gc.type at offset 8, must be 23 (XR_TJSON) */
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, obj, (int32_t) XIR_GC_HDR_TYPE_OFFSET);
            x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, 23);
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            /* Load gc.extra (uint16 at offset 10) */
            x64_movzx_rm16(&ctx->buf, X64_SCRATCH_REG, obj, (int32_t) XIR_GC_HDR_EXTRA_OFFSET);
            /* shape_id = extra >> 2 */
            x64_shr_ri(&ctx->buf, X64_SCRATCH_REG, 2);
            /* Compare with expected shape_id */
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                int32_t expected_id = (int32_t) ctx->func->consts[ci].val.raw;
                x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, expected_id);
            }
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            break;
        }

        case XIR_DEOPT: {
            /* Unconditional deopt */
            x64_emit_deopt_id(ctx, ins);
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *p = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE9); /* JMP rel32 */
            p->emit_pos = ctx->buf.pos;
            p->target_blk = 0;
            p->type = X64_PATCH_DEOPT_JMP;
            p->cc = X64_CC_E; /* unused */
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_deopt = true;
            break;
        }

        case XIR_TAG_CHECK: {
            /* Compare tag byte against expected constant, deopt on mismatch */
            X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, val_reg, XIR_XRVALUE_TAG_OFFSET);
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                int32_t expected = (int32_t) ctx->func->consts[ci].val.raw;
                x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, expected);
            }
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            break;
        }

        /* ========== Safepoint ========== */
        case XIR_SAFEPOINT: {
            /* Record safepoint bitmap so GC can identify roots if we fault */
            CODEGEN_CHECK(ctx, ctx->nsmap < XIR_MAX_STACK_MAP_ENTRIES, "too many safepoints");
            uint32_t smap_id_sp = x64_record_safepoint(ctx);
            /* Store smap_id to jit_ctx (GC reads this in signal handler) */
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_sp);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XIR_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_SCRATCH_REG);
            /* Guard page poll: load from safepoint_page → faults if armed */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG,
                       (int32_t) XIR_JIT_SAFEPOINT_PAGE_OFFSET);
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG, 0);
            break;
        }

        /* ========== Mixed-type runtime arithmetic ========== */
        case XIR_RT_ADD:
        case XIR_RT_SUB:
        case XIR_RT_MUL:
        case XIR_RT_DIV:
        case XIR_RT_MOD: {
            CODEGEN_CHECK(ctx, !xir_ref_is_none(ins->args[0]), "RT arith: missing lhs");
            CODEGEN_CHECK(ctx, !xir_ref_is_none(ins->args[1]), "RT arith: missing rhs");
            uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                if (ai < ctx->func->nvreg)
                    ta = ctx->func->vregs[ai].rep;
            }
            if (xir_ref_is_vreg(ins->args[1])) {
                uint32_t bi = XIR_REF_INDEX(ins->args[1]);
                if (bi < ctx->func->nvreg)
                    tb = ctx->func->vregs[bi].rep;
            }

            if ((ta == XR_REP_I64 || ta == XR_REP_F64) && (tb == XR_REP_I64 || tb == XR_REP_F64)) {
                /* Both numeric: convert to FP, operate, result in FP dst */
                X64Xmm fa;
                if (ta == XR_REP_F64) {
                    fa = x64_get_fp_operand(ctx, ins->args[0], X64_XMM14);
                } else {
                    X64Reg ga = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                    fa = X64_XMM14; /* scratch FP */
                    x64_cvtsi2sd(&ctx->buf, fa, ga);
                }
                X64Xmm fb;
                if (tb == XR_REP_F64) {
                    fb = x64_get_fp_operand(ctx, ins->args[1], X64_XMM15);
                } else {
                    X64Reg gb = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                    fb = X64_XMM15; /* scratch FP */
                    x64_cvtsi2sd(&ctx->buf, fb, gb);
                }
                /* Ensure dst != operand aliasing issues */
                X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
                if (fd != fa)
                    x64_movsd_rr(&ctx->buf, fd, fa);
                switch (ins->op) {
                    case XIR_RT_ADD:
                        x64_addsd(&ctx->buf, fd, fb);
                        break;
                    case XIR_RT_SUB:
                        x64_subsd(&ctx->buf, fd, fb);
                        break;
                    case XIR_RT_MUL:
                        x64_mulsd(&ctx->buf, fd, fb);
                        break;
                    case XIR_RT_DIV:
                        x64_divsd(&ctx->buf, fd, fb);
                        break;
                    case XIR_RT_MOD: {
                        /* fmod: a - trunc(a/b) * b */
                        x64_movsd_rr(&ctx->buf, X64_XMM14, fd);
                        x64_divsd(&ctx->buf, X64_XMM14, fb);
                        x64_cvttsd2si(&ctx->buf, X64_SCRATCH_REG, X64_XMM14);
                        x64_cvtsi2sd(&ctx->buf, X64_XMM14, X64_SCRATCH_REG);
                        x64_mulsd(&ctx->buf, X64_XMM14, fb);
                        x64_subsd(&ctx->buf, fd, X64_XMM14);
                        break;
                    }
                    default:
                        break;
                }
            } else {
                /* Unknown types: deopt */
                x64_emit_deopt_id(ctx, ins);
                CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
                X64BranchPatch *p = &ctx->patches[ctx->npatch];
                x64_emit8(&ctx->buf, 0xE9);
                p->emit_pos = ctx->buf.pos;
                p->target_blk = 0;
                p->type = X64_PATCH_DEOPT_JMP;
                p->cc = X64_CC_E;
                ctx->npatch++;
                x64_emit32(&ctx->buf, 0);
                ctx->has_deopt = true;
            }
            break;
        }

        case XIR_RT_UNM: {
            CODEGEN_CHECK(ctx, !xir_ref_is_none(ins->args[0]), "RT_UNM: missing operand");
            uint8_t ta = XR_REP_I64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                if (ai < ctx->func->nvreg)
                    ta = ctx->func->vregs[ai].rep;
            }
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            if (ta == XR_REP_F64) {
                X64Xmm fa = x64_get_fp_operand(ctx, ins->args[0], X64_XMM14);
                /* XORPD + SUBSD for negation: 0.0 - x */
                x64_xorpd(&ctx->buf, fd, fd);
                x64_subsd(&ctx->buf, fd, fa);
            } else {
                X64Reg ga = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                x64_cvtsi2sd(&ctx->buf, fd, ga);
                x64_xorpd(&ctx->buf, X64_XMM15, X64_XMM15);
                x64_subsd(&ctx->buf, X64_XMM15, fd);
                x64_movsd_rr(&ctx->buf, fd, X64_XMM15);
            }
            break;
        }

        case XIR_RT_LT:
        case XIR_RT_LE:
        case XIR_RT_EQ: {
            uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                if (ai < ctx->func->nvreg)
                    ta = ctx->func->vregs[ai].rep;
            }
            if (xir_ref_is_vreg(ins->args[1])) {
                uint32_t bi = XIR_REF_INDEX(ins->args[1]);
                if (bi < ctx->func->nvreg)
                    tb = ctx->func->vregs[bi].rep;
            }
            if ((ta == XR_REP_I64 || ta == XR_REP_F64) && (tb == XR_REP_I64 || tb == XR_REP_F64)) {
                X64Xmm fa;
                if (ta == XR_REP_F64) {
                    fa = x64_get_fp_operand(ctx, ins->args[0], X64_XMM14);
                } else {
                    X64Reg ga = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                    fa = X64_XMM14;
                    x64_cvtsi2sd(&ctx->buf, fa, ga);
                }
                X64Xmm fb;
                if (tb == XR_REP_F64) {
                    fb = x64_get_fp_operand(ctx, ins->args[1], X64_XMM15);
                } else {
                    X64Reg gb = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                    fb = X64_XMM15;
                    x64_cvtsi2sd(&ctx->buf, fb, gb);
                }
                x64_ucomisd(&ctx->buf, fa, fb);
                /* UCOMISD sets ZF/CF for unordered float comparison.
                 * SETcc to get 0/1 result in rd. */
                X64Cond cc;
                if (ins->op == XIR_RT_LT)
                    cc = X64_CC_B; /* CF=1 → below */
                else if (ins->op == XIR_RT_LE)
                    cc = X64_CC_BE; /* CF=1 or ZF=1 */
                else
                    cc = X64_CC_E; /* ZF=1 */
                x64_xor_rr(&ctx->buf, rd, rd);
                /* SETcc r8: 0F 9x /0 — set low byte of rd */
                x64_emit8(&ctx->buf, (rd > 7) ? 0x41 : 0x40); /* REX prefix */
                x64_emit8(&ctx->buf, 0x0F);
                x64_emit8(&ctx->buf, (uint8_t) (0x90 | cc));
                x64_emit8(&ctx->buf, (uint8_t) (0xC0 | ((uint8_t) rd & 7)));
            } else {
                x64_emit_deopt_id(ctx, ins);
                CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
                X64BranchPatch *p = &ctx->patches[ctx->npatch];
                x64_emit8(&ctx->buf, 0xE9);
                p->emit_pos = ctx->buf.pos;
                p->target_blk = 0;
                p->type = X64_PATCH_DEOPT_JMP;
                p->cc = X64_CC_E;
                ctx->npatch++;
                x64_emit32(&ctx->buf, 0);
                ctx->has_deopt = true;
            }
            break;
        }

        case XIR_RT_ARRAY_NEW:
        case XIR_RT_MAP_NEW: {
            /* Inline CALL_C sequence: alloc array/map via runtime helper.
             * args[0] = capacity, dst = result ptr. */
            x64_emit_ptr_spill_writeback(ctx);

            uint32_t smap_id = x64_record_safepoint(ctx);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                         (int32_t) XIR_JIT_ACTIVE_SMAP_ID_OFFSET, X64_SCRATCH_REG);

            /* extra_arg = capacity */
            if (xir_ref_is_const(ins->args[0])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[0]);
                uint64_t val = (uint64_t) ctx->func->consts[ci].val.raw;
                x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, val);
            } else {
                X64Reg r = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                if (r != X64_SCRATCH_REG)
                    x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, r);
            }
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                       (int32_t) X64_EXTRA_ARG_OFFSET, X64_SCRATCH_REG);

            /* Clear deopt_id */
            x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                         (int32_t) XIR_JIT_DEOPT_ID_OFFSET, X64_SCRATCH_REG);

            /* Load runtime helper pointer */
            void *fn = (ins->op == XIR_RT_ARRAY_NEW)
                           ? (void *) (uintptr_t) xr_jit_rt_array_new
                           : (void *) (uintptr_t) xr_jit_rt_map_new;
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) fn);

            /* CALL call_c_stub */
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *p = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            p->emit_pos = ctx->buf.pos;
            p->target_blk = 0;
            p->type = X64_PATCH_CALL_C;
            p->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_call_c = true;

            /* Result ptr in RAX */
            if (xir_ref_is_vreg(ins->dst) && rd != X64_RAX)
                x64_mov_rr(&ctx->buf, rd, X64_RAX);
            break;
        }

        case XIR_RT_ARRAY_PUSH: {
            /* Inline CALL_C sequence: push value into array.
             * args[0] = array ptr, args[1] = value to push. */
            x64_emit_ptr_spill_writeback(ctx);

            uint32_t smap_id = x64_record_safepoint(ctx);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                         (int32_t) XIR_JIT_ACTIVE_SMAP_ID_OFFSET, X64_SCRATCH_REG);

            /* Store call_args[0] = array ptr */
            int32_t off0 = (int32_t) XIR_JIT_CALL_ARGS_OFFSET;
            X64Reg arr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, off0, arr);

            /* Store call_args[1] = value payload */
            int32_t off1 = (int32_t) (XIR_JIT_CALL_ARGS_OFFSET + 8);
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                uint64_t val = (uint64_t) ctx->func->consts[ci].val.raw;
                x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, val);
                x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, off1, X64_SCRATCH_REG);
            } else {
                X64Reg vr = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, off1, vr);
            }

            /* Store call_arg_tags: tag[0]=PTR(arr), tag[1]=inferred from type */
            uint8_t tag0 = 4; /* XR_TAG_PTR */
            uint8_t tag1 = XR_RTAG_UNKNOWN;
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                tag1 = const_rep_to_value_tag(ctx->func->consts[ci].rep);
            } else {
                XirType ct = xir_ref_ctype(ctx->func, ins->args[1]);
                uint8_t vk = type_kind_to_vtag(ct.kind);
                if (vtag_is_concrete(vk))
                    tag1 = vtag_to_value_tag(vk);
            }
            uint64_t tag_pack = (uint64_t) tag0 | ((uint64_t) tag1 << 8);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, tag_pack);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                       (int32_t) XIR_JIT_CALL_ARG_TAGS_OFFSET, X64_SCRATCH_REG);

            /* Dynamic tag patch: if tag1 unknown, read from slot_runtime_tags */
            if (tag1 == XR_RTAG_UNKNOWN && xir_ref_is_vreg(ins->args[1])) {
                uint32_t ai = XIR_REF_INDEX(ins->args[1]);
                if (ai < ctx->func->nvreg) {
                    int16_t bc_slot = ctx->func->vregs[ai].bc_slot;
                    if (bc_slot >= 0 && bc_slot < 256) {
                        int32_t src_off = (int32_t) XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot;
                        int32_t dst_off = (int32_t) XIR_JIT_CALL_ARG_TAGS_OFFSET + 1;
                        x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG,
                                      X64_JIT_CTX_REG, src_off);
                        x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG,
                                    dst_off, X64_SCRATCH_REG);
                    }
                }
            }

            /* extra_arg = 0 (unused) */
            x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                       (int32_t) X64_EXTRA_ARG_OFFSET, X64_SCRATCH_REG);

            /* Clear deopt_id */
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                         (int32_t) XIR_JIT_DEOPT_ID_OFFSET, X64_SCRATCH_REG);

            /* Load runtime helper pointer */
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG,
                           (uint64_t) (uintptr_t) xr_jit_rt_array_push);

            /* CALL call_c_stub */
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *pa = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            pa->emit_pos = ctx->buf.pos;
            pa->target_blk = 0;
            pa->type = X64_PATCH_CALL_C;
            pa->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_call_c = true;
            break;
        }

        case XIR_RT_PRINT:
        case XIR_RT_ARRAY_LEN:
        case XIR_RT_INDEX_GET:
        case XIR_RT_INDEX_SET:
            /* Not yet lowered to CALL_C; fall through to warning */
            xr_log_warning("x64-cg", "RT opcode %d should use CALL_C path", ins->op);
            break;

        case XIR_RT_ISNULL: {
            X64Reg val = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_xor_rr(&ctx->buf, rd, rd);
            x64_test_rr(&ctx->buf, val, val);
            /* SETcc: SETE rd (ZF=1 → null → result=1) */
            x64_emit8(&ctx->buf, (rd > 7) ? 0x41 : 0x40);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, 0x94); /* SETE */
            x64_emit8(&ctx->buf, (uint8_t) (0xC0 | ((uint8_t) rd & 7)));
            break;
        }

        default:
            return false; /* not handled */
    }

    return true; /* handled */
}

#endif /* __x86_64__ || _M_X64 */
