/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_riscv64.c - Xm -> RISC-V 64 machine code generation
 *
 * KEY CONCEPT:
 *   Translates Xm SSA instructions into RV64GD machine code.
 *   Single-pass emit with deferred branch patching.
 *   Fixed-width 32-bit instructions (instruction-index addressing).
 *
 * RISC-V SPECIFICS vs x86-64/ARM64:
 *   - Fixed 32-bit instruction encoding, instruction-index code buffer
 *   - B-type branches have +-4 KiB range; J-type (JAL) has +-1 MiB range
 *   - No flags register: comparisons produce GPR 0/1, branches test two GPRs
 *   - 3-operand ISA: rd = rs1 OP rs2 (no destructive 2-operand constraint)
 *   - Hardware DIV/REM with defined zero-divisor behaviour (no trap)
 *   - No link register implicit in CALL: JAL writes rd=ra explicitly
 *   - FENCE.I required after writing code to ensure I-cache coherence
 */

#ifdef __riscv

#include "xm_codegen_riscv64_internal.h"
#include "xm_coalesce.h"
#include "xm_code_alloc.h"
#include "xm_offsets.h"
#include "xm_jit_runtime.h"
#include "../coro/xcoroutine.h"
#include <string.h>

/* ========== Register Mapping Tables ========== */

/* Allocatable GPRs: must match rv64_gpr_alloc[] in xm_target_riscv64.c.
 * Index i into this array gives the hardware register for RA "register i". */
static const Rv64Reg rv64_alloc_regs[] = {
    /* Caller-saved (first 12) */
    RV64_A0,
    RV64_A1,
    RV64_A2,
    RV64_A3,
    RV64_A4,
    RV64_A5,
    RV64_A6,
    RV64_A7,
    RV64_T0,
    RV64_T1,
    RV64_T2,
    RV64_T3,
    /* Callee-saved (next 9) */
    RV64_S1,
    RV64_S2,
    RV64_S3,
    RV64_S4,
    RV64_S5,
    RV64_S6,
    RV64_S7,
    RV64_S8,
    RV64_S9,
};

#define RV64_MAX_PHYS_REGS 21

/* Allocatable FPRs: same order as rv64_fpr_alloc[]. */
static const Rv64Freg rv64_alloc_fp_regs[] = {
    /* Caller-saved (first 12) */
    RV64_FA0,
    RV64_FA1,
    RV64_FA2,
    RV64_FA3,
    RV64_FA4,
    RV64_FA5,
    RV64_FA6,
    RV64_FA7,
    RV64_FT0,
    RV64_FT1,
    RV64_FT2,
    RV64_FT3,
    /* Callee-saved (next 8) */
    RV64_FS0,
    RV64_FS1,
    RV64_FS2,
    RV64_FS3,
    RV64_FS4,
    RV64_FS5,
    RV64_FS6,
    RV64_FS7,
};

#define RV64_MAX_FP_REGS 20
#define RV64_SCRATCH_FP RV64_FT11
#define RV64_MAX_VREGS 4096

/* ========== Register Lookup ========== */

XR_FUNC Rv64Reg rv64_get_reg(Rv64CodegenCtx *ctx, XmRef ref) {
    if (xm_ref_is_none(ref))
        return RV64_SCRATCH_REG;
    if (!xm_ref_is_vreg(ref))
        return RV64_SCRATCH_REG;
    uint32_t idx = XM_REF_INDEX(ref);

    /* FP vregs use float registers — return scratch GP to avoid misuse */
    if (idx < ctx->func->nvreg && ctx->func->vregs[idx].rep == XR_REP_F64)
        return RV64_SCRATCH_REG;

    int8_t ri;
    if (ctx->vreg_override && idx < ctx->xra->nvreg && ctx->vreg_override[idx] != -128)
        ri = ctx->vreg_override[idx];
    else {
        ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    }

    if (ri < 0) {
        /* Vreg not in register: attempt spill reload */
        if (xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos) ||
            xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos + 1)) {
            int16_t slot = xra_vreg_spill(ctx->xra, idx);
            if (slot >= 0) {
                int32_t offset = -(RV64_SPILL_BASE + slot * 8);
                XR_DCHECK(offset >= -2048, "rv64_get_reg: spill offset out of 12-bit range");
                rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_FP, offset));
                return RV64_SCRATCH_REG;
            }
        }
        ctx->had_error = true;
        return RV64_SCRATCH_REG;
    }
    RV64_CODEGEN_CHECK(ctx, ri < RV64_MAX_PHYS_REGS, "rv64_get_reg: reg index OOB");
    return rv64_alloc_regs[ri];
}

XR_FUNC Rv64Reg rv64_get_operand(Rv64CodegenCtx *ctx, XmRef ref, Rv64Reg scratch) {
    if (xm_ref_is_const(ref)) {
        uint32_t ci = XM_REF_INDEX(ref);
        RV64_CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "rv64_get_operand: const OOB");
        uint64_t val = ctx->func->consts[ci].val.raw;
        rv64_load_imm64(&ctx->buf, scratch, val);
        return scratch;
    }
    return rv64_get_reg(ctx, ref);
}

/* ========== FP Register Lookup ========== */

