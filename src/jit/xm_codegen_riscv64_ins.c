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
#include "xm_dispatch_meta.h"
#include "xm_dispatch_emit_gen.h"
#include "xm_helper_table.h"
#include "xm_jit_runtime.h"
#define XM_RUNTIME_STUBS_ENTRIES
#include "xm_runtime_stubs_gen.h"
#include "../coro/xcoroutine.h"
#include "../runtime/gc/xgc_internal.h"
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
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected op");
}

static void rv64_h_sub(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected op");
}

static void rv64_h_mul(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected op");
}

/* RISC-V DIV: no trap on div-by-zero (returns -1) or INT64_MIN/-1 (returns INT64_MIN).
 * This matches Xray's wrap semantics, so no guard needed (unlike x64 IDIV). */
static void rv64_h_div(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected DIV");
}

/* RISC-V REM: same no-trap semantics as DIV. */
static void rv64_h_mod(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected MOD");
}

static void rv64_h_neg(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, RV64_X0, rs),
                       "riscv64 generated gp_rrr dispatch rejected NEG");
}

/* ========== Bitwise Logic ========== */

static void rv64_h_and(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected op");
}

static void rv64_h_or(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected op");
}

static void rv64_h_xor(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected op");
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
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected op");
}

static void rv64_h_shr(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg rs1 = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg rs2 = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(ins->op, &ctx->buf, rd, rs1, rs2),
                       "riscv64 generated gp_rrr dispatch rejected op");
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
            RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(XM_LT, &ctx->buf, rd, rs1, rs2),
                               "riscv64 generated gp_rrr dispatch rejected XM_LT");
            break;
        case XM_GE:
            /* rd = !(rs1 < rs2) = (rs1 >= rs2): slt tmp, rs1, rs2; xori rd, tmp, 1 */
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_gp_cmp_inv_rrr(XM_GE, &ctx->buf, rd, rs1, rs2),
                "riscv64 generated gp_cmp_inv_rrr dispatch rejected XM_GE");
            break;
        case XM_GT:
            /* rd = (rs2 < rs1) — swap operands at the wrapper call. */
            RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_rrr(XM_GT, &ctx->buf, rd, rs2, rs1),
                               "riscv64 generated gp_rrr dispatch rejected XM_GT");
            break;
        case XM_LE:
            /* rd = !(rs2 < rs1) = (rs1 <= rs2): swap rs1/rs2 at the wrapper call so
             * the wrapper body is the canonical slt(rd, a, b) + xori(rd, rd, 1). */
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_gp_cmp_inv_rrr(XM_LE, &ctx->buf, rd, rs2, rs1),
                "riscv64 generated gp_cmp_inv_rrr dispatch rejected XM_LE");
            break;
        case XM_EQ:
            /* rd = (rs1 == rs2): sub tmp, rs1, rs2; sltiu rd, tmp, 1 */
            RV64_CODEGEN_CHECK(ctx,
                               xm_dispatch_emit_riscv64_gp_cmp_diff_rrr(XM_EQ, &ctx->buf, rd, rs1,
                                                                        rs2, RV64_SCRATCH_REG),
                               "riscv64 generated gp_cmp_diff_rrr dispatch rejected XM_EQ");
            break;
        case XM_NE:
            /* rd = (rs1 != rs2): sub tmp, rs1, rs2; sltu rd, x0, tmp */
            RV64_CODEGEN_CHECK(ctx,
                               xm_dispatch_emit_riscv64_gp_cmp_diff_rrr(XM_NE, &ctx->buf, rd, rs1,
                                                                        rs2, RV64_SCRATCH_REG),
                               "riscv64 generated gp_cmp_diff_rrr dispatch rejected XM_NE");
            break;
        default:
            RV64_CODEGEN_CHECK(ctx, false, "rv64_h_cmp_int: unreachable opcode");
            break;
    }
}

/* ========== Float Arithmetic ========== */

static void rv64_h_fadd(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_fp_rrr(ins->op, &ctx->buf, fd, fs1, fs2),
                       "riscv64 generated fp_rrr dispatch rejected op");
}

static void rv64_h_fsub(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_fp_rrr(ins->op, &ctx->buf, fd, fs1, fs2),
                       "riscv64 generated fp_rrr dispatch rejected op");
}

static void rv64_h_fmul(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_fp_rrr(ins->op, &ctx->buf, fd, fs1, fs2),
                       "riscv64 generated fp_rrr dispatch rejected op");
}

static void rv64_h_fdiv(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_fp_rrr(ins->op, &ctx->buf, fd, fs1, fs2),
                       "riscv64 generated fp_rrr dispatch rejected op");
}

static void rv64_h_fneg(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Freg fs = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    /* FNEG.D = FSGNJN.D rd, rs, rs (hardware instruction, no XOR hack) */
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_fp_r(ins->op, &ctx->buf, fd, fs),
                       "riscv64 generated fp_r dispatch rejected op");
}

/* ========== Float Conversion ========== */

static void rv64_h_i2f(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    Rv64Reg rs = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_conv_i2f(ins->op, &ctx->buf, fd, rs),
                       "riscv64 generated conv_i2f dispatch rejected op");
}

static void rv64_h_f2i(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Freg fs = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_conv_f2i(ins->op, &ctx->buf, rd, fs),
                       "riscv64 generated conv_f2i dispatch rejected op");
}

/* ========== Float Comparison ========== */

/* RISC-V FP comparisons write 0/1 directly to a GPR — no flags, no parity. */

static void rv64_h_cmp_float(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Freg fs1 = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT11);
    Rv64Freg fs2 = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT10);

    /* All FP compares (FEQ/FNE/FLT/FLE) now use generated wrapper. */
    RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_fp_cmp_rrr(ins->op, &ctx->buf, rd, fs1, fs2),
                       "riscv64 generated fp_cmp_rrr dispatch rejected op");
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
            RV64_CODEGEN_CHECK(ctx, xm_dispatch_emit_riscv64_gp_r(ins->op, &ctx->buf, rd, rs),
                               "riscv64 generated gp_r dispatch rejected op");
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
        RV64_CODEGEN_CHECK(ctx,
                           xm_dispatch_emit_riscv64_mem_load_gp(ins->op, &ctx->buf, rd, base, 0),
                           "riscv64 generated mem_load_gp dispatch rejected LOAD");
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
        RV64_CODEGEN_CHECK(ctx,
                           xm_dispatch_emit_riscv64_mem_store_gp(ins->op, &ctx->buf, base, 0, val),
                           "riscv64 generated mem_store_gp dispatch rejected STORE");
    }
}

/* ========== Sub-word Load/Store ========== */

