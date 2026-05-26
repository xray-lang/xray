/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_riscv64_ins.c - Per-instruction RISC-V 64 code emission
 *
 * Contains rv64_emit_xm_ins() and handler functions for translating
 * Xm SSA instructions into RV64GD machine code.
 *
 * RISC-V advantages over x86-64 for JIT:
 *   - 3-operand ISA: no destructive 2-op constraint, no commutative dance
 *   - Hardware DIV/REM: no RAX:RDX, no IDIV trap on div-by-zero
 *   - Variable shifts: no CL register constraint
 *   - FP comparisons: feq/flt/fle write GPR directly, no flags
 *   - Hardware FNEG: fsgnjn.d (no XOR sign-bit hack)
 */

#ifdef __riscv

#include "xm_codegen_riscv64_internal.h"
#include "xm_jit_runtime.h"
#include "../coro/xcoroutine.h"
#include <string.h>

/* ========== Handler type ========== */
typedef void (*Rv64InsHandler)(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd);

/* ========== Constants ========== */

static void rv64_h_const(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    if (xm_ref_is_const(ins->args[0])) {
        uint32_t ci = XM_REF_INDEX(ins->args[0]);
        RV64_CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "const index OOB");
        rv64_load_imm64(&ctx->buf, rd, ctx->func->consts[ci].val.raw);
    }
}

static void rv64_h_const_f64(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    if (xm_ref_is_const(ins->args[0])) {
        uint32_t ci = XM_REF_INDEX(ins->args[0]);
        RV64_CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "CONST_F64: const OOB");
        uint64_t raw = ctx->func->consts[ci].val.raw;
        if (raw == 0) {
            /* fcvt.d.l fd, x0 — convert integer 0 to double 0.0 */
            rv64_buf_emit(&ctx->buf, rv64_fcvt_d_l(fd, RV64_X0));
        } else {
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, raw);
            rv64_buf_emit(&ctx->buf, rv64_fmv_d_x(fd, RV64_SCRATCH_REG));
        }
    }
}

/* ========== Integer Arithmetic ========== */

/* RISC-V 3-operand: rd = rs1 OP rs2.  No commutative dance needed. */

static void rv64_h_add(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_add(rd, rs1, rs2));
}

static void rv64_h_sub(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_sub(rd, rs1, rs2));
}

static void rv64_h_mul(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_mul(rd, rs1, rs2));
}

/* RISC-V DIV: no trap on div-by-zero (returns -1) or INT64_MIN/-1 (returns INT64_MIN).
 * This matches Xray's wrap semantics, so no guard needed (unlike x64 IDIV). */
static void rv64_h_div(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_div(rd, rs1, rs2));
}

/* RISC-V REM: same no-trap semantics as DIV. */
static void rv64_h_mod(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_rem(rd, rs1, rs2));
}

static void rv64_h_neg(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    rv64_buf_emit(&ctx->buf, rv64_sub(rd, RV64_X0, rs));
}

/* ========== Bitwise Logic ========== */

static void rv64_h_and(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_and(rd, rs1, rs2));
}

static void rv64_h_or(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_or(rd, rs1, rs2));
}

static void rv64_h_xor(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_xor(rd, rs1, rs2));
}

static void rv64_h_not(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    /* NOT = XORI rd, rs, -1 */
    rv64_buf_emit(&ctx->buf, rv64_xori(rd, rs, -1));
}

/* ========== Shifts ========== */

/* RISC-V: shift amount in rs2[5:0], no CL register constraint. */

static void rv64_h_shl(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_sll(rd, rs1, rs2));
}

static void rv64_h_shr(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    rv64_buf_emit(&ctx->buf, rv64_sra(rd, rs1, rs2));
}

/* ========== Integer Comparison ========== */

/* RISC-V has no flags register. Comparisons produce 0/1 in a GPR via
 * SLT/SLTU or multi-instruction sequences. */