XR_FUNC Rv64Freg rv64_get_fp_reg(Rv64CodegenCtx *ctx, XmRef ref) {
    RV64_CODEGEN_CHECK(ctx, xm_ref_is_vreg(ref), "rv64_get_fp_reg: not a vreg");
    uint32_t idx = XM_REF_INDEX(ref);

    int8_t ri;
    if (ctx->vreg_override && idx < ctx->xra->nvreg && ctx->vreg_override[idx] != -128)
        ri = ctx->vreg_override[idx];
    else {
        ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    }

    if (ri < 0) {
        /* FP spill reload via FLD */
        if (xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos) ||
            xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos + 1)) {
            int16_t slot = xra_vreg_spill(ctx->xra, idx);
            if (slot >= 0) {
                int32_t offset = -(RV64_SPILL_BASE + slot * 8);
                XR_DCHECK(offset >= -2048, "rv64_get_fp_reg: spill offset out of range");
                rv64_buf_emit(&ctx->buf, rv64_fld(RV64_SCRATCH_FP, RV64_FP, offset));
                return RV64_SCRATCH_FP;
            }
        }
        ctx->had_error = true;
        return RV64_SCRATCH_FP;
    }
    RV64_CODEGEN_CHECK(ctx, ri < RV64_MAX_FP_REGS, "rv64_get_fp_reg: reg index OOB");
    return rv64_alloc_fp_regs[ri];
}

XR_FUNC Rv64Freg rv64_get_fp_operand(Rv64CodegenCtx *ctx, XmRef ref, Rv64Freg scratch) {
    if (xm_ref_is_const(ref)) {
        uint32_t ci = XM_REF_INDEX(ref);
        RV64_CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "rv64_get_fp_operand: const OOB");
        uint64_t raw = ctx->func->consts[ci].val.raw;
        rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, raw);
        rv64_buf_emit(&ctx->buf, rv64_fmv_d_x(scratch, RV64_SCRATCH_REG));
        return scratch;
    }
    return rv64_get_fp_reg(ctx, ref);
}

/* ========== Immediate Helpers ========== */

XR_FUNC void rv64_load_imm64(Rv64Buf *buf, Rv64Reg dst, uint64_t val) {
    XR_DCHECK(buf != NULL, "rv64_load_imm64: NULL buf");
    if (val == 0) {
        rv64_buf_emit(buf, rv64_mv(dst, RV64_X0));
        return;
    }
    int64_t sval = (int64_t) val;
    /* Small 12-bit signed immediate: -2048..2047 */
    if (sval >= -2048 && sval <= 2047) {
        rv64_buf_emit(buf, rv64_addi(dst, RV64_X0, (int32_t) sval));
        return;
    }
    /* 32-bit: LUI + ADDI */
    if (sval >= -2147483648LL && sval <= 2147483647LL) {
        int32_t lo = (int32_t) (sval & 0xFFF);
        int32_t hi = (int32_t) ((sval - lo) >> 12);
        rv64_buf_emit(buf, rv64_lui(dst, (uint32_t) hi & 0xFFFFF));
        if (lo != 0)
            rv64_buf_emit(buf, rv64_addi(dst, dst, lo));
        return;
    }
    /* Full 64-bit: build in steps using shifts.
     * Split into 4 x 16-bit groups and build from MSB down.
     *   LUI+ADDI for upper 32, then SLLI+ADDI for each 11-bit chunk. */
    int32_t upper32 = (int32_t) (sval >> 32);
    int32_t lo32 = (int32_t) (sval & 0xFFFFFFFF);

    /* Load upper 32 bits */
    int32_t ulo = upper32 & 0xFFF;
    if (ulo >= 0x800)
        ulo -= 0x1000;
    int32_t uhi = (upper32 - ulo) >> 12;
    rv64_buf_emit(buf, rv64_lui(dst, (uint32_t) uhi & 0xFFFFF));
    if (ulo != 0)
        rv64_buf_emit(buf, rv64_addi(dst, dst, ulo));

    /* Shift left 12, add bits [31:20] of lower 32 */
    rv64_buf_emit(buf, rv64_slli(dst, dst, 12));
    int32_t chunk1 = (lo32 >> 20) & 0xFFF;
    if (chunk1 >= 0x800)
        chunk1 -= 0x1000;
    if (chunk1 != 0)
        rv64_buf_emit(buf, rv64_addi(dst, dst, chunk1));

    /* Shift left 12, add bits [19:8] */
    rv64_buf_emit(buf, rv64_slli(dst, dst, 12));
    int32_t chunk2 = (lo32 >> 8) & 0xFFF;
    if (chunk2 >= 0x800)
        chunk2 -= 0x1000;
    if (chunk2 != 0)
        rv64_buf_emit(buf, rv64_addi(dst, dst, chunk2));

    /* Shift left 8, add bits [7:0] */
    rv64_buf_emit(buf, rv64_slli(dst, dst, 8));
    int32_t chunk3 = lo32 & 0xFF;
    if (chunk3 >= 0x80)
        chunk3 -= 0x100;
    if (chunk3 != 0)
        rv64_buf_emit(buf, rv64_addi(dst, dst, chunk3));
}

/* ========== Spill Writeback ========== */

XR_FUNC void rv64_maybe_spill(Rv64CodegenCtx *ctx, XmRef dst_ref) {
    if (!xm_ref_is_vreg(dst_ref))
        return;
    uint32_t idx = XM_REF_INDEX(dst_ref);
    int16_t slot = xra_vreg_spill(ctx->xra, idx);
    if (slot < 0)
        return;
    int8_t ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    int32_t offset = -(RV64_SPILL_BASE + slot * 8);
    bool is_fp = rv64_is_fp_vreg(ctx, dst_ref);
    if (ri >= 0) {
        if (is_fp)
            rv64_buf_emit(&ctx->buf, rv64_fsd(rv64_alloc_fp_regs[ri], RV64_FP, offset));
        else
            rv64_buf_emit(&ctx->buf, rv64_sd(rv64_alloc_regs[ri], RV64_FP, offset));
    } else {
        if (is_fp)
            rv64_buf_emit(&ctx->buf, rv64_fsd(RV64_SCRATCH_FP, RV64_FP, offset));
        else
            rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_FP, offset));
    }
}