static void rv64_h_subword(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    switch (ins->op) {
        case XM_LOAD8Z: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_mem_load_gp(ins->op, &ctx->buf, rd, base, 0),
                "riscv64 generated mem_load_gp dispatch rejected op");
            break;
        }
        case XM_LOAD8S: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_mem_load_gp(ins->op, &ctx->buf, rd, base, 0),
                "riscv64 generated mem_load_gp dispatch rejected op");
            break;
        }
        case XM_STORE8: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            Rv64Reg val = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_mem_store_gp(ins->op, &ctx->buf, base, 0, val),
                "riscv64 generated mem_store_gp dispatch rejected op");
            break;
        }
        case XM_LOAD16Z: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_mem_load_gp(ins->op, &ctx->buf, rd, base, 0),
                "riscv64 generated mem_load_gp dispatch rejected op");
            break;
        }
        case XM_LOAD16S: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_mem_load_gp(ins->op, &ctx->buf, rd, base, 0),
                "riscv64 generated mem_load_gp dispatch rejected op");
            break;
        }
        case XM_STORE16: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            Rv64Reg val = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_mem_store_gp(ins->op, &ctx->buf, base, 0, val),
                "riscv64 generated mem_store_gp dispatch rejected op");
            break;
        }
        case XM_LOAD32Z: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_mem_load_gp(ins->op, &ctx->buf, rd, base, 0),
                "riscv64 generated mem_load_gp dispatch rejected op");
            break;
        }
        case XM_LOAD32S: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            int32_t offset = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_mem_load_gp(ins->op, &ctx->buf, rd, base, offset),
                "riscv64 generated mem_load_gp dispatch rejected op");
            break;
        }
        case XM_STORE32: {
            Rv64Reg base = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            Rv64Reg val = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
            RV64_CODEGEN_CHECK(
                ctx, xm_dispatch_emit_riscv64_mem_store_gp(ins->op, &ctx->buf, base, 0, val),
                "riscv64 generated mem_store_gp dispatch rejected op");
            break;
        }
        default:
            RV64_CODEGEN_CHECK(ctx, false, "rv64_h_subword: unreachable opcode");
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
        RV64_CODEGEN_CHECK(
            ctx, xm_dispatch_emit_riscv64_mem_load_gp(ins->op, &ctx->buf, rd, base, offset),
            "riscv64 generated mem_load_gp dispatch rejected LOAD_FIELD");
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
        RV64_CODEGEN_CHECK(
            ctx, xm_dispatch_emit_riscv64_mem_store_gp(ins->op, &ctx->buf, base, offset, val),
            "riscv64 generated mem_store_gp dispatch rejected STORE_FIELD");
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
            RV64_CODEGEN_CHECK(ctx, false, "rv64_h_coro: unreachable opcode");
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

/* ========== Tag Load ========== */

static void rv64_h_tag_load(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    Rv64Reg ptr = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    int32_t offset = 0;
    if (!xm_ref_is_none(ins->args[1]) && xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        offset = (int32_t) ctx->func->consts[ci].val.i64;
    }
    rv64_buf_emit(&ctx->buf, rv64_lbu(rd, ptr, offset));
}

/* ========== Box / Unbox ========== */

static void rv64_h_box(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    /* BOX is a no-op inside JIT (values are always raw/untagged) */
    Rv64Reg rn = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    if (rd != rn)
        rv64_buf_emit(&ctx->buf, rv64_mv(rd, rn));
}

static void rv64_h_unbox_i64(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    uint8_t src_type = XR_REP_I64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t vi = XM_REF_INDEX(ins->args[0]);
        if (vi < ctx->func->nvreg)
            src_type = ctx->func->vregs[vi].rep;
    }
    if (src_type == XR_REP_PTR) {
        Rv64Reg ptr = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
        rv64_buf_emit(&ctx->buf, rv64_ld(rd, ptr, XM_XRVALUE_PAYLOAD_OFFSET));
    } else {
        Rv64Reg rn = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
        if (rd != rn)
            rv64_buf_emit(&ctx->buf, rv64_mv(rd, rn));
    }
}

static void rv64_h_unbox_f64(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    uint8_t src_type = XR_REP_F64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t vi = XM_REF_INDEX(ins->args[0]);
        if (vi < ctx->func->nvreg)
            src_type = ctx->func->vregs[vi].rep;
    }
    if (src_type == XR_REP_PTR) {
        Rv64Reg ptr = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
        Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
        rv64_buf_emit(&ctx->buf, rv64_fld(fd, ptr, XM_XRVALUE_PAYLOAD_OFFSET));
    } else {
        Rv64Reg rn = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
        if (rd != rn)
            rv64_buf_emit(&ctx->buf, rv64_mv(rd, rn));
    }
}

/* ========== Guard Ops ========== */

static void rv64_h_guard(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    switch (ins->op) {
        case XM_GUARD_TAG: {
            Rv64Reg val_reg = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_lbu(RV64_SCRATCH_REG, val_reg, XM_XRVALUE_TAG_OFFSET));
            Rv64Reg exp_reg;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                int32_t expected = (int32_t) ctx->func->consts[ci].val.raw;
                rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) expected);
                exp_reg = RV64_SCRATCH_REG2;
            } else {
                exp_reg = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
            }
            /* Deopt if tag != expected */
            rv64_buf_emit(&ctx->buf, rv64_sub(RV64_SCRATCH_REG, RV64_SCRATCH_REG, exp_reg));
            rv64_emit_deopt_id(ctx, ins);
            rv64_emit_deopt_branch(ctx, RV64_SCRATCH_REG);
            break;
        }
        case XM_GUARD_BOUNDS: {
            Rv64Reg idx_reg = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            Rv64Reg len_reg = rv64_get_operand(ctx, ins->args[1],
                                               idx_reg == RV64_SCRATCH_REG ? RV64_SCRATCH_REG2
                                                                           : RV64_SCRATCH_REG);
            /* Deopt if idx >= len (unsigned), i.e., BGEU idx, len -> deopt.
             * Emit: SLTU tmp, idx, len; deopt if tmp == 0 (idx >= len) */
            rv64_buf_emit(&ctx->buf, rv64_sltu(RV64_SCRATCH_REG, idx_reg, len_reg));
            /* Invert: deopt when tmp == 0.  SEQZ + deopt_branch. */
            rv64_buf_emit(&ctx->buf, rv64_seqz(RV64_SCRATCH_REG, RV64_SCRATCH_REG));
            rv64_emit_deopt_id(ctx, ins);
            rv64_emit_deopt_branch(ctx, RV64_SCRATCH_REG);
            break;
        }
        case XM_GUARD_NONNULL: {
            Rv64Reg val_reg = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            /* Deopt if val == 0 (null) */
            rv64_emit_deopt_id(ctx, ins);
            rv64_emit_deopt_branch(ctx, val_reg);
            break;
        }
        case XM_GUARD_CLASS: {
            Rv64Reg obj = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_lhu(RV64_SCRATCH_REG, obj, (int32_t) XM_GC_EXTRA_OFFSET));
            Rv64Reg exp_reg;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                int32_t expected = (int32_t) ctx->func->consts[ci].val.raw;
                rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) expected);
                exp_reg = RV64_SCRATCH_REG2;
            } else {
                exp_reg = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
            }
            rv64_buf_emit(&ctx->buf, rv64_sub(RV64_SCRATCH_REG, RV64_SCRATCH_REG, exp_reg));
            rv64_emit_deopt_id(ctx, ins);
            rv64_emit_deopt_branch(ctx, RV64_SCRATCH_REG);
            break;
        }
        case XM_GUARD_KLASS: {
            Rv64Reg obj = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf,
                          rv64_ld(RV64_SCRATCH_REG, obj, (int32_t) XM_INSTANCE_KLASS_OFFSET));
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                uint64_t expected = (uint64_t) ctx->func->consts[ci].val.i64;
                rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, expected);
            } else {
                Rv64Reg exp = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
                if (exp != RV64_SCRATCH_REG2)
                    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG2, exp));
            }
            rv64_buf_emit(&ctx->buf,
                          rv64_sub(RV64_SCRATCH_REG, RV64_SCRATCH_REG, RV64_SCRATCH_REG2));
            rv64_emit_deopt_id(ctx, ins);
            rv64_emit_deopt_branch(ctx, RV64_SCRATCH_REG);
            break;
        }
        case XM_TAG_CHECK: {
            Rv64Reg val_reg = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            rv64_buf_emit(&ctx->buf, rv64_lbu(RV64_SCRATCH_REG, val_reg, XM_XRVALUE_TAG_OFFSET));
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                int32_t expected = (int32_t) ctx->func->consts[ci].val.raw;
                rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) expected);
            }
            rv64_buf_emit(&ctx->buf,
                          rv64_sub(RV64_SCRATCH_REG, RV64_SCRATCH_REG, RV64_SCRATCH_REG2));
            rv64_emit_deopt_id(ctx, ins);
            rv64_emit_deopt_branch(ctx, RV64_SCRATCH_REG);
            break;
        }
        default:
            RV64_CODEGEN_CHECK(ctx, false, "rv64_h_guard: unreachable opcode");
            break;
    }
}