static void rv64_h_cmp_int(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);

    switch (ins->op) {
        case XM_LT:
            /* rd = (rs1 < rs2) ? 1 : 0 */
            rv64_buf_emit(&ctx->buf, rv64_slt(rd, rs1, rs2));
            break;
        case XM_GE:
            /* rd = !(rs1 < rs2) = (rs1 >= rs2)
             * slt tmp, rs1, rs2; xori rd, tmp, 1 */
            rv64_buf_emit(&ctx->buf, rv64_slt(rd, rs1, rs2));
            rv64_buf_emit(&ctx->buf, rv64_xori(rd, rd, 1));
            break;
        case XM_GT:
            /* rd = (rs2 < rs1) — swap operands */
            rv64_buf_emit(&ctx->buf, rv64_slt(rd, rs2, rs1));
            break;
        case XM_LE:
            /* rd = !(rs2 < rs1) = (rs1 <= rs2)
             * slt tmp, rs2, rs1; xori rd, tmp, 1 */
            rv64_buf_emit(&ctx->buf, rv64_slt(rd, rs2, rs1));
            rv64_buf_emit(&ctx->buf, rv64_xori(rd, rd, 1));
            break;
        case XM_EQ:
            /* rd = (rs1 == rs2): sub tmp, rs1, rs2; sltiu rd, tmp, 1 */
            rv64_buf_emit(&ctx->buf, rv64_sub(RV64_SCRATCH_REG, rs1, rs2));
            rv64_buf_emit(&ctx->buf, rv64_sltiu(rd, RV64_SCRATCH_REG, 1));
            break;
        case XM_NE:
            /* rd = (rs1 != rs2): sub tmp, rs1, rs2; sltu rd, x0, tmp */
            rv64_buf_emit(&ctx->buf, rv64_sub(RV64_SCRATCH_REG, rs1, rs2));
            rv64_buf_emit(&ctx->buf, rv64_sltu(rd, RV64_X0, RV64_SCRATCH_REG));
            break;
        default:
            XR_DCHECK(false, "rv64_h_cmp_int: unreachable opcode");
            break;
    }
}

/* ========== Float Arithmetic ========== */

static void rv64_h_fadd(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);
    rv64_buf_emit(&ctx->buf, rv64_fadd_d(fd, fs1, fs2));
}

static void rv64_h_fsub(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);
    rv64_buf_emit(&ctx->buf, rv64_fsub_d(fd, fs1, fs2));
}

static void rv64_h_fmul(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);
    rv64_buf_emit(&ctx->buf, rv64_fmul_d(fd, fs1, fs2));
}

static void rv64_h_fdiv(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);
    rv64_buf_emit(&ctx->buf, rv64_fdiv_d(fd, fs1, fs2));
}

static void rv64_h_fneg(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    /* FNEG.D = FSGNJN.D rd, rs, rs (hardware instruction, no XOR hack) */
    rv64_buf_emit(&ctx->buf, rv64_fneg_d(fd, fs));
}

/* ========== Float Conversion ========== */

static void rv64_h_i2f(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Reg rs = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    rv64_buf_emit(&ctx->buf, rv64_fcvt_d_l(fd, rs));
}

static void rv64_h_f2i(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Freg fs = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    rv64_buf_emit(&ctx->buf, rv64_fcvt_l_d(rd, fs));
}

/* ========== Float Comparison ========== */

/* RISC-V FP comparisons write 0/1 directly to a GPR — no flags, no parity. */

static void rv64_h_cmp_float(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);

    switch (ins->op) {
        case XM_FEQ:
            rv64_buf_emit(&ctx->buf, rv64_feq_d(rd, fs1, fs2));
            break;
        case XM_FNE:
            /* !(fs1 == fs2): feq.d tmp, fs1, fs2; xori rd, tmp, 1 */
            rv64_buf_emit(&ctx->buf, rv64_feq_d(rd, fs1, fs2));
            rv64_buf_emit(&ctx->buf, rv64_xori(rd, rd, 1));
            break;
        case XM_FLT:
            rv64_buf_emit(&ctx->buf, rv64_flt_d(rd, fs1, fs2));
            break;
        case XM_FLE:
            rv64_buf_emit(&ctx->buf, rv64_fle_d(rd, fs1, fs2));
            break;
        default:
            XR_DCHECK(false, "rv64_h_cmp_float: unreachable opcode");
            break;
    }
}