/* ========== Branch Patching ========== */

XR_FUNC void rv64_add_patch(Rv64CodegenCtx *ctx, Rv64PatchType type, uint32_t target_blk,
                            Rv64Cond cc, uint8_t rs1, uint8_t rs2) {
    if (ctx->npatch >= ctx->patches_cap) {
        uint32_t new_cap = ctx->patches_cap * 2;
        XR_REALLOC_OR_ABORT(ctx->patches, new_cap * sizeof(Rv64BranchPatch),
                            "rv64 codegen patches grow");
        ctx->patches_cap = new_cap;
    }
    Rv64BranchPatch *p = &ctx->patches[ctx->npatch++];
    p->emit_idx = ctx->buf.count;
    p->target_blk = target_blk;
    p->type = type;
    p->cc = cc;
    p->rs1 = rs1;
    p->rs2 = rs2;
}

/* ========== Gap Moves ========== */

static void rv64_emit_gap_moves_before(Rv64CodegenCtx *ctx, uint32_t ins_idx) {
    if (!ctx->xra || !ctx->xra->gap_moves)
        return;
    uint32_t blk_id = ctx->cur_blk_id;
    uint32_t cursor = ctx->gap_move_cursor;
    while (cursor < ctx->xra->ngap_move) {
        XraGapMove *gm = &ctx->xra->gap_moves[cursor];
        if (gm->gap_blk > blk_id)
            break;
        if (gm->gap_blk < blk_id) {
            cursor++;
            continue;
        }
        if (gm->gap_ins_idx > ins_idx)
            break;
        if (gm->gap_ins_idx == ins_idx) {
            if (gm->src_reg >= 0 && gm->dst_reg >= 0) {
                /* reg-to-reg move */
                if (gm->is_fp) {
                    Rv64Freg sf = rv64_alloc_fp_regs[gm->src_reg];
                    Rv64Freg df = rv64_alloc_fp_regs[gm->dst_reg];
                    if (sf != df) {
                        /* FMV.D df, sf via FSGNJ.D df, sf, sf */
                        rv64_buf_emit(&ctx->buf, 0x53u | ((uint32_t) df << 7) | (0x0u << 12) |
                                                     ((uint32_t) sf << 15) | ((uint32_t) sf << 20) |
                                                     (0x11u << 25));
                    }
                } else {
                    Rv64Reg sh = rv64_alloc_regs[gm->src_reg];
                    Rv64Reg dh = rv64_alloc_regs[gm->dst_reg];
                    if (sh != dh)
                        rv64_buf_emit(&ctx->buf, rv64_mv(dh, sh));
                }
            } else if (gm->src_reg >= 0 && gm->dst_reg < 0) {
                /* reg-to-spill (store) */
                int32_t offset = -(RV64_SPILL_BASE + gm->spill_slot * 8);
                XR_DCHECK(offset >= -2048, "gap move: spill offset out of range");
                if (gm->is_fp)
                    rv64_buf_emit(&ctx->buf,
                                  rv64_fsd(rv64_alloc_fp_regs[gm->src_reg], RV64_FP, offset));
                else
                    rv64_buf_emit(&ctx->buf,
                                  rv64_sd(rv64_alloc_regs[gm->src_reg], RV64_FP, offset));
            } else if (gm->src_reg < 0 && gm->dst_reg >= 0) {
                /* spill-to-reg (reload) */
                int32_t offset = -(RV64_SPILL_BASE + gm->spill_slot * 8);
                XR_DCHECK(offset >= -2048, "gap move: spill offset out of range");
                if (gm->is_fp)
                    rv64_buf_emit(&ctx->buf,
                                  rv64_fld(rv64_alloc_fp_regs[gm->dst_reg], RV64_FP, offset));
                else
                    rv64_buf_emit(&ctx->buf,
                                  rv64_ld(rv64_alloc_regs[gm->dst_reg], RV64_FP, offset));
            }
            if (ctx->vreg_override && gm->vreg < ctx->xra->nvreg)
                ctx->vreg_override[gm->vreg] = gm->dst_reg;
        }
        cursor++;
    }
    ctx->gap_move_cursor = cursor;
}

/* ========== Prologue / Epilogue ========== */

/* Callee-saved registers pushed in prologue:
 * ra, s0/fp, s1, s2-s9, s10(jit_ctx), s11(coro) = 13 regs
 * Plus FP callee-saved: fs0-fs7 = 8 regs
 * Frame layout:
 *   [high] caller frame
 *          sd ra, [sp+frame_size-8]
 *          sd s0, [sp+frame_size-16]
 *          ... callee-saved GPRs and FPRs ...
 *          [stack_map_ptr]    [fp - 8]
 *          [safepoint_id]     [fp - 16]
 *          ... spill slots ...   [fp - SPILL_BASE - slot*8]
 *   [low]  sp after prologue
 */

#define RV64_NUM_CALLEE_GP 13 /* ra + s0 + s1 + s2-s9 + s10 + s11 */
#define RV64_NUM_CALLEE_FP 8  /* fs0-fs7 */

