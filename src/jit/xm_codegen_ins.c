/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_ins.c - Per-instruction ARM64 code emission (table-driven)
 *
 * Contains a64_emit_xm_ins() and its handler functions for translating
 * individual Xm SSA instructions into ARM64 machine code.  Split from
 * xm_codegen.c to keep each file under 3000 lines.
 */

#ifdef __aarch64__

#include "xm_codegen_internal.h"
#include "xm_dispatch_meta.h"
#include "xm_dispatch_emit_gen.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"

/* ========== Handler type ========== */
typedef void (*A64InsHandler)(CodegenCtx *ctx, XmIns *ins, A64Reg rd);

/* ========== Helpers ========== */

/* Try to extract a small immediate (0..4095) from an Xm operand.
 * Returns true if the operand is a vreg defined by XM_CONST_I64
 * with a value that fits in ARM64 add_imm/sub_imm (12-bit unsigned). */
static bool try_get_imm12(CodegenCtx *ctx, XmRef ref, uint32_t *out_imm) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t vi = XM_REF_INDEX(ref);
    if (vi >= ctx->func->nvreg)
        return false;
    XmIns *def = ctx->func->vregs[vi].def;
    if (!def || def->op != XM_CONST_I64)
        return false;
    if (!xm_ref_is_const(def->args[0]))
        return false;
    uint32_t ci = XM_REF_INDEX(def->args[0]);
    if (ci >= ctx->func->nconst)
        return false;
    int64_t val = ctx->func->consts[ci].val.i64;
    if (val >= 0 && val <= 4095) {
        *out_imm = (uint32_t) val;
        return true;
    }
    return false;
}

static bool a64_meta_allows_zero_emit(const XmDispatchMeta *meta) {
    return meta->mcinsn_count == 0;
}

static bool a64_check_emit_progress(CodegenCtx *ctx, XmIns *ins, const XmDispatchMeta *meta,
                                    uint32_t count_before) {
    if (ctx->buf.count != count_before || a64_meta_allows_zero_emit(meta))
        return true;
    xr_log_warning("jit", "handler emitted 0 instructions for %s; declared mcinsns=%s",
                   xm_op_name(ins->op), meta->mcinsns);
    ctx->had_error = true;
    return false;
}

static bool a64_emit_generated_gp_rrr(CodegenCtx *ctx, XmIns *ins, A64Reg rd, A64Reg rn,
                                      A64Reg rm) {
    if (xm_dispatch_emit_arm64_gp_rrr(ins->op, &ctx->buf, rd, rn, rm))
        return true;
    xr_log_warning("jit", "generated ARM64 gp_rrr dispatch rejected %s", xm_op_name(ins->op));
    ctx->had_error = true;
    return false;
}

static bool a64_emit_generated_cmp_rr_cc(CodegenCtx *ctx, XmIns *ins, A64Reg rd, A64Reg rn,
                                         A64Reg rm) {
    if (xm_dispatch_emit_arm64_cmp_rr_cc(ins->op, &ctx->buf, rd, rn, rm))
        return true;
    xr_log_warning("jit", "generated ARM64 cmp_rr_cc dispatch rejected %s", xm_op_name(ins->op));
    ctx->had_error = true;
    return false;
}

/* ========== Integer Arithmetic Handlers ========== */

static void a64_h_add(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    uint32_t imm;
    if (try_get_imm12(ctx, ins->args[1], &imm)) {
        A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
        a64_buf_emit(&ctx->buf, a64_add_imm(rd, rn, imm));
    } else if (try_get_imm12(ctx, ins->args[0], &imm)) {
        A64Reg rn = xra_operand(ctx, ins->args[1], SCRATCH_REG);
        a64_buf_emit(&ctx->buf, a64_add_imm(rd, rn, imm));
    } else {
        A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
        A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
        if (!a64_emit_generated_gp_rrr(ctx, ins, rd, rn, rm))
            return;
    }
}