/* ========== Deopt ========== */

static void rv64_h_deopt(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    rv64_emit_deopt_id(ctx, ins);
    rv64_emit_deopt_jmp(ctx);
}

/* ========== Safepoint ========== */

static void rv64_h_safepoint(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    (void) ins;
    uint32_t smap_id = rv64_record_safepoint(ctx);
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) smap_id);
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                     (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET));
    /* Touch safepoint page to allow GC stop-the-world via mprotect */
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                     (int32_t) XM_JIT_SAFEPOINT_PAGE_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_lbu(RV64_SCRATCH_REG, RV64_SCRATCH_REG, 0));
}

/* ========== Runtime Arithmetic (mixed int/float) ========== */

static void rv64_h_rt_arith(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t ai = XM_REF_INDEX(ins->args[0]);
        if (ai < ctx->func->nvreg)
            ta = ctx->func->vregs[ai].rep;
    }
    if (xm_ref_is_vreg(ins->args[1])) {
        uint32_t bi = XM_REF_INDEX(ins->args[1]);
        if (bi < ctx->func->nvreg)
            tb = ctx->func->vregs[bi].rep;
    }
    XR_DCHECK(ins->op >= XM_RT_ADD && ins->op <= XM_RT_MOD, "rv64_h_rt_arith: bad op");

    if ((ta == XR_REP_I64 || ta == XR_REP_F64) && (tb == XR_REP_I64 || tb == XR_REP_F64)) {
        /* Promote both to double */
        Rv64Freg fa;
        if (ta == XR_REP_F64) {
            fa = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT10);
        } else {
            Rv64Reg ga = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            fa = RV64_FT10;
            rv64_buf_emit(&ctx->buf, rv64_fcvt_d_l(fa, ga));
        }
        Rv64Freg fb;
        if (tb == XR_REP_F64) {
            fb = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT11);
        } else {
            Rv64Reg gb = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG);
            fb = RV64_FT11;
            rv64_buf_emit(&ctx->buf, rv64_fcvt_d_l(fb, gb));
        }
        Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
        switch (ins->op) {
            case XM_RT_ADD:
                rv64_buf_emit(&ctx->buf, rv64_fadd_d(fd, fa, fb));
                break;
            case XM_RT_SUB:
                rv64_buf_emit(&ctx->buf, rv64_fsub_d(fd, fa, fb));
                break;
            case XM_RT_MUL:
                rv64_buf_emit(&ctx->buf, rv64_fmul_d(fd, fa, fb));
                break;
            case XM_RT_DIV:
                rv64_buf_emit(&ctx->buf, rv64_fdiv_d(fd, fa, fb));
                break;
            case XM_RT_MOD: {
                /* fmod: fd = fa - trunc(fa/fb) * fb */
                rv64_buf_emit(&ctx->buf, rv64_fdiv_d(RV64_FT10, fa, fb));
                rv64_buf_emit(&ctx->buf, rv64_fcvt_l_d(RV64_SCRATCH_REG, RV64_FT10));
                rv64_buf_emit(&ctx->buf, rv64_fcvt_d_l(RV64_FT10, RV64_SCRATCH_REG));
                rv64_buf_emit(&ctx->buf, rv64_fmul_d(RV64_FT10, RV64_FT10, fb));
                rv64_buf_emit(&ctx->buf, rv64_fsub_d(fd, fa, RV64_FT10));
                break;
            }
            default:
                RV64_CODEGEN_CHECK(ctx, false, "rv64_h_rt_arith: unreachable float op");
                break;
        }
    } else {
        /* Non-numeric types: deopt to interpreter */
        rv64_emit_deopt_id(ctx, ins);
        rv64_emit_deopt_jmp(ctx);
    }
}

/* ========== Runtime Unary Minus ========== */

static void rv64_h_rt_unm(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    uint8_t ta = XR_REP_I64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t ai = XM_REF_INDEX(ins->args[0]);
        if (ai < ctx->func->nvreg)
            ta = ctx->func->vregs[ai].rep;
    }
    Rv64Freg fd = rv64_get_fp_reg(ctx, ins->dst);
    if (ta == XR_REP_F64) {
        Rv64Freg fa = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT10);
        rv64_buf_emit(&ctx->buf, rv64_fneg_d(fd, fa));
    } else {
        Rv64Reg ga = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
        rv64_buf_emit(&ctx->buf, rv64_fcvt_d_l(fd, ga));
        rv64_buf_emit(&ctx->buf, rv64_fneg_d(fd, fd));
    }
}

/* ========== Runtime Comparison ========== */

static void rv64_h_rt_cmp(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t ai = XM_REF_INDEX(ins->args[0]);
        if (ai < ctx->func->nvreg)
            ta = ctx->func->vregs[ai].rep;
    }
    if (xm_ref_is_vreg(ins->args[1])) {
        uint32_t bi = XM_REF_INDEX(ins->args[1]);
        if (bi < ctx->func->nvreg)
            tb = ctx->func->vregs[bi].rep;
    }

    if ((ta == XR_REP_I64 || ta == XR_REP_F64) && (tb == XR_REP_I64 || tb == XR_REP_F64)) {
        Rv64Freg fa;
        if (ta == XR_REP_F64) {
            fa = rv64_get_fp_operand(ctx, ins->args[0], RV64_FT10);
        } else {
            Rv64Reg ga = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
            fa = RV64_FT10;
            rv64_buf_emit(&ctx->buf, rv64_fcvt_d_l(fa, ga));
        }
        Rv64Freg fb;
        if (tb == XR_REP_F64) {
            fb = rv64_get_fp_operand(ctx, ins->args[1], RV64_FT11);
        } else {
            Rv64Reg gb = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG);
            fb = RV64_FT11;
            rv64_buf_emit(&ctx->buf, rv64_fcvt_d_l(fb, gb));
        }
        /* RISC-V FP compare writes GPR directly */
        switch (ins->op) {
            case XM_RT_LT:
                rv64_buf_emit(&ctx->buf, rv64_flt_d(rd, fa, fb));
                break;
            case XM_RT_LE:
                rv64_buf_emit(&ctx->buf, rv64_fle_d(rd, fa, fb));
                break;
            case XM_RT_EQ:
                rv64_buf_emit(&ctx->buf, rv64_feq_d(rd, fa, fb));
                break;
            default:
                RV64_CODEGEN_CHECK(ctx, false, "rv64_h_rt_cmp: unreachable cmp op");
                break;
        }
    } else {
        rv64_emit_deopt_id(ctx, ins);
        rv64_emit_deopt_jmp(ctx);
    }
}

/* ========== Runtime Collection New ========== */