static void rv64_emit_prologue(Rv64CodegenCtx *ctx) {
    /* ADDI sp, sp, -frame_size (placeholder — patched later).
     * We emit a placeholder that will be overwritten once we know
     * the final frame size. Use the minimum frame base for now. */
    RV64_CODEGEN_CHECK(ctx, ctx->nsub_patches < 16, "too many frame sub patches");
    ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, -(int32_t) RV64_JIT_FRAME_BASE));

    /* Save ra, callee-saved GPRs into the frame (SD rs2, offset(sp)).
     * We save relative to sp; after setting fp=sp+frame_size we can
     * also address via fp with negative offsets. */
    uint32_t off = RV64_JIT_FRAME_BASE - 8;
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_RA, RV64_SP, (int32_t) off));
    off -= 8;
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_FP, RV64_SP, (int32_t) off));
    off -= 8;
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_S1, RV64_SP, (int32_t) off));
    off -= 8;

    /* s2-s9 */
    for (Rv64Reg r = RV64_S2; r <= RV64_S9; r++) {
        rv64_buf_emit(&ctx->buf, rv64_sd(r, RV64_SP, (int32_t) off));
        off -= 8;
    }

    /* s10=jit_ctx, s11=coro — saved as callee-saved even though reserved */
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_S10, RV64_SP, (int32_t) off));
    off -= 8;
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_S11, RV64_SP, (int32_t) off));
    off -= 8;

    /* Save FP callee-saved: fs0-fs7 */
    for (Rv64Freg f = RV64_FS0; f <= RV64_FS7; f++) {
        rv64_buf_emit(&ctx->buf, rv64_fsd(f, RV64_SP, (int32_t) off));
        off -= 8;
    }

    /* Set up frame pointer: fp = sp + frame_size.
     * Placeholder — patched later alongside the SP adjustment. */
    RV64_CODEGEN_CHECK(ctx, ctx->nadd_patches < 8, "too many frame add patches");
    ctx->frame_patch_add[ctx->nadd_patches++] = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_FP, RV64_SP, (int32_t) RV64_JIT_FRAME_BASE));

    /* Copy coro pointer from a0 (first ABI arg) */
    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_CORO_REG, RV64_A0));

    /* Load jit_ctx from coro */
    rv64_buf_emit(&ctx->buf,
                  rv64_ld(RV64_JIT_CTX_REG, RV64_CORO_REG, (int32_t) XM_CORO_JIT_CTX_OFFSET));

    /* Save stack_map_ptr from jit_ctx into frame for GC */
    rv64_buf_emit(&ctx->buf,
                  rv64_ld(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_OFFSET));
    rv64_buf_emit(&ctx->buf,
                  rv64_sd(RV64_SCRATCH_REG, RV64_FP, -(int32_t) RV64_FRAME_SMAP_PTR_OFFSET));

    /* Initialize safepoint_id = UINT32_MAX */
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, 0xFFFFFFFF);
    rv64_buf_emit(&ctx->buf,
                  rv64_sw(RV64_SCRATCH_REG, RV64_FP, -(int32_t) RV64_FRAME_SMAP_ID_OFFSET));

    /* Load params from args array (a1 = args pointer).
     * Two-pass: first load params that don't clobber a1,
     * then load the one that does. */
    uint32_t nparams = ctx->func->num_params;
    if (nparams > 0 && ctx->xra) {
        uint32_t entry_id = ctx->func->blocks[0]->id;
        int32_t entry_pos = (entry_id < ctx->xra->nblk) ? ctx->xra->blk_start[entry_id] : 0;
        int32_t deferred = -1;
        for (uint32_t i = 0; i < nparams; i++) {
            bool is_fp = (i < ctx->func->nvreg && ctx->func->vregs[i].rep == XR_REP_F64);
            int8_t ri = xra_reg_at_pos(ctx->xra, i, entry_pos);
            if (ri < 0) {
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0) {
                    int32_t soff = -(RV64_SPILL_BASE + slot * 8);
                    if (is_fp) {
                        rv64_buf_emit(&ctx->buf,
                                      rv64_fld(RV64_SCRATCH_FP, RV64_A1, (int32_t) (i * 8)));
                        rv64_buf_emit(&ctx->buf, rv64_fsd(RV64_SCRATCH_FP, RV64_FP, soff));
                    } else {
                        rv64_buf_emit(&ctx->buf,
                                      rv64_ld(RV64_SCRATCH_REG, RV64_A1, (int32_t) (i * 8)));
                        rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_FP, soff));
                    }
                }
                continue;
            }
            if (!is_fp && rv64_alloc_regs[ri] == RV64_A1) {
                deferred = (int32_t) i;
                continue;
            }
            if (is_fp) {
                Rv64Freg dst = rv64_alloc_fp_regs[ri];
                rv64_buf_emit(&ctx->buf, rv64_fld(dst, RV64_A1, (int32_t) (i * 8)));
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0)
                    rv64_buf_emit(&ctx->buf, rv64_fsd(dst, RV64_FP, -(RV64_SPILL_BASE + slot * 8)));
            } else {
                Rv64Reg dst = rv64_alloc_regs[ri];
                rv64_buf_emit(&ctx->buf, rv64_ld(dst, RV64_A1, (int32_t) (i * 8)));
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0)
                    rv64_buf_emit(&ctx->buf, rv64_sd(dst, RV64_FP, -(RV64_SPILL_BASE + slot * 8)));
            }
        }
        /* Deferred param (clobbers a1) */
        if (deferred >= 0) {
            uint32_t i = (uint32_t) deferred;
            int8_t ri = xra_reg_at_pos(ctx->xra, i, entry_pos);
            Rv64Reg dst = rv64_alloc_regs[ri];
            rv64_buf_emit(&ctx->buf, rv64_ld(dst, RV64_A1, (int32_t) (i * 8)));
            int16_t slot = xra_vreg_spill(ctx->xra, i);
            if (slot >= 0)
                rv64_buf_emit(&ctx->buf, rv64_sd(dst, RV64_FP, -(RV64_SPILL_BASE + slot * 8)));
        }
    }

    /* Init vreg_runtime_tags for TAGGED params */
    for (uint32_t i = 0; i < nparams && i < 8; i++) {
        if (i >= ctx->func->nvreg || i >= XR_JIT_MAX_VREG_TAGS)
            continue;
        if (ctx->func->vregs[i].rep != XR_REP_TAGGED)
            continue;
        int32_t pt_off = (int32_t) (XM_JIT_PARAM_TAGS_OFFSET + i * 8);
        int32_t rt_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) i;
        rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, pt_off));
        rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, rt_off));
    }
}