static void a64_h_sub(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    uint32_t imm;
    if (try_get_imm12(ctx, ins->args[1], &imm)) {
        A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
        a64_buf_emit(&ctx->buf, a64_sub_imm(rd, rn, imm));
    } else {
        A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
        A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
        if (!a64_emit_generated_gp_rrr(ctx, ins, rd, rn, rm))
            return;
    }
}

static void a64_h_mul(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_gp_rrr(ctx, ins, rd, rn, rm);
}

static void a64_h_div(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    /* Division by zero: ARM64 SDIV returns 0 silently — deopt instead */
    add_patch(ctx, PATCH_DEOPT_CBZ, 0, rm);
    a64_buf_emit(&ctx->buf, a64_nop()); /* patched to CBZ rm, deopt */
    ctx->has_deopt = true;
    (void) a64_emit_generated_gp_rrr(ctx, ins, rd, rn, rm);
}

static void a64_h_mod(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    /* ARM64 has no MOD: dst = a - (a / b) * b */
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    add_patch(ctx, PATCH_DEOPT_CBZ, 0, rm);
    a64_buf_emit(&ctx->buf, a64_nop());
    ctx->has_deopt = true;
    a64_buf_emit(&ctx->buf, a64_sdiv(SCRATCH_REG, rn, rm));
    a64_buf_emit(&ctx->buf, a64_msub(rd, SCRATCH_REG, rm, rn));
}

static bool a64_emit_generated_gp_r(CodegenCtx *ctx, XmIns *ins, A64Reg rd, A64Reg rm) {
    if (xm_dispatch_emit_arm64_gp_r(ins->op, &ctx->buf, rd, rm))
        return true;
    xr_log_warning("jit", "generated ARM64 gp_r dispatch rejected %s", xm_op_name(ins->op));
    ctx->had_error = true;
    return false;
}

static void a64_h_neg(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rm = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    (void) a64_emit_generated_gp_r(ctx, ins, rd, rm);
}

/* ========== Bitwise / Shift ========== */

static void a64_h_and(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_gp_rrr(ctx, ins, rd, rn, rm);
}

static void a64_h_or(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_gp_rrr(ctx, ins, rd, rn, rm);
}

static void a64_h_xor(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_gp_rrr(ctx, ins, rd, rn, rm);
}

static void a64_h_not(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rm = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    (void) a64_emit_generated_gp_r(ctx, ins, rd, rm);
}

static void a64_h_shl(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_gp_rrr(ctx, ins, rd, rn, rm);
}

static void a64_h_shr(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_gp_rrr(ctx, ins, rd, rn, rm);
}

/* ========== Float Arithmetic ========== */

static bool a64_emit_generated_fp_rrr(CodegenCtx *ctx, XmIns *ins, A64Reg fd, A64Reg fn,
                                      A64Reg fm) {
    if (xm_dispatch_emit_arm64_fp_rrr(ins->op, &ctx->buf, fd, fn, fm))
        return true;
    xr_log_warning("jit", "generated ARM64 fp_rrr dispatch rejected %s", xm_op_name(ins->op));
    ctx->had_error = true;
    return false;
}

static void a64_h_fadd(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_fp_rrr(ctx, ins, rd, dn, dm);
}

static void a64_h_fsub(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_fp_rrr(ctx, ins, rd, dn, dm);
}

static void a64_h_fmul(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_fp_rrr(ctx, ins, rd, dn, dm);
}

static void a64_h_fdiv(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    (void) a64_emit_generated_fp_rrr(ctx, ins, rd, dn, dm);
}

static bool a64_emit_generated_fp_r(CodegenCtx *ctx, XmIns *ins, A64Reg fd, A64Reg fn) {
    if (xm_dispatch_emit_arm64_fp_r(ins->op, &ctx->buf, fd, fn))
        return true;
    xr_log_warning("jit", "generated ARM64 fp_r dispatch rejected %s", xm_op_name(ins->op));
    ctx->had_error = true;
    return false;
}