static void rv64_h_rt_collection(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    rv64_emit_ptr_spill_writeback(ctx);

    uint32_t smap_id = rv64_record_safepoint(ctx);
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) smap_id);
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                     (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET));

    /* Store capacity to extra_arg scratch slot */
    if (xm_ref_is_const(ins->args[0])) {
        uint32_t ci = XM_REF_INDEX(ins->args[0]);
        uint64_t cval = (uint64_t) ctx->func->consts[ci].val.raw;
        rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, cval);
    } else {
        Rv64Reg r = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
        if (r != RV64_SCRATCH_REG)
            rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, r));
    }
    rv64_buf_emit(&ctx->buf,
                  rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, (int32_t) RV64_EXTRA_ARG_OFFSET));

    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_X0, RV64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET));

    void *fn = (ins->op == XM_RT_ARRAY_NEW) ? xm_helper_func(XM_HELPER_rt_array_new)
                                            : xm_helper_func(XM_HELPER_rt_map_new);
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) (uintptr_t) fn);

    rv64_add_patch(ctx, RV64_PATCH_CALL_C, 0, RV64_CC_EQ, 0, 0);
    rv64_buf_emit(&ctx->buf, rv64_call(0));
    ctx->has_call_c = true;

    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t dvi = XM_REF_INDEX(ins->dst);
        if (rd != RV64_A0)
            rv64_buf_emit(&ctx->buf, rv64_mv(rd, RV64_A0));
        if (dvi < ctx->func->nvreg && dvi < XR_JIT_MAX_VREG_TAGS) {
            int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) dvi;
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, XR_TAG_PTR);
            rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, tag_off));
        }
    }
}

/* ========== Runtime Array Push ========== */

static void rv64_h_rt_array_push(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    rv64_emit_ptr_spill_writeback(ctx);

    uint32_t smap_id = rv64_record_safepoint(ctx);
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) smap_id);
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                     (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET));

    /* Store array (arg0) and element (arg1) to call_args[0..1] */
    int32_t ca0 = (int32_t) XM_JIT_CALL_ARGS_OFFSET;
    Rv64Reg arr_r = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    rv64_buf_emit(&ctx->buf, rv64_sd(arr_r, RV64_JIT_CTX_REG, ca0));

    int32_t ca1 = (int32_t) (XM_JIT_CALL_ARGS_OFFSET + 8);
    if (xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        uint64_t cval = (uint64_t) ctx->func->consts[ci].val.raw;
        rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, cval);
        rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, ca1));
    } else {
        Rv64Reg vr = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG);
        rv64_buf_emit(&ctx->buf, rv64_sd(vr, RV64_JIT_CTX_REG, ca1));
    }

    /* Pack call_arg_tags: tag0=PTR, tag1 from compile-time info */
    uint8_t t0 = XR_TAG_PTR;
    uint8_t t1 = XR_RTAG_UNKNOWN;
    if (xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        t1 = rv64_const_rep_to_value_tag(ctx->func->consts[ci].rep);
    } else {
        XmType ct = xm_ref_ctype(ctx->func, ins->args[1]);
        uint8_t vk = type_kind_to_vtag(ct.kind);
        if (vtag_is_concrete(vk))
            t1 = vtag_to_value_tag(vk);
    }
    uint64_t tpk = (uint64_t) t0 | ((uint64_t) t1 << 8);
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, tpk);
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                     (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET));

    /* Dynamic tag patch for unknown element tag */
    if (t1 == XR_RTAG_UNKNOWN && xm_ref_is_vreg(ins->args[1])) {
        uint32_t ai = XM_REF_INDEX(ins->args[1]);
        if (ai < ctx->func->nvreg && ai < XR_JIT_MAX_VREG_TAGS) {
            int32_t soff = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) ai;
            int32_t doff = (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET + 1;
            rv64_buf_emit(&ctx->buf, rv64_lbu(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, soff));
            rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, doff));
        }
    }

    /* Clear extra_arg and deopt_id */
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_X0, RV64_JIT_CTX_REG, (int32_t) RV64_EXTRA_ARG_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_X0, RV64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET));

    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG,
                    (uint64_t) (uintptr_t) xm_helper_func(XM_HELPER_rt_array_push));

    rv64_add_patch(ctx, RV64_PATCH_CALL_C, 0, RV64_CC_EQ, 0, 0);
    rv64_buf_emit(&ctx->buf, rv64_call(0));
    ctx->has_call_c = true;
}

/* ========== Runtime Simple Ops (ISNULL, PRINT, ARRAY_LEN, INDEX_GET/SET) ========== */

static bool rv64_isnull_uses_runtime_tag(Rv64CodegenCtx *ctx, XmRef ref, uint32_t *out_vi) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t vi = XM_REF_INDEX(ref);
    if (vi >= ctx->func->nvreg || vi >= XR_JIT_MAX_VREG_TAGS)
        return false;
    if (ctx->func->vregs[vi].rep == XR_REP_TAGGED) {
        *out_vi = vi;
        return true;
    }
    XmType ct = xm_ref_ctype(ctx->func, ref);
    if (ct.kind != XM_TK_TAGGED)
        return false;
    XmIns *def = ctx->func->vregs[vi].def;
    if (!def) {
        *out_vi = vi;
        return true;
    }
    if (def->op == XM_CALL_METHOD_KNOWN) {
        *out_vi = vi;
        return true;
    }
    switch (def->op) {
        case XM_CALL_C:
        case XM_CALL_KNOWN:
        case XM_CALL_KNOWN_REG:
        case XM_CALL_DIRECT:
        case XM_CALL_SELF_DIRECT:
            *out_vi = vi;
            return true;
        default:
            return false;
    }
}

static void rv64_h_rt_simple(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    if (ins->op == XM_RT_ISNULL) {
        if (xm_ref_is_vreg(ins->args[0])) {
            XmType ct = xm_ref_ctype(ctx->func, ins->args[0]);
            if (ct.kind == XM_TK_NULL) {
                rv64_load_imm64(&ctx->buf, rd, 1);
                return;
            }
            if (ct.kind == XM_TK_INT || ct.kind == XM_TK_FLOAT || ct.kind == XM_TK_BOOL) {
                rv64_load_imm64(&ctx->buf, rd, 0);
                return;
            }
            uint32_t vi = 0;
            if (rv64_isnull_uses_runtime_tag(ctx, ins->args[0], &vi)) {
                int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
                rv64_buf_emit(&ctx->buf, rv64_lbu(rd, RV64_JIT_CTX_REG, tag_off));
                /* rd = (rd == XR_TAG_NULL) ? 1 : 0.  XR_TAG_NULL == 0, so SEQZ. */
                rv64_buf_emit(&ctx->buf, rv64_seqz(rd, rd));
                return;
            }
        }
        Rv64Reg val = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
        /* rd = (val == 0) ? 1 : 0 */
        rv64_buf_emit(&ctx->buf, rv64_seqz(rd, val));
    } else {
        /* RT_PRINT, RT_ARRAY_LEN, RT_INDEX_GET, RT_INDEX_SET:
         * These are lowered to CALL_C by the builder. If they reach
         * codegen, emit a NOP so branch offsets stay valid. */
        xr_log_debug("rv64-cg", "RT opcode %d fell through to NOP (expected CALL_C)", ins->op);
        rv64_buf_emit(&ctx->buf, rv64_nop());
    }
}

/* ========== Write Barriers ========== */

static void rv64_h_barrier_fwd(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Reg parent_reg = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    Rv64Reg child_reg = rv64_get_operand(ctx, ins->args[1], RV64_SCRATCH_REG2);
    if (parent_reg != RV64_SCRATCH_REG)
        rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, parent_reg));
    if (child_reg != RV64_SCRATCH_REG2)
        rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG2, child_reg));
    rv64_add_patch(ctx, RV64_PATCH_BARRIER_FWD, 0, RV64_CC_EQ, 0, 0);
    rv64_buf_emit(&ctx->buf, rv64_call(0));
    ctx->has_barriers = true;
}