XR_FUNC void rv64_emit_epilogue(Rv64CodegenCtx *ctx) {
    /* Restore callee-saved registers (reverse order of saves).
     * We saved them at frame_size-8, frame_size-16, ... from sp.
     * Since fp = sp + frame_size, we can restore using negative offsets from fp. */
    uint32_t off = RV64_JIT_FRAME_BASE - 8;

    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_RA, RV64_SP, (int32_t) off));
    off -= 8;
    /* Don't restore FP yet — we need it to compute SP */
    uint32_t fp_save_off = off;
    off -= 8;
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_S1, RV64_SP, (int32_t) off));
    off -= 8;
    for (Rv64Reg r = RV64_S2; r <= RV64_S9; r++) {
        rv64_buf_emit(&ctx->buf, rv64_ld(r, RV64_SP, (int32_t) off));
        off -= 8;
    }
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_S10, RV64_SP, (int32_t) off));
    off -= 8;
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_S11, RV64_SP, (int32_t) off));
    off -= 8;
    /* Restore FP callee-saved: fs0-fs7 */
    for (Rv64Freg f = RV64_FS0; f <= RV64_FS7; f++) {
        rv64_buf_emit(&ctx->buf, rv64_fld(f, RV64_SP, (int32_t) off));
        off -= 8;
    }
    /* Restore fp last */
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_FP, RV64_SP, (int32_t) fp_save_off));

    /* Restore sp: ADDI sp, sp, frame_size (placeholder — patched) */
    RV64_CODEGEN_CHECK(ctx, ctx->nadd_patches < 8, "too many frame add patches (epilogue)");
    ctx->frame_patch_add[ctx->nadd_patches++] = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, (int32_t) RV64_JIT_FRAME_BASE));

    /* Return */
    rv64_buf_emit(&ctx->buf, rv64_ret());
}

/* ========== Edge Copies ========== */

/* Minimal edge-copy emission: handles reg-to-reg and reg-to-spill transitions
 * at block boundaries. Full implementation with cycle-breaking will follow. */
static void rv64_emit_edge_copies(Rv64CodegenCtx *ctx, XmBlock *target, XmBlock *from) {
    if (!ctx->xra)
        return;
    uint32_t nedge = 0;
    XraEdgeCopy copies[64];
    xra_edge_copies(ctx->xra, from->id, target->id, copies, 64, &nedge);

    /* Stores first (to free registers), then normal moves, then reloads */
    for (uint32_t i = 0; i < nedge; i++) {
        XraEdgeCopy *ec = &copies[i];
        if (!ec->is_store)
            continue;
        int32_t offset = -(RV64_SPILL_BASE + ec->spill_slot * 8);
        if (ec->is_fp)
            rv64_buf_emit(&ctx->buf, rv64_fsd(rv64_alloc_fp_regs[ec->src_reg], RV64_FP, offset));
        else
            rv64_buf_emit(&ctx->buf, rv64_sd(rv64_alloc_regs[ec->src_reg], RV64_FP, offset));
    }
    for (uint32_t i = 0; i < nedge; i++) {
        XraEdgeCopy *ec = &copies[i];
        if (ec->is_store || ec->is_reload)
            continue;
        if (ec->is_fp) {
            Rv64Freg sf = rv64_alloc_fp_regs[ec->src_reg];
            Rv64Freg df = rv64_alloc_fp_regs[ec->dst_reg];
            if (sf != df)
                rv64_buf_emit(&ctx->buf, 0x53u | ((uint32_t) df << 7) | (0x0u << 12) |
                                             ((uint32_t) sf << 15) | ((uint32_t) sf << 20) |
                                             (0x11u << 25));
        } else {
            Rv64Reg sh = rv64_alloc_regs[ec->src_reg];
            Rv64Reg dh = rv64_alloc_regs[ec->dst_reg];
            if (sh != dh)
                rv64_buf_emit(&ctx->buf, rv64_mv(dh, sh));
        }
    }
    for (uint32_t i = 0; i < nedge; i++) {
        XraEdgeCopy *ec = &copies[i];
        if (!ec->is_reload)
            continue;
        int32_t offset = -(RV64_SPILL_BASE + ec->spill_slot * 8);
        if (ec->is_fp)
            rv64_buf_emit(&ctx->buf, rv64_fld(rv64_alloc_fp_regs[ec->dst_reg], RV64_FP, offset));
        else
            rv64_buf_emit(&ctx->buf, rv64_ld(rv64_alloc_regs[ec->dst_reg], RV64_FP, offset));
    }
}

/* ========== Block Emission ========== */