static void a64_h_fneg(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    (void) a64_emit_generated_fp_r(ctx, ins, rd, dn);
}

/* ========== Type Conversion ========== */

static bool a64_emit_generated_conv_i2f(CodegenCtx *ctx, XmIns *ins, A64Reg fd, A64Reg rn) {
    if (xm_dispatch_emit_arm64_conv_i2f(ins->op, &ctx->buf, fd, rn))
        return true;
    xr_log_warning("jit", "generated ARM64 conv_i2f dispatch rejected %s", xm_op_name(ins->op));
    ctx->had_error = true;
    return false;
}

static bool a64_emit_generated_conv_f2i(CodegenCtx *ctx, XmIns *ins, A64Reg rd, A64Reg fn) {
    if (xm_dispatch_emit_arm64_conv_f2i(ins->op, &ctx->buf, rd, fn))
        return true;
    xr_log_warning("jit", "generated ARM64 conv_f2i dispatch rejected %s", xm_op_name(ins->op));
    ctx->had_error = true;
    return false;
}

static void a64_h_i2f(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    (void) a64_emit_generated_conv_i2f(ctx, ins, rd, rn);
}

static void a64_h_f2i(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    (void) a64_emit_generated_conv_f2i(ctx, ins, rd, dn);
}

/* ========== Float Comparison ========== */

static void a64_h_cmp_float(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    A64Reg tmp = SCRATCH_REG2;

    /* Handle alias: if rd aliases SCRATCH_REG2, use SCRATCH_REG as tmp */
    if (rd == SCRATCH_REG2) {
        tmp = SCRATCH_REG;
        XR_DCHECK(rd != tmp, "a64_h_cmp_float: rd aliases both scratch regs");
    }

    /* CMP+BR fusion: skip CSET when fused with BR terminator */
    if (!xm_ref_is_none(ctx->fused_cmp_ref) && xm_ref_is_vreg(ins->dst) &&
        ins->dst == ctx->fused_cmp_ref) {
        a64_buf_emit(&ctx->buf, a64_fcmp(dn, dm));
        return;
    }

    if (!xm_dispatch_emit_arm64_fcmp_rr_cc(ins->op, &ctx->buf, rd, dn, dm, tmp)) {
        xr_log_warning("jit", "generated ARM64 fcmp_rr_cc dispatch rejected %s",
                       xm_op_name(ins->op));
        ctx->had_error = true;
    }
}

/* ========== Integer Comparison ========== */

static void a64_h_cmp_int(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    /* CMP+BR fusion: skip CSET when fused with BR terminator */
    if (!xm_ref_is_none(ctx->fused_cmp_ref) && xm_ref_is_vreg(ins->dst) &&
        ins->dst == ctx->fused_cmp_ref) {
        a64_buf_emit(&ctx->buf, a64_cmp(rn, rm));
        return;
    }
    (void) a64_emit_generated_cmp_rr_cc(ctx, ins, rd, rn, rm);
}

/* ========== Constants ========== */

static void a64_h_const(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    emit_load_const(ctx, rd, ins->args[0]);
}

static void a64_h_const_f64(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    if (xm_ref_is_const(ins->args[0])) {
        uint32_t ci = XM_REF_INDEX(ins->args[0]);
        XR_DCHECK(ci < ctx->func->nconst, "CONST_F64: const OOB");
        double val;
        uint64_t raw = (uint64_t) ctx->func->consts[ci].val.raw;
        memcpy(&val, &raw, 8);
        a64_load_f64(&ctx->buf, rd, SCRATCH_REG, val);
    } else {
        A64Reg src = xra_arg(ctx, ins->args[0], SCRATCH_REG);
        if (src != rd)
            a64_buf_emit(&ctx->buf, a64_fmov(rd, src));
    }
}

/* ========== Select (conditional move) ========== */