static void rv64_h_barrier_back(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    Rv64Reg container_reg = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG);
    if (container_reg != RV64_SCRATCH_REG)
        rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, container_reg));
    rv64_add_patch(ctx, RV64_PATCH_BARRIER_BACK, 0, RV64_CC_EQ, 0, 0);
    rv64_buf_emit(&ctx->buf, rv64_call(0));
    ctx->has_barriers = true;
}

/* ========== GC Allocation (inline fast path + slow path) ========== */

/* Inline bump-pointer allocator for RISC-V.
 * Fast path: cursor check -> commit -> init GC header -> alloc_post bookkeeping.
 * Slow path: fall through to CALL_C(xr_jit_alloc).
 *
 * Register usage during fast path:
 *   t6 (SCRATCH_REG)  = gc pointer, then block pointer
 *   t5 (SCRATCH_REG2) = cursor / new_cursor / scratch
 *   rd                = temporarily holds limit, then allocated obj pointer */
static void rv64_h_alloc(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    uint8_t gc_type = 0;
    uint16_t gc_extra = 0;
    uint32_t alloc_size = 0;
    if (xm_ref_is_const(ins->args[0])) {
        uint32_t ci = XM_REF_INDEX(ins->args[0]);
        int64_t packed_const = ctx->func->consts[ci].val.i64;
        gc_type = (uint8_t) (packed_const & 0xFF);
        gc_extra = (uint16_t) ((packed_const >> 8) & 0xFFFF);
    }
    if (xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        alloc_size = (uint32_t) ctx->func->consts[ci].val.i64;
    }
    alloc_size = (alloc_size + 7) & ~7u;
    RV64_CODEGEN_CHECK(ctx, alloc_size > 0 && alloc_size < 65536, "alloc_size OOB");

    bool force_slow_path = xr_gc_type_may_need_finalize(gc_type);
    uint32_t force_slow_idx = 0;
    if (force_slow_path) {
        force_slow_idx = ctx->buf.count;
        rv64_buf_emit(&ctx->buf, rv64_j(0)); /* patched below */
    }

    /* === Fast path: inline bump-pointer === */

    /* t6 = coro->coro_gc */
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_CORO_REG, (int32_t) XM_CORO_GC_OFFSET));

    /* BEQ t6, x0, slow_path (gc == NULL) */
    uint32_t beq_slow_idx = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_beq(RV64_SCRATCH_REG, RV64_X0, 0)); /* placeholder */

    /* t5 = gc->cursor */
    rv64_buf_emit(&ctx->buf,
                  rv64_ld(RV64_SCRATCH_REG2, RV64_SCRATCH_REG, (int32_t) XM_REGION_CURSOR_OFFSET));
    /* t5 = cursor + alloc_size (new_cursor) */
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SCRATCH_REG2, RV64_SCRATCH_REG2, (int32_t) alloc_size));

    /* rd = gc->limit (borrow rd as temp) */
    rv64_buf_emit(&ctx->buf, rv64_ld(rd, RV64_SCRATCH_REG, (int32_t) XM_REGION_LIMIT_OFFSET));

    /* BLTU rd, t5, slow_path  (limit < new_cursor → overflow) */
    uint32_t bltu_slow_idx = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_bltu(rd, RV64_SCRATCH_REG2, 0)); /* placeholder */

    /* Commit: gc->cursor = new_cursor */
    rv64_buf_emit(&ctx->buf,
                  rv64_sd(RV64_SCRATCH_REG2, RV64_SCRATCH_REG, (int32_t) XM_REGION_CURSOR_OFFSET));

    /* rd = new_cursor - alloc_size = allocated GCHeader* */
    rv64_buf_emit(&ctx->buf, rv64_addi(rd, RV64_SCRATCH_REG2, -(int32_t) alloc_size));

    /* Init GC header inline */
    /* type = gc_type */
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) gc_type);
    rv64_buf_emit(&ctx->buf, rv64_sh(RV64_SCRATCH_REG2, rd, (int32_t) XM_GC_HDR_TYPE_OFFSET));

    /* extra = gc_extra (16-bit store) */
    if (gc_extra != 0) {
        rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) gc_extra);
        rv64_buf_emit(&ctx->buf, rv64_sh(RV64_SCRATCH_REG2, rd, (int32_t) XM_GC_HDR_EXTRA_OFFSET));
    } else {
        rv64_buf_emit(&ctx->buf, rv64_sh(RV64_X0, rd, (int32_t) XM_GC_HDR_EXTRA_OFFSET));
    }

    /* refcount = 0 (RC is 0-based: a fresh object has exactly one owner,
     * encoded as 0 == unique) */
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_X0, rd, (int32_t) XM_GC_HDR_REFCOUNT_OFFSET));

    /* objsize = alloc_size (32-bit store) */
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) alloc_size);
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG2, rd, (int32_t) XM_GC_HDR_OBJSIZE_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_X0, rd, (int32_t) XM_GC_HDR_RSV_OFFSET));

    /* block = rd & ~0x3FFF (16KB block alignment) */
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, ~(uint64_t) XM_REGION_BLOCK_SIZE_MASK);
    rv64_buf_emit(&ctx->buf, rv64_and(RV64_SCRATCH_REG2, rd, RV64_SCRATCH_REG2));
    /* t5 = block pointer now */

    /* block->alloc_count++ */
    rv64_buf_emit(&ctx->buf, rv64_lw(RV64_SCRATCH_REG, RV64_SCRATCH_REG2,
                                     (int32_t) XM_REGION_BLOCK_ALLOC_COUNT_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SCRATCH_REG, RV64_SCRATCH_REG, 1));
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, RV64_SCRATCH_REG2,
                                     (int32_t) XM_REGION_BLOCK_ALLOC_COUNT_OFFSET));

    /* block->alloc_bytes += alloc_size */
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_SCRATCH_REG2,
                                     (int32_t) XM_REGION_BLOCK_ALLOC_BYTES_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SCRATCH_REG, RV64_SCRATCH_REG, (int32_t) alloc_size));
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_SCRATCH_REG2,
                                     (int32_t) XM_REGION_BLOCK_ALLOC_BYTES_OFFSET));

    /* GC stats: gc->totalbytes += size (RC has no GCdebt/collection trigger). */
    rv64_buf_emit(&ctx->buf,
                  rv64_ld(RV64_SCRATCH_REG2, RV64_CORO_REG, (int32_t) XM_CORO_GC_OFFSET));
    rv64_buf_emit(&ctx->buf,
                  rv64_ld(RV64_SCRATCH_REG, RV64_SCRATCH_REG2, (int32_t) XM_GC_TOTALBYTES_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SCRATCH_REG, RV64_SCRATCH_REG, (int32_t) alloc_size));
    rv64_buf_emit(&ctx->buf,
                  rv64_sd(RV64_SCRATCH_REG, RV64_SCRATCH_REG2, (int32_t) XM_GC_TOTALBYTES_OFFSET));

    /* J alloc_done (skip slow path) */
    uint32_t j_done_idx = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_j(0)); /* placeholder */

    /* === Slow path: CALL_C to xr_jit_alloc === */
    uint32_t slow_path_idx = ctx->buf.count;

    /* Patch fast-path branches to target slow_path */
    if (force_slow_path) {
        int32_t force_off = (int32_t) (slow_path_idx - force_slow_idx) * 4;
        ctx->buf.code[force_slow_idx] = rv64_j(force_off);
    }
    int32_t beq_off = (int32_t) (slow_path_idx - beq_slow_idx) * 4;
    ctx->buf.code[beq_slow_idx] = rv64_beq(RV64_SCRATCH_REG, RV64_X0, beq_off);
    int32_t bltu_off = (int32_t) (slow_path_idx - bltu_slow_idx) * 4;
    ctx->buf.code[bltu_slow_idx] = rv64_bltu(rd, RV64_SCRATCH_REG2, bltu_off);

    rv64_emit_ptr_spill_writeback(ctx);

    uint32_t smap_id = rv64_record_safepoint(ctx);
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) smap_id);
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                     (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET));

    /* Pack type_and_size matching xr_jit_alloc convention: gc_type<<32 | alloc_size */
    uint64_t packed_arg = ((uint64_t) gc_type << 32) | (uint64_t) alloc_size;
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, packed_arg);
    rv64_buf_emit(&ctx->buf,
                  rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, (int32_t) RV64_EXTRA_ARG_OFFSET));

    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_X0, RV64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET));

    uintptr_t alloc_entry =
        xm_runtime_stub_entry(XM_RUNTIME_STUB_alloc, XM_RUNTIME_STUB_ABI_CALL_C_EXTRA_ARG);
    RV64_CODEGEN_CHECK(ctx, alloc_entry != 0, "runtime stub alloc ABI mismatch");
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) alloc_entry);
    rv64_add_patch(ctx, RV64_PATCH_CALL_C, 0, RV64_CC_EQ, 0, 0);
    rv64_buf_emit(&ctx->buf, rv64_call(0));
    ctx->has_call_c = true;

    /* Result (pointer) in a0 */
    if (rd != RV64_A0)
        rv64_buf_emit(&ctx->buf, rv64_mv(rd, RV64_A0));

    /* Deopt if NULL (allocation failure): BEQ rd, x0, deopt */
    rv64_emit_deopt_id(ctx, ins);
    rv64_add_patch(ctx, RV64_PATCH_DEOPT_BRANCH, 0, RV64_CC_EQ, (uint8_t) rd, (uint8_t) RV64_X0);
    rv64_buf_emit(&ctx->buf, rv64_beq(rd, RV64_X0, 0));
    ctx->has_deopt = true;

    /* Set gc_extra after slow path (xr_jit_alloc sets extra=0) */
    if (gc_extra != 0) {
        rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) gc_extra);
        rv64_buf_emit(&ctx->buf, rv64_sh(RV64_SCRATCH_REG, rd, (int32_t) XM_GC_HDR_EXTRA_OFFSET));
    }

    /* alloc_done: patch J over slow path to land here */
    int32_t j_done_off = (int32_t) (ctx->buf.count - j_done_idx) * 4;
    ctx->buf.code[j_done_idx] = rv64_j(j_done_off);

    /* Tag result as PTR */
    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t dvi = XM_REF_INDEX(ins->dst);
        if (dvi < ctx->func->nvreg && dvi < XR_JIT_MAX_VREG_TAGS) {
            int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) dvi;
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, XR_TAG_PTR);
            rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, tag_off));
        }
    }
}