/* ========== Move / Redefine ========== */

static void rv64_h_mov(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    if (rv64_is_fp_vreg(ctx, ins->dst)) {
        Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
        Rv64Freg fs = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
        if (fs != fd) {
            /* FMV.D fd, fs  via FSGNJ.D fd, fs, fs */
            rv64_buf_emit(&ctx->buf, 0x53u | ((uint32_t) fd << 7) | (0x0u << 12) |
                                         ((uint32_t) fs << 15) | ((uint32_t) fs << 20) |
                                         (0x11u << 25));
        }
    } else {
        Rv64Reg rs = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
        if (rs != rd)
            rv64_buf_emit(&ctx->buf, rv64_mv(rd, rs));
    }
}

/* ========== Select ========== */

static void rv64_h_select_cond(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    /* Stash condition register for the following XM_SELECT.
     * RISC-V has no flags, so we just make sure the cond vreg is accessible. */
    (void) rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
}

static void rv64_h_select(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    /* Branch-around pattern: BNE cond, x0, +8; MV rd, false_val; else MV rd, true_val
     * But since we don't have CMOV, use a small branch sequence. */
    Rv64Reg true_reg = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg false_reg = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);

    /* Default: rd = false_val */
    if (false_reg != rd)
        rv64_buf_emit(&ctx->buf, rv64_mv(rd, false_reg));

    /* BNE cond, x0, +8 — skip next instruction if cond != 0 */
    /* The condition vreg was prepared by SELECT_COND, use SCRATCH_REG as fallback */
    rv64_buf_emit(&ctx->buf, rv64_beq(RV64_SCRATCH_REG, RV64_X0, 8));

    /* rd = true_val (only reached if cond != 0) */
    if (true_reg != rd)
        rv64_buf_emit(&ctx->buf, rv64_mv(rd, true_reg));
    else
        rv64_buf_emit(&ctx->buf, rv64_nop());
}

/* ========== Load / Store ========== */

static void rv64_h_load(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    if (ins->rep == XR_REP_F64) {
        Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
        rv64_buf_emit(&ctx->buf, rv64_fld(fd, base, 0));
    } else {
        rv64_buf_emit(&ctx->buf, rv64_ld(rd, base, 0));
    }
}

static void rv64_h_store(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    bool store_fp = false;
    if (xm_ref_is_vreg(ins->args[1])) {
        uint32_t vi = XM_REF_INDEX(ins->args[1]);
        if (vi < ctx->func->nvreg)
            store_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
    }
    if (store_fp) {
        Rv64Freg fs = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT11);
        rv64_buf_emit(&ctx->buf, rv64_fsd(fs, base, 0));
    } else {
        Rv64Reg val = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
        rv64_buf_emit(&ctx->buf, rv64_sd(val, base, 0));
    }
}

/* ========== Sub-word Load/Store ========== */

static void rv64_h_subword(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    switch (ins->op) {
        case XM_LOAD8Z: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_lbu(rd, base, 0));
            break;
        }
        case XM_LOAD8S: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_lb(rd, base, 0));
            break;
        }
        case XM_STORE8: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            Rv64Reg val = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
            rv64_buf_emit(&ctx->buf, rv64_sb(val, base, 0));
            break;
        }
        case XM_LOAD16Z: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_lhu(rd, base, 0));
            break;
        }
        case XM_LOAD16S: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_lh(rd, base, 0));
            break;
        }
        case XM_STORE16: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            Rv64Reg val = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
            rv64_buf_emit(&ctx->buf, rv64_sh(val, base, 0));
            break;
        }
        case XM_LOAD32Z: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_lwu(rd, base, 0));
            break;
        }
        case XM_LOAD32S: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            int32_t offset = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            rv64_buf_emit(&ctx->buf, rv64_lw(rd, base, offset));
            break;
        }
        case XM_STORE32: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            Rv64Reg val = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
            rv64_buf_emit(&ctx->buf, rv64_sw(val, base, 0));
            break;
        }
        default:
            XR_DCHECK(false, "rv64_h_subword: unreachable opcode");
            break;
    }
}