static void a64_h_select_cond(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    (void) rd;
    A64Reg cond_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    a64_buf_emit(&ctx->buf, a64_cmp_imm(cond_reg, 0));
}

static void a64_h_select(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    if (ins->rep == XR_REP_F64) {
        A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
        A64Reg rm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
        a64_buf_emit(&ctx->buf, a64_fcsel(rd, rn, rm, A64_CC_NE));
    } else {
        A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
        A64Reg rm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
        a64_buf_emit(&ctx->buf, a64_csel(rd, rn, rm, A64_CC_NE));
    }
}

/* ========== Move / Redefine ========== */

static void a64_h_mov(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    if (rd != rn) {
        if (ins->rep == XR_REP_F64)
            a64_buf_emit(&ctx->buf, a64_fmov(rd, rn));
        else if (!xm_dispatch_emit_arm64_gp_r(ins->op, &ctx->buf, rd, rn))
            ctx->had_error = true;
    }
}

/* ========== Box / Unbox ========== */

static void a64_h_box(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    if (ins->op == XM_BOX_F64) {
        // FP register → GP register: must use FMOV to transfer float bits
        a64_buf_emit(&ctx->buf, a64_fmov_to_gpr(rd, rn));
    } else if (rd != rn) {
        a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
    }
}

static void a64_h_unbox_i64(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    uint8_t src_type = XR_REP_I64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t vi = XM_REF_INDEX(ins->args[0]);
        if (vi < ctx->func->nvreg)
            src_type = ctx->func->vregs[vi].rep;
    }
    if (src_type == XR_REP_PTR) {
        A64Reg ptr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
        a64_buf_emit(&ctx->buf, a64_ldr(rd, ptr, XM_XRVALUE_PAYLOAD_OFFSET));
    } else {
        A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
        if (rd != rn)
            a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
    }
}

static void a64_h_unbox_f64(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    uint8_t src_type = XR_REP_F64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t vi = XM_REF_INDEX(ins->args[0]);
        if (vi < ctx->func->nvreg)
            src_type = ctx->func->vregs[vi].rep;
    }
    if (src_type == XR_REP_PTR) {
        A64Reg ptr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
        a64_buf_emit(&ctx->buf, a64_ldr_fp(rd, ptr, XM_XRVALUE_PAYLOAD_OFFSET));
    } else if (src_type == XR_REP_F64) {
        // FP → FP: use fmov between FP registers
        A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
        if (rd != rn)
            a64_buf_emit(&ctx->buf, a64_fmov(rd, rn));
    } else {
        // GP (TAGGED/I64) → FP: use fmov to transfer bits across domains
        A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
        a64_buf_emit(&ctx->buf, a64_fmov_gp_to_fp(rd, rn));
    }
}

/* ========== Pseudo / Marker Handlers ==========
 * NOP emits a real ARM64 NOP because generated metadata declares arm64.nop.
 * Marker ops emit no machine code: PHI is resolved by register allocator edge
 * copies; TRY_BEGIN / TRY_END are EH markers; THROW is lowered through CALL_C. */
static void a64_h_nop(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    (void) ins;
    (void) rd;
    a64_buf_emit(&ctx->buf, a64_nop());
}

static void a64_h_marker(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    (void) ctx;
    (void) ins;
    (void) rd;
}

/* ========== Dispatch Table ========== */