/* ========== Inlined RC fast path (XM_RETAIN / XM_RELEASE) ========== */
/* See a64_emit_rc_ins (xm_codegen_mem.c) for the shared design. LW
 * sign-extends the int32 refcount on RV64, so a signed branch against x0
 * implements the 0-based sign test. Cold cases call xr_jit_rc_{dup,drop}_ptr
 * via the CALL_C stub with the pointer as the extra arg. The operand R is a
 * vreg or RV64_SCRATCH_REG2 (its fallback), never RV64_SCRATCH_REG, so the
 * latter is safe as the refcount temp and slow-path func register. */
static void rv64_emit_rc_ins(Rv64CodegenCtx *ctx, XmIns *ins, bool is_release) {
    Rv64Reg R = rv64_get_operand(ctx, ins->args[0], RV64_SCRATCH_REG2);

    // BEQ R, x0, done  (null pointer → nothing to do)
    uint32_t beq_done_idx = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_beq(R, RV64_X0, 0)); /* placeholder */

    // LW t, [R + refcount]  (sign-extended)
    rv64_buf_emit(&ctx->buf, rv64_lw(RV64_SCRATCH_REG, R, (int32_t) XM_GC_HDR_REFCOUNT_OFFSET));

    // branch to slow: retain BLT t,x0 (rc<0); release BGE x0,t (0>=rc → rc<=0)
    uint32_t bslow_idx = ctx->buf.count;
    if (is_release)
        rv64_buf_emit(&ctx->buf, rv64_bge(RV64_X0, RV64_SCRATCH_REG, 0)); /* placeholder */
    else
        rv64_buf_emit(&ctx->buf, rv64_blt(RV64_SCRATCH_REG, RV64_X0, 0)); /* placeholder */

    // fast: addi t, t, +/-1; sw t, [R + refcount]
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SCRATCH_REG, RV64_SCRATCH_REG, is_release ? -1 : 1));
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, R, (int32_t) XM_GC_HDR_REFCOUNT_OFFSET));

    // J done
    uint32_t j_done_idx = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_j(0)); /* placeholder */

    // --- slow path: CALL_C to rc_{dup,drop} with pointer as extra arg ---
    uint32_t slow_idx = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_sd(R, RV64_JIT_CTX_REG, (int32_t) RV64_EXTRA_ARG_OFFSET));
    uintptr_t entry =
        xm_runtime_stub_entry(is_release ? XM_RUNTIME_STUB_rc_drop : XM_RUNTIME_STUB_rc_dup,
                              XM_RUNTIME_STUB_ABI_CALL_C_EXTRA_ARG);
    RV64_CODEGEN_CHECK(ctx, entry != 0, "rc runtime stub ABI mismatch");
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) entry);
    rv64_add_patch(ctx, RV64_PATCH_CALL_C, 0, RV64_CC_EQ, 0, 0);
    rv64_buf_emit(&ctx->buf, rv64_call(0));
    ctx->has_call_c = true;

    // done:
    uint32_t done_idx = ctx->buf.count;
    ctx->buf.code[beq_done_idx] = rv64_beq(R, RV64_X0, (int32_t) (done_idx - beq_done_idx) * 4);
    if (is_release)
        ctx->buf.code[bslow_idx] =
            rv64_bge(RV64_X0, RV64_SCRATCH_REG, (int32_t) (slow_idx - bslow_idx) * 4);
    else
        ctx->buf.code[bslow_idx] =
            rv64_blt(RV64_SCRATCH_REG, RV64_X0, (int32_t) (slow_idx - bslow_idx) * 4);
    ctx->buf.code[j_done_idx] = rv64_j((int32_t) (done_idx - j_done_idx) * 4);
}

static void rv64_h_retain(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    rv64_emit_rc_ins(ctx, ins, false);
}

static void rv64_h_release(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    rv64_emit_rc_ins(ctx, ins, true);
}

/* ========== Catch ========== */

static void rv64_h_catch(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    int16_t spill_slot = -1;
    rv64_buf_emit(&ctx->buf, rv64_ld(rd, RV64_JIT_CTX_REG, (int32_t) XM_JIT_EXCEPTION_OFFSET));
    if (rd == RV64_SCRATCH_REG && xm_ref_is_vreg(ins->dst)) {
        uint32_t dvi = XM_REF_INDEX(ins->dst);
        spill_slot = xra_vreg_spill(ctx->xra, dvi);
        if (spill_slot >= 0) {
            int32_t spill_off = -(int32_t) (RV64_SPILL_BASE + (uint32_t) spill_slot * 8);
            rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_FP, spill_off));
        }
    }
    /* Clear exception slot */
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_X0, RV64_JIT_CTX_REG, (int32_t) XM_JIT_EXCEPTION_OFFSET));
    /* Tag result as PTR */
    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t dvi = XM_REF_INDEX(ins->dst);
        if (dvi < ctx->func->nvreg && dvi < XR_JIT_MAX_VREG_TAGS) {
            int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) dvi;
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, XR_TAG_PTR);
            rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG2, RV64_JIT_CTX_REG, tag_off));
        }
    }
    if (rd == RV64_SCRATCH_REG && spill_slot >= 0) {
        int32_t spill_off = -(int32_t) (RV64_SPILL_BASE + (uint32_t) spill_slot * 8);
        rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_FP, spill_off));
    }
}