static void rv64_emit_block(Rv64CodegenCtx *ctx, uint32_t block_idx) {
    XmBlock *blk = ctx->func->blocks[block_idx];
    XR_DCHECK(blk != NULL, "rv64_emit_block: NULL block");

    ctx->cur_blk_id = blk->id;
    ctx->cur_ra_pos = (ctx->xra && blk->id < ctx->xra->nblk) ? ctx->xra->blk_start[blk->id] : 0;

    if (ctx->vreg_override && ctx->xra)
        memset(ctx->vreg_override, -128, ctx->xra->nvreg);

    if (ctx->xra && ctx->xra->gap_moves) {
        while (ctx->gap_move_cursor < ctx->xra->ngap_move &&
               ctx->xra->gap_moves[ctx->gap_move_cursor].gap_blk < blk->id)
            ctx->gap_move_cursor++;
    }

    /* Record block start offset (instruction index) */
    ctx->block_offsets[blk->id] = ctx->buf.count;

    /* Snapshot loop headers for OSR */
    if (blk->is_loop_header && ctx->nosr_snap < XM_MAX_OSR_ENTRIES && !ctx->func->has_coro_deopt) {
        ctx->osr_snaps[ctx->nosr_snap].block_id = blk->id;
        ctx->osr_snaps[ctx->nosr_snap].block_offset = ctx->buf.count;
        ctx->nosr_snap++;
    }

    /* Emit all instructions */
    for (uint32_t i = 0; i < blk->nins; i++) {
        ctx->cur_ra_pos = (ctx->xra && blk->id < ctx->xra->nblk)
                              ? ctx->xra->blk_start[blk->id] + 2 + (int32_t) i * 2
                              : 0;
        rv64_emit_gap_moves_before(ctx, i);
        ctx->cur_ins_idx = i;
        rv64_emit_xm_ins(ctx, &blk->ins[i]);
        rv64_maybe_spill(ctx, blk->ins[i].dst);
    }

    /* Emit terminator */
    switch (blk->jmp.type) {
        case XM_JMP_JMP: {
            RV64_CODEGEN_CHECK(ctx, blk->s1 != NULL, "JMP: no s1");
            rv64_emit_edge_copies(ctx, blk->s1, blk);
            bool is_next =
                (block_idx + 1 < ctx->func->nblk) && (ctx->func->blocks[block_idx + 1] == blk->s1);
            if (!is_next) {
                rv64_add_patch(ctx, RV64_PATCH_JAL, blk->s1->id, RV64_CC_EQ, 0, 0);
                rv64_buf_emit(&ctx->buf, rv64_j(0)); /* placeholder */
            }
            break;
        }
        case XM_JMP_BR: {
            RV64_CODEGEN_CHECK(ctx, blk->s1 != NULL && blk->s2 != NULL, "BR: no s1/s2");
            Rv64Reg cond = rv64_get_reg(ctx, blk->jmp.arg);

            bool s1_next =
                (block_idx + 1 < ctx->func->nblk) && (ctx->func->blocks[block_idx + 1] == blk->s1);

            if (s1_next) {
                /* BNE cond,x0 → skip over false path → fall through to s1 (true) */
                /* First emit false path (s2): edge copies + jump */
                uint32_t skip_idx = ctx->buf.count;
                rv64_buf_emit(&ctx->buf, rv64_bne(cond, RV64_X0, 0)); /* placeholder */

                rv64_emit_edge_copies(ctx, blk->s2, blk);
                rv64_add_patch(ctx, RV64_PATCH_JAL, blk->s2->id, RV64_CC_EQ, 0, 0);
                rv64_buf_emit(&ctx->buf, rv64_j(0));

                /* Patch skip branch to here */
                int32_t skip_off = (int32_t) (ctx->buf.count - skip_idx) * 4;
                ctx->buf.code[skip_idx] = rv64_bne(cond, RV64_X0, skip_off);

                rv64_emit_edge_copies(ctx, blk->s1, blk);
            } else {
                /* BEQ cond,x0 → skip over true path → fall or jump to s2 (false) */
                uint32_t skip_idx = ctx->buf.count;
                rv64_buf_emit(&ctx->buf, rv64_beq(cond, RV64_X0, 0)); /* placeholder */

                rv64_emit_edge_copies(ctx, blk->s1, blk);
                rv64_add_patch(ctx, RV64_PATCH_JAL, blk->s1->id, RV64_CC_EQ, 0, 0);
                rv64_buf_emit(&ctx->buf, rv64_j(0));

                /* Patch skip branch */
                int32_t skip_off = (int32_t) (ctx->buf.count - skip_idx) * 4;
                ctx->buf.code[skip_idx] = rv64_beq(cond, RV64_X0, skip_off);

                rv64_emit_edge_copies(ctx, blk->s2, blk);
                bool s2_next = (block_idx + 1 < ctx->func->nblk) &&
                               (ctx->func->blocks[block_idx + 1] == blk->s2);
                if (!s2_next) {
                    rv64_add_patch(ctx, RV64_PATCH_JAL, blk->s2->id, RV64_CC_EQ, 0, 0);
                    rv64_buf_emit(&ctx->buf, rv64_j(0));
                }
            }
            break;
        }
        case XM_JMP_RET: {
            /* Move return value to a0 */
            if (!xm_ref_is_none(blk->jmp.arg)) {
                uint32_t ret_idx = XM_REF_INDEX(blk->jmp.arg);
                bool is_fp = (xm_ref_is_vreg(blk->jmp.arg) && ret_idx < ctx->func->nvreg &&
                              ctx->func->vregs[ret_idx].rep == XR_REP_F64);
                if (is_fp) {
                    Rv64Freg fsrc = rv64_get_fp_reg(ctx, blk->jmp.arg);
                    rv64_buf_emit(&ctx->buf, rv64_fmv_x_d(RV64_A0, fsrc));
                } else {
                    Rv64Reg ret_reg = rv64_get_operand(ctx, blk->jmp.arg, RV64_A0);
                    if (ret_reg != RV64_A0)
                        rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A0, ret_reg));
                }

                /* Return type tag in a1 (RISC-V uses a1 for tag, analogous to x64's RCX) */
                uint8_t ret_xr_tag = 3; /* XR_TAG_I64 default */
                if (is_fp)
                    ret_xr_tag = 4; /* XR_TAG_F64 */
                else if (xm_ref_is_vreg(blk->jmp.arg) && ret_idx < ctx->func->nvreg) {
                    uint8_t rep = ctx->func->vregs[ret_idx].rep;
                    if (rep == XR_REP_F64)
                        ret_xr_tag = 4;
                    else if (rep == XR_REP_PTR)
                        ret_xr_tag = 5;
                }
                rv64_buf_emit(&ctx->buf, rv64_li(RV64_A1, (int32_t) ret_xr_tag));
            }
            rv64_emit_epilogue(ctx);
            break;
        }
        default:
            XR_DCHECK(false, "rv64_emit_terminator: unhandled jump type");
            break;
    }
}