static const A64InsHandler a64_ins_handlers[XM_OP_COUNT] = {
    [XM_ADD] = a64_h_add,
    [XM_SUB] = a64_h_sub,
    [XM_MUL] = a64_h_mul,
    [XM_DIV] = a64_h_div,
    [XM_MOD] = a64_h_mod,
    [XM_NEG] = a64_h_neg,
    [XM_AND] = a64_h_and,
    [XM_OR] = a64_h_or,
    [XM_XOR] = a64_h_xor,
    [XM_NOT] = a64_h_not,
    [XM_SHL] = a64_h_shl,
    [XM_SHR] = a64_h_shr,
    [XM_FADD] = a64_h_fadd,
    [XM_FSUB] = a64_h_fsub,
    [XM_FMUL] = a64_h_fmul,
    [XM_FDIV] = a64_h_fdiv,
    [XM_FNEG] = a64_h_fneg,
    [XM_I2F] = a64_h_i2f,
    [XM_F2I] = a64_h_f2i,
    [XM_FEQ] = a64_h_cmp_float,
    [XM_FNE] = a64_h_cmp_float,
    [XM_FLT] = a64_h_cmp_float,
    [XM_FLE] = a64_h_cmp_float,
    [XM_LT] = a64_h_cmp_int,
    [XM_LE] = a64_h_cmp_int,
    [XM_GT] = a64_h_cmp_int,
    [XM_GE] = a64_h_cmp_int,
    [XM_EQ] = a64_h_cmp_int,
    [XM_NE] = a64_h_cmp_int,
    [XM_CONST_I64] = a64_h_const,
    [XM_CONST_PTR] = a64_h_const,
    [XM_CONST_F64] = a64_h_const_f64,
    [XM_SELECT_COND] = a64_h_select_cond,
    [XM_SELECT] = a64_h_select,
    [XM_MOV] = a64_h_mov,
    [XM_REDEFINE] = a64_h_mov,
    [XM_BOX_I64] = a64_h_box,
    [XM_BOX_F64] = a64_h_box,
    [XM_UNBOX_I64] = a64_h_unbox_i64,
    [XM_UNBOX_F64] = a64_h_unbox_f64,
    [XM_NOP] = a64_h_nop,
    /* Pseudo / marker ops that emit no code on arm64 (see a64_h_nop above) */
    [XM_PHI] = a64_h_marker,
    [XM_TRY_BEGIN] = a64_h_marker,
    [XM_TRY_END] = a64_h_marker,
    [XM_THROW] = a64_h_marker,
    /* All other opcodes (mem, call, etc.) handled by fallback chain */
};

/* ========== Instruction Dispatch ========== */

XR_FUNC void a64_emit_xm_ins(CodegenCtx *ctx, XmIns *ins) {
    XR_DCHECK(ctx != NULL, "a64_emit_xm_ins: NULL ctx");
    XR_DCHECK(ins != NULL, "a64_emit_xm_ins: NULL ins");
    A64Reg rd = xra_get(ctx, ins->dst);

    if (ins->op < XM_OP_COUNT) {
        const XmDispatchMeta *meta =
            xm_dispatch_meta_find((XmOp) ins->op, XM_DISPATCH_BACKEND_ARM64);
        if (!meta) {
            xr_log_warning("jit", "missing generated dispatch metadata for %s (%u)",
                           xm_op_name(ins->op), (uint32_t) ins->op);
            ctx->had_error = true;
            return;
        }
        A64InsHandler handler = a64_ins_handlers[ins->op];
        if (handler) {
            uint32_t count_before = ctx->buf.count;
            handler(ctx, ins, rd);
            a64_check_emit_progress(ctx, ins, meta, count_before);
            return;
        }

        uint32_t count_before = ctx->buf.count;
        if (xm_emit_call_ops(ctx, ins, rd)) {
            a64_check_emit_progress(ctx, ins, meta, count_before);
            return;
        }
        count_before = ctx->buf.count;
        if (xm_emit_mem_ops(ctx, ins, rd)) {
            a64_check_emit_progress(ctx, ins, meta, count_before);
            return;
        }
        xr_log_warning("jit", "unhandled Xm opcode %s in a64_emit_xm_ins; mcinsns=%s",
                       xm_op_name(ins->op), meta->mcinsns);
        ctx->had_error = true;
        return;
    }

    xr_log_warning("jit", "Xm opcode %u out of range in a64_emit_xm_ins", (uint32_t) ins->op);
    ctx->had_error = true;
}

#endif /* __aarch64__ */