/* ========== Field Load/Store ========== */

static void rv64_h_field_load(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    int32_t offset = 0;
    if (xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        offset = (int32_t) ctx->func->consts[ci].val.i64;
    }
    RV64_CODEGEN_CHECK(ctx, offset >= -2048 && offset <= 2047, "field offset out of 12-bit range");
    if (ins->rep == XR_REP_F64) {
        Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
        rv64_buf_emit(&ctx->buf, rv64_fld(fd, base, offset));
    } else {
        rv64_buf_emit(&ctx->buf, rv64_ld(rd, base, offset));
    }
}

static void rv64_h_field_store(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    int32_t offset = 0;
    if (xm_ref_is_const(ins->dst)) {
        uint32_t ci = XM_REF_INDEX(ins->dst);
        offset = (int32_t) ctx->func->consts[ci].val.i64;
    }
    RV64_CODEGEN_CHECK(ctx, offset >= -2048 && offset <= 2047, "field offset out of 12-bit range");
    bool val_fp = false;
    if (xm_ref_is_vreg(ins->args[1])) {
        uint32_t vi = XM_REF_INDEX(ins->args[1]);
        if (vi < ctx->func->nvreg)
            val_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
    }
    if (val_fp) {
        Rv64Freg fs = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT11);
        rv64_buf_emit(&ctx->buf, rv64_fsd(fs, base, offset));
    } else {
        Rv64Reg val = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
        rv64_buf_emit(&ctx->buf, rv64_sd(val, base, offset));
    }
}

/* ========== Coro/context access ========== */

static void rv64_h_coro(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    switch (ins->op) {
        case XM_LOAD_CORO: {
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->args[0]) && xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            rv64_buf_emit(&ctx->buf, rv64_ld(rd, RV64_JIT_CTX_REG, offset));
            break;
        }
        case XM_LOAD_CORO_BYTE: {
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->args[0]) && xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            rv64_buf_emit(&ctx->buf, rv64_lbu(rd, RV64_JIT_CTX_REG, offset));
            break;
        }
        case XM_STORE_CORO: {
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                uint32_t ci = XM_REF_INDEX(ins->dst);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            bool val_fp = false;
            if (xm_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XM_REF_INDEX(ins->args[0]);
                if (vi < ctx->func->nvreg)
                    val_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
            }
            if (val_fp) {
                Rv64Freg fs = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
                rv64_buf_emit(&ctx->buf, rv64_fsd(fs, RV64_JIT_CTX_REG, offset));
            } else {
                Rv64Reg val = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
                rv64_buf_emit(&ctx->buf, rv64_sd(val, RV64_JIT_CTX_REG, offset));
            }
            break;
        }
        case XM_STORE_CORO_BYTE: {
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                uint32_t ci = XM_REF_INDEX(ins->dst);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            Rv64Reg val = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_sb(val, RV64_JIT_CTX_REG, offset));
            break;
        }
        default:
            XR_DCHECK(false, "rv64_h_coro: unreachable opcode");
            break;
    }
}

/* ========== Return ========== */

static void rv64_h_ret(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Reg rs = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    if (rs != RV64_A0)
        rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A0, rs));
}

/* ========== NOP / PHI (no-op in codegen) ========== */

static void rv64_h_nop(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) ctx;
    (void) ins;
    (void) rd;
}

/* ========== Call Handler Wrapper ========== */

static void rv64_h_call(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    bool handled = rv64_emit_call_ins(ctx, ins, rd);
    RV64_CODEGEN_CHECK(ctx, handled, "rv64_h_call: unhandled call opcode");
}