/* ========== Branch Patch Resolution ========== */

static void rv64_patch_branches(Rv64CodegenCtx *ctx) {
    for (uint32_t i = 0; i < ctx->npatch; i++) {
        Rv64BranchPatch *p = &ctx->patches[i];
        uint32_t target_idx = 0;
        int32_t byte_offset = 0;

        switch (p->type) {
            case RV64_PATCH_JAL:
                RV64_CODEGEN_CHECK(ctx, p->target_blk < ctx->nblock_offsets,
                                   "patch JAL: target block OOB");
                target_idx = ctx->block_offsets[p->target_blk];
                byte_offset = (int32_t) (target_idx - p->emit_idx) * 4;
                ctx->buf.code[p->emit_idx] = rv64_j(byte_offset);
                break;
            case RV64_PATCH_BRANCH:
                RV64_CODEGEN_CHECK(ctx, p->target_blk < ctx->nblock_offsets,
                                   "patch branch: target block OOB");
                target_idx = ctx->block_offsets[p->target_blk];
                byte_offset = (int32_t) (target_idx - p->emit_idx) * 4;
                switch (p->cc) {
                    case RV64_CC_EQ:
                        ctx->buf.code[p->emit_idx] = rv64_beq(p->rs1, p->rs2, byte_offset);
                        break;
                    case RV64_CC_NE:
                        ctx->buf.code[p->emit_idx] = rv64_bne(p->rs1, p->rs2, byte_offset);
                        break;
                    case RV64_CC_LT:
                        ctx->buf.code[p->emit_idx] = rv64_blt(p->rs1, p->rs2, byte_offset);
                        break;
                    case RV64_CC_GE:
                        ctx->buf.code[p->emit_idx] = rv64_bge(p->rs1, p->rs2, byte_offset);
                        break;
                    default:
                        XR_DCHECK(false, "rv64_patch: unhandled branch cc");
                        break;
                }
                break;
            case RV64_PATCH_CALL_C:
                byte_offset = (int32_t) (ctx->call_c_stub - p->emit_idx) * 4;
                ctx->buf.code[p->emit_idx] = rv64_call(byte_offset);
                break;
            case RV64_PATCH_DEOPT_JAL:
                byte_offset = (int32_t) (ctx->deopt_stub - p->emit_idx) * 4;
                ctx->buf.code[p->emit_idx] = rv64_j(byte_offset);
                break;
            case RV64_PATCH_DEOPT_BRANCH:
                byte_offset = (int32_t) (ctx->deopt_stub - p->emit_idx) * 4;
                switch (p->cc) {
                    case RV64_CC_NE:
                        ctx->buf.code[p->emit_idx] = rv64_bne(p->rs1, p->rs2, byte_offset);
                        break;
                    case RV64_CC_EQ:
                        ctx->buf.code[p->emit_idx] = rv64_beq(p->rs1, p->rs2, byte_offset);
                        break;
                    default:
                        ctx->buf.code[p->emit_idx] = rv64_bne(p->rs1, p->rs2, byte_offset);
                        break;
                }
                break;
            case RV64_PATCH_BARRIER_FWD:
                byte_offset = (int32_t) (ctx->barrier_fwd_stub - p->emit_idx) * 4;
                ctx->buf.code[p->emit_idx] = rv64_call(byte_offset);
                break;
            case RV64_PATCH_BARRIER_BACK:
                byte_offset = (int32_t) (ctx->barrier_back_stub - p->emit_idx) * 4;
                ctx->buf.code[p->emit_idx] = rv64_call(byte_offset);
                break;
            case RV64_PATCH_CALL_SELF:
                /* Self-call patches target the fast_entry_offset.
                 * byte_offset = (fast_entry_offset_in_inst - emit_idx) * 4 */
                target_idx = ctx->fast_entry_offset / 4;
                byte_offset = (int32_t) (target_idx - p->emit_idx) * 4;
                ctx->buf.code[p->emit_idx] = rv64_call(byte_offset);
                break;
        }
    }
}

/* ========== Main Codegen Entry ========== */