/* ========== Suspend (coroutine await/channel) ========== */

static void rv64_h_suspend(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
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

    uint32_t smap_id = rv64_record_safepoint(ctx);

    /* t6 = jit_state->suspend */
    rv64_emit_load_jit_state(ctx, RV64_SCRATCH_REG);
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_SCRATCH_REG,
                                     (int32_t) XM_JIT_STATE_SUSPEND_PTR_OFFSET));
    RV64_CODEGEN_CHECK(ctx, suspend_id < XM_MAX_SUSPEND_ENTRIES, "suspend_id out of range");

    /* Save caller-saved GP regs to suspend state */
    for (int i = 0; i < RV64_NGPR_CALLER_SAVE && i < RV64_MAX_PHYS_REGS; i++)
        rv64_buf_emit(&ctx->buf, rv64_sd(rv64_alloc_regs[i], RV64_SCRATCH_REG,
                                         (int32_t) XM_SUSPEND_CALLER_SAVED_OFF + i * 8));

    /* Save callee-saved GP regs */
    for (int i = 0; i < RV64_NGPR_CALLEE_SAVE_ALLOC; i++)
        rv64_buf_emit(&ctx->buf,
                      rv64_sd(rv64_alloc_regs[RV64_NGPR_CALLER_SAVE + i], RV64_SCRATCH_REG,
                              (int32_t) XM_SUSPEND_CALLEE_SAVED_OFF + i * 8));

    /* Save spill slots */
    {
        uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
        if (ns > XM_SUSPEND_SPILL_MAX)
            ns = XM_SUSPEND_SPILL_MAX;
        for (uint32_t s = 0; s < ns; s++) {
            int32_t frame_off = -(int32_t) (RV64_SPILL_BASE + s * 8);
            int32_t regs_off = (int32_t) (XM_SUSPEND_SPILL_OFF + s * 8);
            rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG2, RV64_FP, frame_off));
            rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG2, RV64_SCRATCH_REG, regs_off));
        }
    }

    /* Store suspend_id and smap_id */
    rv64_emit_load_jit_state(ctx, RV64_SCRATCH_REG);
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) suspend_id);
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG2, RV64_SCRATCH_REG,
                                     (int32_t) XM_JIT_STATE_SUSPEND_ID_OFFSET));
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) smap_id);
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG2, RV64_SCRATCH_REG,
                                     (int32_t) XM_JIT_STATE_SUSPEND_SMAP_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG2, RV64_JIT_CTX_REG,
                                     (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET));

    /* Pre-store resume info */
    rv64_buf_emit(&ctx->buf,
                  rv64_ld(RV64_SCRATCH_REG2, RV64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_PROTO_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG2, RV64_SCRATCH_REG,
                                     (int32_t) XM_JIT_STATE_RESUME_PROTO_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG2, RV64_SCRATCH_REG2,
                                     (int32_t) XM_PROTO_JIT_RESUME_ENTRY_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG2, RV64_SCRATCH_REG,
                                     (int32_t) XM_JIT_STATE_RESUME_ENTRY_OFFSET));

    /* Call block_helper(coro, extra_arg) via JALR */
    void *block_helper = ctx->func->suspend_block_helpers[suspend_id];
    int64_t helper_extra_arg = 0;
    if (!block_helper) {
        block_helper = xm_helper_func(XM_HELPER_await_block);
        helper_extra_arg = discard_result;
    }
    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A0, RV64_CORO_REG));
    rv64_load_imm64(&ctx->buf, RV64_A1, (uint64_t) helper_extra_arg);
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) (uintptr_t) block_helper);
    rv64_buf_emit(&ctx->buf, rv64_jalr(RV64_RA, RV64_SCRATCH_REG2, 0));

    /* Check result: DEOPT_MARKER returns to VM recovery, 0 blocks, any
     * other non-zero value continues inline. */
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG2, (uint64_t) XM_DEOPT_MARKER);
    uint32_t not_deopt_idx = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_bne(RV64_A0, RV64_SCRATCH_REG2, 0));
    rv64_load_imm64(&ctx->buf, RV64_A0, (uint64_t) XM_DEOPT_MARKER);
    rv64_emit_epilogue(ctx);
    int32_t not_deopt_off = (int32_t) (ctx->buf.count - not_deopt_idx) * 4;
    ctx->buf.code[not_deopt_idx] = rv64_bne(RV64_A0, RV64_SCRATCH_REG2, not_deopt_off);

    /* Check result: a0 == 0 -> blocked (return SUSPEND_MARKER),
     * a0 != 0 -> inline resume */
    uint32_t bne_idx = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_bne(RV64_A0, RV64_X0, 0)); /* placeholder, patched below */

    /* Blocked path: return SUSPEND_MARKER via epilogue */
    rv64_load_imm64(&ctx->buf, RV64_A0, (uint64_t) XM_SUSPEND_MARKER);
    rv64_emit_epilogue(ctx);

    /* Patch BNE to skip to not-blocked path */
    int32_t bne_off = (int32_t) (ctx->buf.count - bne_idx) * 4;
    ctx->buf.code[bne_idx] = rv64_bne(RV64_A0, RV64_X0, bne_off);

    /* Not-blocked: reload suspend pointer (clobbered by CALL) */
    rv64_emit_load_jit_state(ctx, RV64_SCRATCH_REG);
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_SCRATCH_REG,
                                     (int32_t) XM_JIT_STATE_SUSPEND_PTR_OFFSET));

    /* Reload caller-saved GP regs */
    for (int i = 0; i < RV64_NGPR_CALLER_SAVE && i < RV64_MAX_PHYS_REGS; i++)
        rv64_buf_emit(&ctx->buf, rv64_ld(rv64_alloc_regs[i], RV64_SCRATCH_REG,
                                         (int32_t) XM_SUSPEND_CALLER_SAVED_OFF + i * 8));

    /* Load await/channel result into dst */
    if (rd != RV64_SCRATCH_REG) {
        rv64_buf_emit(&ctx->buf, rv64_ld(rd, RV64_SCRATCH_REG, (int32_t) XM_SUSPEND_RESULT_OFF));
    }

    /* Load result_tag -> runtime_tags[vreg] */
    {
        int32_t res_vreg_off = -1;
        int16_t res_bc_slot = -1;
        if (xm_ref_is_vreg(ins->dst)) {
            uint32_t vi = XM_REF_INDEX(ins->dst);
            if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS)
                res_vreg_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
            if (vi < ctx->func->nvreg)
                res_bc_slot = ctx->func->vregs[vi].bc_slot;
        }
        if (res_vreg_off >= 0) {
            rv64_buf_emit(&ctx->buf, rv64_lbu(RV64_SCRATCH_REG2, RV64_SCRATCH_REG,
                                              (int32_t) XM_SUSPEND_RESULT_TAG_OFF));
            rv64_buf_emit(&ctx->buf, rv64_sb(RV64_SCRATCH_REG2, RV64_JIT_CTX_REG, res_vreg_off));
        }
        if (suspend_id < XM_MAX_SUSPEND_ENTRIES) {
            ctx->suspend_result_bc_slots[suspend_id] = res_bc_slot;
            ctx->suspend_result_tag_offs[suspend_id] = res_vreg_off;
        }
    }

    /* Record continuation for resume entry jump table */
    if (suspend_id < XM_MAX_SUSPEND_ENTRIES) {
        ctx->suspend_cont_offsets[suspend_id] = rv64_buf_offset(&ctx->buf);
        ctx->suspend_smap_ids[suspend_id] = smap_id;
        ctx->suspend_result_regs[suspend_id] =
            (uint8_t) (xm_ref_is_vreg(ins->dst) ? rd : RV64_SCRATCH_REG);
        if (suspend_id >= ctx->nsuspend)
            ctx->nsuspend = suspend_id + 1;
    }
}