/* ========== Stub Handlers (to be implemented) ========== */

/* These handlers emit a bail for now — they will be fleshed out
 * when the full RISC-V JIT pipeline is brought up. */

static void rv64_h_stub(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) ins;
    (void) rd;
    RV64_CODEGEN_CHECK(ctx, false, "unimplemented RV64 handler (stub)");
}

/* ========== Dispatch Table ========== */

static const Rv64InsHandler rv64_ins_handlers[XM_OP_COUNT] = {
    /* Constants */
    [XM_CONST_I64] = rv64_h_const,
    [XM_CONST_PTR] = rv64_h_const,
    [XM_CONST_F64] = rv64_h_const_f64,

    /* Integer arithmetic */
    [XM_ADD] = rv64_h_add,
    [XM_SUB] = rv64_h_sub,
    [XM_MUL] = rv64_h_mul,
    [XM_DIV] = rv64_h_div,
    [XM_MOD] = rv64_h_mod,
    [XM_NEG] = rv64_h_neg,

    /* Bitwise */
    [XM_AND] = rv64_h_and,
    [XM_OR] = rv64_h_or,
    [XM_XOR] = rv64_h_xor,
    [XM_NOT] = rv64_h_not,

    /* Shifts */
    [XM_SHL] = rv64_h_shl,
    [XM_SHR] = rv64_h_shr,

    /* Integer comparison */
    [XM_EQ] = rv64_h_cmp_int,
    [XM_NE] = rv64_h_cmp_int,
    [XM_LT] = rv64_h_cmp_int,
    [XM_LE] = rv64_h_cmp_int,
    [XM_GT] = rv64_h_cmp_int,
    [XM_GE] = rv64_h_cmp_int,

    /* Float arithmetic */
    [XM_FADD] = rv64_h_fadd,
    [XM_FSUB] = rv64_h_fsub,
    [XM_FMUL] = rv64_h_fmul,
    [XM_FDIV] = rv64_h_fdiv,
    [XM_FNEG] = rv64_h_fneg,

    /* Float conversion */
    [XM_I2F] = rv64_h_i2f,
    [XM_F2I] = rv64_h_f2i,

    /* Float comparison */
    [XM_FEQ] = rv64_h_cmp_float,
    [XM_FNE] = rv64_h_cmp_float,
    [XM_FLT] = rv64_h_cmp_float,
    [XM_FLE] = rv64_h_cmp_float,

    /* Move / Select */
    [XM_MOV] = rv64_h_mov,
    [XM_REDEFINE] = rv64_h_mov,
    [XM_SELECT_COND] = rv64_h_select_cond,
    [XM_SELECT] = rv64_h_select,

    /* Load / Store */
    [XM_LOAD] = rv64_h_load,
    [XM_STORE] = rv64_h_store,
    [XM_LOAD8Z] = rv64_h_subword,
    [XM_LOAD8S] = rv64_h_subword,
    [XM_STORE8] = rv64_h_subword,
    [XM_LOAD16Z] = rv64_h_subword,
    [XM_LOAD16S] = rv64_h_subword,
    [XM_STORE16] = rv64_h_subword,
    [XM_LOAD32Z] = rv64_h_subword,
    [XM_LOAD32S] = rv64_h_subword,
    [XM_STORE32] = rv64_h_subword,
    [XM_LOAD_F32] = rv64_h_stub,
    [XM_STORE_F32] = rv64_h_stub,

    /* Coro/context */
    [XM_LOAD_CORO] = rv64_h_coro,
    [XM_LOAD_CORO_BYTE] = rv64_h_coro,
    [XM_STORE_CORO] = rv64_h_coro,
    [XM_STORE_CORO_BYTE] = rv64_h_coro,

    /* Field access */
    [XM_LOAD_FIELD] = rv64_h_field_load,
    [XM_STORE_FIELD] = rv64_h_field_store,

    /* Tagged value / box / unbox — stub for now */
    [XM_TAG_LOAD] = rv64_h_stub,
    [XM_BOX_I64] = rv64_h_stub,
    [XM_BOX_F64] = rv64_h_stub,
    [XM_UNBOX_I64] = rv64_h_stub,
    [XM_UNBOX_F64] = rv64_h_stub,

    /* Guards / deopt — stub for now */
    [XM_GUARD_TAG] = rv64_h_stub,
    [XM_GUARD_BOUNDS] = rv64_h_stub,
    [XM_GUARD_NONNULL] = rv64_h_stub,
    [XM_GUARD_CLASS] = rv64_h_stub,
    [XM_GUARD_KLASS] = rv64_h_stub,
    [XM_TAG_CHECK] = rv64_h_stub,
    [XM_DEOPT] = rv64_h_stub,
    [XM_SAFEPOINT] = rv64_h_stub,

    /* Runtime helpers — stub for now */
    [XM_RT_ADD] = rv64_h_stub,
    [XM_RT_SUB] = rv64_h_stub,
    [XM_RT_MUL] = rv64_h_stub,
    [XM_RT_DIV] = rv64_h_stub,
    [XM_RT_MOD] = rv64_h_stub,
    [XM_RT_UNM] = rv64_h_stub,
    [XM_RT_LT] = rv64_h_stub,
    [XM_RT_LE] = rv64_h_stub,
    [XM_RT_EQ] = rv64_h_stub,
    [XM_RT_ARRAY_NEW] = rv64_h_stub,
    [XM_RT_MAP_NEW] = rv64_h_stub,
    [XM_RT_ARRAY_PUSH] = rv64_h_stub,
    [XM_RT_PRINT] = rv64_h_stub,
    [XM_RT_ARRAY_LEN] = rv64_h_stub,
    [XM_RT_INDEX_GET] = rv64_h_stub,
    [XM_RT_INDEX_SET] = rv64_h_stub,
    [XM_RT_ISNULL] = rv64_h_stub,

    /* Exception / coroutine — no-op or stub */
    [XM_TRY_BEGIN] = rv64_h_nop,
    [XM_TRY_END] = rv64_h_nop,
    [XM_THROW] = rv64_h_nop,
    [XM_NOP] = rv64_h_nop,
    [XM_PHI] = rv64_h_nop,
    [XM_SUSPEND] = rv64_h_stub,
    [XM_BARRIER_FWD] = rv64_h_stub,
    [XM_BARRIER_BACK] = rv64_h_stub,
    [XM_ALLOC] = rv64_h_stub,
    [XM_CATCH] = rv64_h_stub,

    /* Calls */
    [XM_CALL_C] = rv64_h_call,
    [XM_CALL_C_LEAF] = rv64_h_call,
    [XM_CALL_SELF_DIRECT] = rv64_h_call,
    [XM_CALL_KNOWN] = rv64_h_call,
    [XM_CALL_KNOWN_REG] = rv64_h_call,
    [XM_CALL_DIRECT] = rv64_h_call,
    [XM_CALL] = rv64_h_call,

    /* Return */
    [XM_RET] = rv64_h_ret,
};

/* ========== Instruction Dispatch ========== */

XR_FUNC void rv64_emit_xm_ins(Rv64CodegenCtx *ctx, XmIns *ins) {
    RV64_CODEGEN_CHECK(ctx, ctx != NULL, "rv64_emit_xm_ins: NULL ctx");
    RV64_CODEGEN_CHECK(ctx, ins != NULL, "rv64_emit_xm_ins: NULL ins");
    Rv64Reg rd = rv64_get_reg(ctx, ins->dst);

    XR_DCHECK(ins->op >= 0 && ins->op < XM_OP_COUNT, "rv64_emit_xm_ins: op out of range");
    Rv64InsHandler handler = rv64_ins_handlers[ins->op];
    if (handler) {
        handler(ctx, ins, rd);
    } else {
        RV64_CODEGEN_CHECK(ctx, false, "unhandled Xm opcode in riscv64 backend");
    }
}

#endif /* __riscv */
