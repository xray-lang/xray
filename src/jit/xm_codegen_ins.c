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
        a64_buf_emit(&ctx->buf, a64_add(rd, rn, rm));
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
        a64_buf_emit(&ctx->buf, a64_sub(rd, rn, rm));
    }
}

static void a64_h_mul(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_mul(rd, rn, rm));
}

static void a64_h_div(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    /* Division by zero: ARM64 SDIV returns 0 silently — deopt instead */
    add_patch(ctx, PATCH_DEOPT_CBZ, 0, rm);
    a64_buf_emit(&ctx->buf, a64_nop());  /* patched to CBZ rm, deopt */
    ctx->has_deopt = true;
    a64_buf_emit(&ctx->buf, a64_sdiv(rd, rn, rm));
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

static void a64_h_neg(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rm = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    a64_buf_emit(&ctx->buf, a64_neg(rd, rm));
}

/* ========== Bitwise / Shift ========== */

static void a64_h_and(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_and(rd, rn, rm));
}

static void a64_h_or(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_orr(rd, rn, rm));
}

static void a64_h_xor(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_eor(rd, rn, rm));
}

static void a64_h_not(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rm = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    a64_buf_emit(&ctx->buf, a64_mvn(rd, rm));
}

static void a64_h_shl(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_lsl(rd, rn, rm));
}

static void a64_h_shr(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_asr(rd, rn, rm));
}

/* ========== Float Arithmetic ========== */

static void a64_h_fadd(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_fadd(rd, dn, dm));
}

static void a64_h_fsub(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_fsub(rd, dn, dm));
}

static void a64_h_fmul(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_fmul(rd, dn, dm));
}

static void a64_h_fdiv(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_fdiv(rd, dn, dm));
}

static void a64_h_fneg(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    a64_buf_emit(&ctx->buf, a64_fneg(rd, dn));
}

/* ========== Type Conversion ========== */

static void a64_h_i2f(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    a64_buf_emit(&ctx->buf, a64_scvtf(rd, rn));
}

static void a64_h_f2i(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    a64_buf_emit(&ctx->buf, a64_fcvtzs(rd, dn));
}

/* ========== Float Comparison ========== */

static void a64_h_cmp_float(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_fcmp(dn, dm));
    /* CMP+BR fusion: skip CSET when fused with BR terminator */
    if (!xm_ref_is_none(ctx->fused_cmp_ref) && xm_ref_is_vreg(ins->dst) &&
        ins->dst == ctx->fused_cmp_ref)
        return;
    A64Cond cc;
    switch (ins->op) {
        case XM_FEQ: cc = A64_CC_EQ; break;
        case XM_FNE: cc = A64_CC_NE; break;
        case XM_FLT: cc = A64_CC_MI; break;
        case XM_FLE: cc = A64_CC_LS; break;
        default:     cc = A64_CC_AL; break;
    }
    a64_buf_emit(&ctx->buf, a64_cset(rd, cc));
}

/* ========== Integer Comparison ========== */

static void a64_h_cmp_int(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
    A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
    a64_buf_emit(&ctx->buf, a64_cmp(rn, rm));
    /* CMP+BR fusion: skip CSET when fused with BR terminator */
    if (!xm_ref_is_none(ctx->fused_cmp_ref) && xm_ref_is_vreg(ins->dst) &&
        ins->dst == ctx->fused_cmp_ref)
        return;
    A64Cond cc;
    switch (ins->op) {
        case XM_LT: cc = A64_CC_LT; break;
        case XM_LE: cc = A64_CC_LE; break;
        case XM_GT: cc = A64_CC_GT; break;
        case XM_GE: cc = A64_CC_GE; break;
        case XM_EQ: cc = A64_CC_EQ; break;
        case XM_NE: cc = A64_CC_NE; break;
        default:    cc = A64_CC_AL; break;
    }
    a64_buf_emit(&ctx->buf, a64_cset(rd, cc));
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
    (void)rd;
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
        else
            a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
    }
}

/* ========== Box / Unbox ========== */

static void a64_h_box(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
    if (rd != rn)
        a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
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
    } else {
        A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
        if (rd != rn)
            a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
    }
}

/* ========== Dispatch Table ========== */

static const A64InsHandler a64_ins_handlers[XM_OP_COUNT] = {
    [XM_ADD]          = a64_h_add,
    [XM_SUB]          = a64_h_sub,
    [XM_MUL]          = a64_h_mul,
    [XM_DIV]          = a64_h_div,
    [XM_MOD]          = a64_h_mod,
    [XM_NEG]          = a64_h_neg,
    [XM_AND]          = a64_h_and,
    [XM_OR]           = a64_h_or,
    [XM_XOR]          = a64_h_xor,
    [XM_NOT]          = a64_h_not,
    [XM_SHL]          = a64_h_shl,
    [XM_SHR]          = a64_h_shr,
    [XM_FADD]         = a64_h_fadd,
    [XM_FSUB]         = a64_h_fsub,
    [XM_FMUL]         = a64_h_fmul,
    [XM_FDIV]         = a64_h_fdiv,
    [XM_FNEG]         = a64_h_fneg,
    [XM_I2F]          = a64_h_i2f,
    [XM_F2I]          = a64_h_f2i,
    [XM_FEQ]          = a64_h_cmp_float,
    [XM_FNE]          = a64_h_cmp_float,
    [XM_FLT]          = a64_h_cmp_float,
    [XM_FLE]          = a64_h_cmp_float,
    [XM_LT]           = a64_h_cmp_int,
    [XM_LE]           = a64_h_cmp_int,
    [XM_GT]           = a64_h_cmp_int,
    [XM_GE]           = a64_h_cmp_int,
    [XM_EQ]           = a64_h_cmp_int,
    [XM_NE]           = a64_h_cmp_int,
    [XM_CONST_I64]    = a64_h_const,
    [XM_CONST_PTR]    = a64_h_const,
    [XM_CONST_F64]    = a64_h_const_f64,
    [XM_SELECT_COND]  = a64_h_select_cond,
    [XM_SELECT]       = a64_h_select,
    [XM_MOV]          = a64_h_mov,
    [XM_REDEFINE]     = a64_h_mov,
    [XM_BOX_I64]      = a64_h_box,
    [XM_BOX_F64]      = a64_h_box,
    [XM_UNBOX_I64]    = a64_h_unbox_i64,
    [XM_UNBOX_F64]    = a64_h_unbox_f64,
    /* All other opcodes (mem, call, etc.) handled by fallback chain */
};

/* ========== Instruction Dispatch ========== */

XR_FUNC void a64_emit_xm_ins(CodegenCtx *ctx, XmIns *ins) {
    XR_DCHECK(ctx != NULL, "a64_emit_xm_ins: NULL ctx");
    XR_DCHECK(ins != NULL, "a64_emit_xm_ins: NULL ins");
    A64Reg rd = xra_get(ctx, ins->dst);

    if (ins->op >= 0 && ins->op < XM_OP_COUNT) {
        A64InsHandler handler = a64_ins_handlers[ins->op];
        if (handler) {
            handler(ctx, ins, rd);
            return;
        }
    }

    /* Fallback: delegate to sub-emit functions (call ops, mem ops) */
    if (xm_emit_call_ops(ctx, ins, rd))
        return;
    if (xm_emit_mem_ops(ctx, ins, rd))
        return;
    xr_log_warning("jit", "unhandled Xm opcode %d in a64_emit_xm_ins", ins->op);
    a64_buf_emit(&ctx->buf, a64_nop());
}

#endif  /* __aarch64__ */