XR_FUNC XmCodegenResult xm_codegen_riscv64(XmFunc *func, XmCodeAlloc *alloc) {
    XR_DCHECK(func != NULL, "xm_codegen_riscv64: func is NULL");
    XR_DCHECK(alloc != NULL, "xm_codegen_riscv64: alloc is NULL");
    XmCodegenResult result = {
        .code = NULL,
        .code_size = 0,
        .success = false,
        .error = NULL,
        .nosr = 0,
        .ndeopt = 0,
        .stack_map = NULL,
        .fast_entry_offset = 0,
        .resume_entry_offset = 0,
    };

    if (!func || !alloc || func->nblk == 0) {
        result.error = "invalid function or allocator";
        return result;
    }
    if (func->nvreg > RV64_MAX_VREGS) {
        result.error = "too many virtual registers";
        return result;
    }

    /* Rebuild vreg def pointers */
    for (uint32_t v = 0; v < func->nvreg; v++)
        func->vregs[v].def = NULL;
    for (uint32_t b = 0; b < func->nblk; b++) {
        XmBlock *blk = func->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < func->nvreg)
                    func->vregs[vi].def = ins;
            }
        }
    }

    Rv64CodegenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = func;
    ctx.alloc = alloc;
    ctx.patches_cap = RV64_INIT_PATCHES;
    XR_MALLOC_OR_ABORT(ctx.patches, RV64_INIT_PATCHES * sizeof(Rv64BranchPatch),
                       "rv64 codegen patches init");

    if (setjmp(ctx.bail_jmp) != 0) {
        result.error = ctx.error_reason ? ctx.error_reason : "codegen invariant violation";
        goto cleanup;
    }

    /* Pre-RA: MOV coalescing */
    xm_coalesce(func);

    /* Register allocation */
    ctx.xra = xra_run(func);
    if (ctx.xra && ctx.xra->had_error) {
        result.error = "regalloc refused: spill slot limit exceeded";
        goto cleanup;
    }

    /* Gap-move override array */
    if (ctx.xra && ctx.xra->nvreg > 0) {
        XR_MALLOC_OR_ABORT(ctx.vreg_override, ctx.xra->nvreg * sizeof(int8_t),
                           "rv64 codegen vreg_override init");
        memset(ctx.vreg_override, -128, ctx.xra->nvreg);
    }

    /* Block offsets array */
    uint32_t max_blk_id = 0;
    for (uint32_t i = 0; i < func->nblk; i++) {
        if (func->blocks[i]->id > max_blk_id)
            max_blk_id = func->blocks[i]->id;
    }
    XR_CALLOC_OR_ABORT(ctx.block_offsets, max_blk_id + 1, sizeof(uint32_t),
                       "rv64 codegen block_offsets init");
    ctx.nblock_offsets = max_blk_id + 1;

    /* Allocate code buffer: RISC-V fixed-width 32-bit instructions.
     * Budget: ~24 instructions per Xm instruction (generous for 3-operand ISA)
     * + 512 instructions for prologue/epilogue/stubs overhead. */
    uint32_t total_xm_ins = 0;
    for (uint32_t i = 0; i < func->nblk; i++)
        total_xm_ins += func->blocks[i]->nins + 4;
    uint32_t ninst = total_xm_ins * 24 + 512;
    uint32_t alloc_size = ninst * 4;
    alloc_size = (alloc_size + 4095) & ~(uint32_t) 4095;
    if (alloc_size < 8192)
        alloc_size = 8192;

    void *code_mem = xm_code_alloc(alloc, alloc_size, 16);
    if (!code_mem) {
        result.error = "failed to allocate executable memory";
        goto cleanup;
    }

#ifdef XR_OS_MACOS
    xm_code_make_writable(code_mem, alloc_size);
#endif

    rv64_buf_init(&ctx.buf, (uint32_t *) code_mem, alloc_size / 4);

    /* Emit prologue */
    rv64_emit_prologue(&ctx);

    /* Emit all blocks */
    for (uint32_t i = 0; i < func->nblk; i++)
        rv64_emit_block(&ctx, i);

    /* Emit stubs (after all blocks, before patching) */
    rv64_emit_deopt_stub(&ctx);
    rv64_emit_call_c_stub(&ctx);
    rv64_emit_barrier_stubs(&ctx);

    /* Check for buffer overflow before patching */
    if (ctx.buf.count >= ctx.buf.capacity) {
        result.error = "rv64 codegen: code buffer overflow";
        goto cleanup;
    }

    /* Patch branches */
    rv64_patch_branches(&ctx);

    /* Patch frame size: compute actual frame size and fix prologue/epilogue.
     * frame_size = frame_base + spill_area, 16-byte aligned.
     * Stack at entry: retaddr on stack conceptually (RISC-V saves ra explicitly).
     * After prologue: sp = old_sp - frame_size, fp = old_sp.
     * Alignment: frame_size must be 16-byte aligned. */
    uint32_t frame_size =
        (RV64_JIT_FRAME_BASE + (ctx.xra ? ctx.xra->nspill * 8 : 0) + 15) & ~(uint32_t) 15;

    /* Patch SUB sp instructions (ADDI sp, sp, -frame_size) */
    for (uint32_t i = 0; i < ctx.nsub_patches; i++) {
        uint32_t idx = ctx.frame_patch_sub[i];
        ctx.buf.code[idx] = rv64_addi(RV64_SP, RV64_SP, -(int32_t) frame_size);
    }
    /* Patch ADD sp/fp instructions */
    for (uint32_t i = 0; i < ctx.nadd_patches; i++) {
        uint32_t idx = ctx.frame_patch_add[i];
        /* Determine if this is fp setup (ADDI fp, sp, frame_size)
         * or sp restore (ADDI sp, sp, frame_size) by checking rd field. */
        uint32_t rd_field = (ctx.buf.code[idx] >> 7) & 0x1F;
        if (rd_field == RV64_FP)
            ctx.buf.code[idx] = rv64_addi(RV64_FP, RV64_SP, (int32_t) frame_size);
        else
            ctx.buf.code[idx] = rv64_addi(RV64_SP, RV64_SP, (int32_t) frame_size);
    }

    uint32_t code_size = rv64_buf_offset(&ctx.buf);

    xm_code_make_executable(code_mem, code_size);
    /* RISC-V requires FENCE.I for I-cache coherence after code generation */
    __builtin___clear_cache(code_mem, (char *) code_mem + code_size);

    if (ctx.had_error) {
        result.error = "rv64 codegen: unsupported opcode or regalloc error";
        goto cleanup;
    }

    result.code = code_mem;
    result.code_size = code_size;
    result.fast_entry_offset = ctx.fast_entry_offset;
    result.success = true;

cleanup:
    xr_free(ctx.block_offsets);
    xr_free(ctx.patches);
    xr_free(ctx.vreg_override);
    if (ctx.xra)
        xra_result_free(ctx.xra);
    return result;
}

#endif /* __riscv */