/* ========== Float sub-word load/store ========== */

static void rv64_h_f32(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    (void) rd;
    /* f32 load/store are rare in practice; bail to interpreter for now */
    rv64_emit_deopt_id(ctx, ins);
    rv64_emit_deopt_jmp(ctx);
}

/* ========== Call Handler Wrapper ========== */

static void rv64_h_call(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd) {
    bool handled = rv64_emit_call_ins(ctx, ins, rd);
    RV64_CODEGEN_CHECK(ctx, handled, "rv64_h_call: unhandled call opcode");
}

/* ========== Stub Handlers (to be implemented) ========== */

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
    [XM_LOAD_F32] = rv64_h_f32,
    [XM_STORE_F32] = rv64_h_f32,

    /* Coro/context */
    [XM_LOAD_CORO] = rv64_h_coro,
    [XM_LOAD_CORO_BYTE] = rv64_h_coro,
    [XM_STORE_CORO] = rv64_h_coro,
    [XM_STORE_CORO_BYTE] = rv64_h_coro,

    /* Field access */
    [XM_LOAD_FIELD] = rv64_h_field_load,
    [XM_STORE_FIELD] = rv64_h_field_store,

    /* Tagged value / box / unbox */
    [XM_TAG_LOAD] = rv64_h_tag_load,
    [XM_BOX_I64] = rv64_h_box,
    [XM_BOX_F64] = rv64_h_box,
    [XM_UNBOX_I64] = rv64_h_unbox_i64,
    [XM_UNBOX_F64] = rv64_h_unbox_f64,

    /* Guards / deopt */
    [XM_GUARD_TAG] = rv64_h_guard,
    [XM_GUARD_BOUNDS] = rv64_h_guard,
    [XM_GUARD_NONNULL] = rv64_h_guard,
    [XM_GUARD_CLASS] = rv64_h_guard,
    [XM_GUARD_KLASS] = rv64_h_guard,
    [XM_TAG_CHECK] = rv64_h_guard,
    [XM_DEOPT] = rv64_h_deopt,
    [XM_SAFEPOINT] = rv64_h_safepoint,

    /* Runtime helpers */
    [XM_RT_ADD] = rv64_h_rt_arith,
    [XM_RT_SUB] = rv64_h_rt_arith,
    [XM_RT_MUL] = rv64_h_rt_arith,
    [XM_RT_DIV] = rv64_h_rt_arith,
    [XM_RT_MOD] = rv64_h_rt_arith,
    [XM_RT_UNM] = rv64_h_rt_unm,
    [XM_RT_LT] = rv64_h_rt_cmp,
    [XM_RT_LE] = rv64_h_rt_cmp,
    [XM_RT_EQ] = rv64_h_rt_cmp,
    [XM_RT_ARRAY_NEW] = rv64_h_rt_collection,
    [XM_RT_MAP_NEW] = rv64_h_rt_collection,
    [XM_RT_ARRAY_PUSH] = rv64_h_rt_array_push,
    [XM_RT_PRINT] = rv64_h_rt_simple,
    [XM_RT_ARRAY_LEN] = rv64_h_rt_simple,
    [XM_RT_INDEX_GET] = rv64_h_rt_simple,
    [XM_RT_INDEX_SET] = rv64_h_rt_simple,
    [XM_RT_ISNULL] = rv64_h_rt_simple,

    /* Exception / coroutine — no-op or stub */
    [XM_TRY_BEGIN] = rv64_h_nop,
    [XM_TRY_END] = rv64_h_nop,
    [XM_THROW] = rv64_h_nop,
    [XM_NOP] = rv64_h_nop,
    [XM_PHI] = rv64_h_nop,
    [XM_SUSPEND] = rv64_h_suspend,
    [XM_BARRIER_FWD] = rv64_h_barrier_fwd,
    [XM_BARRIER_BACK] = rv64_h_barrier_back,
    [XM_ALLOC] = rv64_h_alloc,
    [XM_RETAIN] = rv64_h_retain,
    [XM_RELEASE] = rv64_h_release,
    [XM_CATCH] = rv64_h_catch,

    /* Calls */
    [XM_CALL_C] = rv64_h_call,
    [XM_CALL_C_LEAF] = rv64_h_call,
    [XM_CALL_SELF_DIRECT] = rv64_h_call,
    [XM_CALL_KNOWN] = rv64_h_call,
    [XM_CALL_KNOWN_REG] = rv64_h_call,
    [XM_CALL_METHOD_KNOWN] = rv64_h_call,
    [XM_CALL_DIRECT] = rv64_h_call,
    [XM_CALL] = rv64_h_call,

    /* Return */
    [XM_RET] = rv64_h_ret,
};

/* ========== Instruction Dispatch ========== */

/* Ops that legitimately emit 0 bytes (metadata-only or resolved elsewhere) */
static bool rv64_op_allows_zero_emit(XmOp op) {
    switch (op) {
        case XM_NOP:
        case XM_PHI:
        case XM_TRY_BEGIN:
        case XM_TRY_END:
        case XM_THROW:
        case XM_SELECT_COND:
            return true;
        default:
            return false;
    }
}

XR_FUNC void rv64_emit_xm_ins(Rv64CodegenCtx *ctx, XmIns *ins) {
    RV64_CODEGEN_CHECK(ctx, ctx != NULL, "rv64_emit_xm_ins: NULL ctx");
    RV64_CODEGEN_CHECK(ctx, ins != NULL, "rv64_emit_xm_ins: NULL ins");
    Rv64Reg rd = rv64_get_reg(ctx, ins->dst);

    RV64_CODEGEN_CHECK(ctx, ins->op < XM_OP_COUNT, "rv64_emit_xm_ins: op out of range");
    const XmDispatchMeta *meta = xm_dispatch_meta_find((XmOp) ins->op, XM_DISPATCH_BACKEND_RISCV64);
    if (!meta) {
        xr_log_warning("rv64-cg", "missing generated dispatch metadata for %s (%u)",
                       xm_op_name(ins->op), (uint32_t) ins->op);
        RV64_CODEGEN_CHECK(ctx, false, "missing generated dispatch metadata");
    }
    Rv64InsHandler handler = rv64_ins_handlers[ins->op];
    if (handler) {
        uint32_t count_before = ctx->buf.count;
        handler(ctx, ins, rd);
        /* Instruction self-check: non-metadata ops must emit at least one instruction. */
        if (ctx->buf.count == count_before && !rv64_op_allows_zero_emit(ins->op)) {
            xr_log_warning("rv64-cg", "handler emitted 0 bytes for %s; declared mcinsns=%s",
                           xm_op_name(ins->op), meta->mcinsns);
            RV64_CODEGEN_CHECK(ctx, false, "rv64 handler emitted 0 bytes for non-metadata op");
        }
    } else {
        xr_log_warning("rv64-cg", "unhandled %s in generated dispatch metadata; mcinsns=%s",
                       xm_op_name(ins->op), meta->mcinsns);
        RV64_CODEGEN_CHECK(ctx, false, "unhandled Xm opcode in riscv64 backend");
    }
}

#endif /* __riscv */
