/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_x64_ins.c - Per-instruction x86-64 code emission
 *
 * Contains x64_emit_xm_ins() and its helper functions for translating
 * individual Xm SSA instructions into x86-64 machine code.  Split from
 * xm_codegen_x64.c to keep each file under 3000 lines.
 */

#if defined(__x86_64__) || defined(_M_X64)

#include "xm_codegen_x64_internal.h"
#include "xm_dispatch_meta.h"
#include "xm_dispatch_emit_gen.h"
#include "xm_helper_table.h"
#include "xm_jit_runtime.h"
#include "../coro/xcoroutine.h"
#include <string.h>

/* ========== Instruction Emission ========== */

/*
 * x86-64 2-operand pattern: dst = dst OP src.
 * If dst != src1, emit MOV dst, src1 first.
 * Returns the hardware register holding src1 (== dst after MOV).
 */
static X64Reg x64_ensure_dst(X64CodegenCtx *ctx, X64Reg rd, XmRef arg0, X64Reg scratch) {
    X64Reg rn = x64_get_operand(ctx, arg0, scratch);
    if (rn != rd)
        x64_mov_rr(&ctx->buf, rd, rn);
    return rd;
}

/* ========== Scratch-Probe Helpers ==========
 *
 * Decide whether materializing a binop operand will need to land in the
 * single GP / FP scratch register. Used by the binop helpers below to
 * detect the "both args clobber scratch" hazard before emitting a second
 * x64_get_operand load (which would silently overwrite the first arg). */

static int8_t x64_probe_phys_reg(X64CodegenCtx *ctx, uint32_t idx) {
    if (ctx->vreg_override && idx < ctx->xra->nvreg && ctx->vreg_override[idx] != -128)
        return ctx->vreg_override[idx];
    int8_t ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos);
    if (ri < 0)
        ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    return ri;
}

static bool x64_arg_needs_scratch_gp(X64CodegenCtx *ctx, XmRef ref) {
    if (xm_ref_is_const(ref))
        return true;
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= ctx->func->nvreg)
        return false;
    if (ctx->func->vregs[idx].rep == XR_REP_F64)
        return false;
    return x64_probe_phys_reg(ctx, idx) < 0;
}

static bool x64_arg_needs_scratch_fp(X64CodegenCtx *ctx, XmRef ref) {
    if (xm_ref_is_const(ref))
        return true;
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= ctx->func->nvreg)
        return false;
    if (ctx->func->vregs[idx].rep != XR_REP_F64)
        return false;
    return x64_probe_phys_reg(ctx, idx) < 0;
}

/* dst = arg0 OP arg1 for a commutative GP binop.
 * Returns the RHS hardware register that the caller should pass to OP.
 *
 * Risks resolved here:
 *   1. Distinct args that both need the GP scratch register.
 *      A naive (rn = get_operand(arg0,SCRATCH); rm = get_operand(arg1,SCRATCH))
 *      sequence silently overwrites rn. We move arg0 out of scratch into
 *      rd first to free scratch for arg1.
 *   2. arg1 already lives in rd. We swap operand order (commutative is
 *      OK to reorder) and use arg1 in place, only moving arg0 over rd
 *      when it would have been clobbered. */
static X64Reg x64_prepare_commutative_gp(X64CodegenCtx *ctx, X64Reg rd, XmRef arg0, XmRef arg1,
                                         X64Reg scratch) {
    X64Reg rn = x64_get_operand(ctx, arg0, scratch);

    if (rn == scratch && arg0 != arg1 && x64_arg_needs_scratch_gp(ctx, arg1)) {
        CODEGEN_CHECK(ctx, rd != scratch,
                      "commutative gp binop: dst aliases scratch with both args needing scratch");
        x64_mov_rr(&ctx->buf, rd, scratch);
        X64Reg rm = x64_get_operand(ctx, arg1, scratch);
        return rm;
    }

    X64Reg rm = x64_get_operand(ctx, arg1, scratch);
    if (rm == rd && rn != rd)
        return rn;
    if (rn != rd)
        x64_mov_rr(&ctx->buf, rd, rn);
    return rm;
}

/* Same as x64_prepare_commutative_gp but for SSE2 scalar-double binops. */
static X64Xmm x64_prepare_commutative_fp(X64CodegenCtx *ctx, X64Xmm fd, XmRef arg0, XmRef arg1,
                                         X64Xmm scratch) {
    X64Xmm fn = x64_get_fp_operand(ctx, arg0, scratch);

    if (fn == scratch && arg0 != arg1 && x64_arg_needs_scratch_fp(ctx, arg1)) {
        CODEGEN_CHECK(ctx, fd != scratch,
                      "commutative fp binop: dst aliases scratch with both args needing scratch");
        x64_movsd_rr(&ctx->buf, fd, scratch);
        X64Xmm fm = x64_get_fp_operand(ctx, arg1, scratch);
        return fm;
    }

    X64Xmm fm = x64_get_fp_operand(ctx, arg1, scratch);
    if (fm == fd && fn != fd)
        return fn;
    if (fn != fd)
        x64_movsd_rr(&ctx->buf, fd, fn);
    return fm;
}

/* dst = arg0 OP arg1 for a non-commutative GP binop (SUB-style).
 *
 * Two contract risks left over from the simple
 * (mov rd, rn; OP rd, rm) pattern:
 *   1. Distinct args that both need the GP scratch register would
 *      silently clobber each other on the second x64_get_operand call.
 *      Stash arg0 onto the stack via PUSH/POP so it ends up in rd while
 *      arg1 lives in scratch.
 *   2. arg1 aliases dst: the implicit `mov rd, rn` would destroy arg1
 *      before OP. Route the computation through scratch:
 *        mov scratch, rn ; (skipped if rn == scratch)
 *        OP  scratch, rm
 *        mov rd, scratch
 *      so arg1 in rd survives until consumed by OP. */
typedef void (*X64BinopGpEmit)(X64Buf *buf, X64Reg dst, X64Reg src);
typedef void (*X64BinopFpEmit)(X64Buf *buf, X64Xmm dst, X64Xmm src);
typedef void (*X64StoreGpEmit)(X64Buf *buf, X64Reg base, int32_t offset, X64Reg src);

static X64Reg x64_saved_temp_reg(X64Reg avoid) {
    return avoid == X64_RAX ? X64_RCX : X64_RAX;
}

static void x64_emit_noncommutative_gp(X64CodegenCtx *ctx, X64Reg rd, XmRef arg0, XmRef arg1,
                                       X64BinopGpEmit emit_op) {
    X64Reg rn = x64_get_operand(ctx, arg0, X64_SCRATCH_REG);

    if (rn == X64_SCRATCH_REG && arg0 != arg1 && x64_arg_needs_scratch_gp(ctx, arg1)) {
        if (rd != X64_SCRATCH_REG) {
            /* dst is a real register: push arg0, load arg1→scratch, pop arg0→dst, OP dst,scratch */
            x64_push_r(&ctx->buf, X64_SCRATCH_REG);
            X64Reg rm = x64_get_operand(ctx, arg1, X64_SCRATCH_REG);
            (void) rm;
            x64_pop_r(&ctx->buf, rd);
            emit_op(&ctx->buf, rd, X64_SCRATCH_REG);
        } else {
            /* dst is ALSO scratch (spilled vreg). Borrow RAX via push/pop as temp. */
            x64_push_r(&ctx->buf, X64_RAX);
            x64_mov_rr(&ctx->buf, X64_RAX, X64_SCRATCH_REG);
            x64_get_operand(ctx, arg1, X64_SCRATCH_REG);
            emit_op(&ctx->buf, X64_RAX, X64_SCRATCH_REG);
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_RAX);
            x64_pop_r(&ctx->buf, X64_RAX);
        }
        return;
    }

    X64Reg rm = x64_get_operand(ctx, arg1, X64_SCRATCH_REG);
    if (rm == rd && rn != rd) {
        if (rn != X64_SCRATCH_REG)
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, rn);
        emit_op(&ctx->buf, X64_SCRATCH_REG, rm);
        x64_mov_rr(&ctx->buf, rd, X64_SCRATCH_REG);
        return;
    }
    if (rn != rd)
        x64_mov_rr(&ctx->buf, rd, rn);
    emit_op(&ctx->buf, rd, rm);
}

static void x64_emit_noncommutative_fp(X64CodegenCtx *ctx, X64Xmm fd, XmRef arg0, XmRef arg1,
                                       X64BinopFpEmit emit_op) {
    X64Xmm fn = x64_get_fp_operand(ctx, arg0, X64_SCRATCH_XMM);

    if (fn == X64_SCRATCH_XMM && arg0 != arg1 && x64_arg_needs_scratch_fp(ctx, arg1)) {
        CODEGEN_CHECK(ctx, fd != X64_SCRATCH_XMM,
                      "noncomm fp binop: dst aliases scratch with both args needing scratch");
        /* x86-64 has no FP push/pop. Use a 16-byte stack stash; the pair
         * keeps RSP 16-aligned for any subsequent CALL boundary. */
        x64_sub_ri(&ctx->buf, X64_RSP, 16);
        x64_movsd_mr(&ctx->buf, X64_RSP, 0, X64_SCRATCH_XMM);
        X64Xmm fm = x64_get_fp_operand(ctx, arg1, X64_SCRATCH_XMM);
        CODEGEN_CHECK(ctx, fm == X64_SCRATCH_XMM,
                      "noncomm fp binop: expected arg1 to land in scratch");
        x64_movsd_rm(&ctx->buf, fd, X64_RSP, 0);
        x64_add_ri(&ctx->buf, X64_RSP, 16);
        emit_op(&ctx->buf, fd, fm);
        return;
    }

    X64Xmm fm = x64_get_fp_operand(ctx, arg1, X64_SCRATCH_XMM);
    if (fm == fd && fn != fd) {
        if (fn != X64_SCRATCH_XMM)
            x64_movsd_rr(&ctx->buf, X64_SCRATCH_XMM, fn);
        emit_op(&ctx->buf, X64_SCRATCH_XMM, fm);
        x64_movsd_rr(&ctx->buf, fd, X64_SCRATCH_XMM);
        return;
    }
    if (fn != fd)
        x64_movsd_rr(&ctx->buf, fd, fn);
    emit_op(&ctx->buf, fd, fm);
}

/* ========== Handler type ========== */
typedef void (*X64InsHandler)(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd);

static void x64_emit_generated_sub_rr(X64Buf *buf, X64Reg dst, X64Reg src) {
    XR_CHECK(xm_dispatch_emit_x64_gp_rr(XM_SUB, buf, dst, src),
             "x64 generated gp_rr dispatch rejected SUB");
}

/* ========== Constants ========== */

static void x64_h_const(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    if (xm_ref_is_const(ins->args[0])) {
        uint32_t ci = XM_REF_INDEX(ins->args[0]);
        CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "const index OOB");
        x64_load_imm64(&ctx->buf, rd, ctx->func->consts[ci].val.raw);
    }
}

static void x64_h_const_f64(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
    if (xm_ref_is_const(ins->args[0])) {
        uint32_t ci = XM_REF_INDEX(ins->args[0]);
        CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "CONST_F64: const OOB");
        uint64_t raw = ctx->func->consts[ci].val.raw;
        if (raw == 0) {
            x64_xorpd(&ctx->buf, fd, fd);
        } else {
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, raw);
            x64_movq_xmm_gp(&ctx->buf, fd, X64_SCRATCH_REG);
        }
    }
}

/* ========== Integer Arithmetic ========== */

static void x64_h_add(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rm = x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_gp_rr_comm(ins->op, &ctx->buf, rd, rm),
                  "x64 generated gp_rr_comm dispatch rejected op");
}

static void x64_h_sub(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    x64_emit_noncommutative_gp(ctx, rd, ins->args[0], ins->args[1], x64_emit_generated_sub_rr);
}

static void x64_h_mul(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rm = x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_gp_rr_comm(ins->op, &ctx->buf, rd, rm),
                  "x64 generated gp_rr_comm dispatch rejected op");
}

static void x64_h_div_mod(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    /* x64 IDIV raises #DE on two inputs that ARM64 SDIV handles silently:
     *   - divisor == 0
     *   - INT64_MIN / -1 (quotient overflows int64)
     * Both must be detected up front. Zero divisor deopts to the VM, which
     * raises DivisionByZero. INT64_MIN/-1 is handled inline with wrap
     * semantics matching xi_opt fold and ARM64 SDIV:
     *   INT64_MIN / -1 = INT64_MIN (via NEG, which wraps the same way)
     *   INT64_MIN % -1 = 0 */
    const bool is_div = (ins->op == XM_DIV);
    {
        X64Reg rm_chk = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        x64_test_rr(&ctx->buf, rm_chk, rm_chk);
        /* MOV-only writes inside emit_deopt_id preserve EFLAGS, so ZF
         * from the test above carries through to the Jcc below. */
        x64_emit_deopt_id(ctx, ins);
        x64_emit_deopt_jcc(ctx, X64_CC_E);
    }

    X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
    if (rn != X64_RAX)
        x64_mov_rr(&ctx->buf, X64_RAX, rn);
    if (rm == X64_RAX || rm == X64_RDX) {
        x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, rm);
        rm = X64_SCRATCH_REG;
    }

    /* Compare divisor against -1 and skip the IDIV when it matches.
     * The wrap path produces the same result IDIV would if x64 did not
     * trap: -rn for DIV (NEG wraps INT64_MIN to itself), 0 for MOD. */
    x64_cmp_ri(&ctx->buf, rm, -1);
    x64_jcc_rel32(&ctx->buf, X64_CC_NE, 0);
    uint32_t jne_normal_d = ctx->buf.pos - 4;

    /* Wrap path: rm == -1 */
    if (is_div) {
        x64_neg_r(&ctx->buf, X64_RAX);
        if (rd != X64_RAX)
            x64_mov_rr(&ctx->buf, rd, X64_RAX);
    } else {
        x64_xor_rr(&ctx->buf, rd, rd);
    }
    x64_jmp_rel32(&ctx->buf, 0);
    uint32_t jmp_done_d = ctx->buf.pos - 4;

    /* .normal: standard IDIV. */
    x64_patch_rel32(&ctx->buf, jne_normal_d, ctx->buf.pos);
    x64_cqo(&ctx->buf);
    x64_idiv_r(&ctx->buf, rm);
    X64Reg result_reg = is_div ? X64_RAX : X64_RDX;
    if (rd != result_reg)
        x64_mov_rr(&ctx->buf, rd, result_reg);

    /* .done */
    x64_patch_rel32(&ctx->buf, jmp_done_d, ctx->buf.pos);
}

static void x64_h_neg(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rm = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    if (rm != rd)
        x64_mov_rr(&ctx->buf, rd, rm);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_gp_r(ins->op, &ctx->buf, rd),
                  "x64 generated gp_r dispatch rejected op");
}

/* ========== Bitwise ========== */

static void x64_h_and(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rm = x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_gp_rr_comm(ins->op, &ctx->buf, rd, rm),
                  "x64 generated gp_rr_comm dispatch rejected op");
}

static void x64_h_or(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rm = x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_gp_rr_comm(ins->op, &ctx->buf, rd, rm),
                  "x64 generated gp_rr_comm dispatch rejected op");
}

static void x64_h_xor(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rm = x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_gp_rr_comm(ins->op, &ctx->buf, rd, rm),
                  "x64 generated gp_rr_comm dispatch rejected op");
}

static void x64_h_not(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rm = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    if (rm != rd)
        x64_mov_rr(&ctx->buf, rd, rm);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_gp_r(ins->op, &ctx->buf, rd),
                  "x64 generated gp_r dispatch rejected op");
}

/* ========== Shifts ========== */

static void x64_h_shl(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
    if (rm != X64_RCX)
        x64_mov_rr(&ctx->buf, X64_RCX, rm);
    if (rn != rd)
        x64_mov_rr(&ctx->buf, rd, rn);
    x64_shl_rcl(&ctx->buf, rd);
}

static void x64_h_shr(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
    if (rm != X64_RCX)
        x64_mov_rr(&ctx->buf, X64_RCX, rm);
    if (rn != rd)
        x64_mov_rr(&ctx->buf, rd, rn);
    x64_sar_rcl(&ctx->buf, rd);
}

/* ========== Integer Comparison ========== */

static void x64_h_cmp_int(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
    if (rn == rm) {
        x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, rm);
        rm = X64_SCRATCH_REG;
    }
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_cmp_rr_cc(ins->op, &ctx->buf, rd, rn, rm),
                  "x64 generated cmp_rr_cc dispatch rejected op");
}

/* ========== Float Arithmetic ========== */

static void x64_emit_generated_fsub(X64Buf *buf, X64Xmm dst, X64Xmm src) {
    XR_CHECK(xm_dispatch_emit_x64_fp_rr(XM_FSUB, buf, dst, src),
             "x64 generated fp_rr dispatch rejected FSUB");
}

static void x64_emit_generated_fdiv(X64Buf *buf, X64Xmm dst, X64Xmm src) {
    XR_CHECK(xm_dispatch_emit_x64_fp_rr(XM_FDIV, buf, dst, src),
             "x64 generated fp_rr dispatch rejected FDIV");
}

static void x64_h_fadd(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
    X64Xmm fm = x64_prepare_commutative_fp(ctx, fd, ins->args[0], ins->args[1], X64_SCRATCH_XMM);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_fp_rr_comm(ins->op, &ctx->buf, fd, fm),
                  "x64 generated fp_rr_comm dispatch rejected op");
}

static void x64_h_fsub(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
    x64_emit_noncommutative_fp(ctx, fd, ins->args[0], ins->args[1], x64_emit_generated_fsub);
}

static void x64_h_fmul(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
    X64Xmm fm = x64_prepare_commutative_fp(ctx, fd, ins->args[0], ins->args[1], X64_SCRATCH_XMM);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_fp_rr_comm(ins->op, &ctx->buf, fd, fm),
                  "x64 generated fp_rr_comm dispatch rejected op");
}

static void x64_h_fdiv(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
    x64_emit_noncommutative_fp(ctx, fd, ins->args[0], ins->args[1], x64_emit_generated_fdiv);
}

static void x64_h_fneg(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
    X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
    if (fn != fd)
        x64_movsd_rr(&ctx->buf, fd, fn);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, 0x8000000000000000ULL);
    x64_movq_xmm_gp(&ctx->buf, X64_SCRATCH_XMM, X64_SCRATCH_REG);
    x64_xorpd(&ctx->buf, fd, X64_SCRATCH_XMM);
}

/* ========== Float Conversion ========== */

static void x64_h_i2f(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
    X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_conv_i2f(ins->op, &ctx->buf, fd, rn),
                  "x64 generated conv_i2f dispatch rejected op");
}

static void x64_h_f2i(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
    CODEGEN_CHECK(ctx, xm_dispatch_emit_x64_conv_f2i(ins->op, &ctx->buf, rd, fn),
                  "x64 generated conv_f2i dispatch rejected op");
}

/* ========== Float Comparison ========== */

static void x64_emit_generated_fcmp(X64CodegenCtx *ctx, XmOp op, X64Reg rd, X64Xmm fn, X64Xmm fm) {
    bool ok;
    if (rd == X64_SCRATCH_REG) {
        x64_push_r(&ctx->buf, X64_RAX);
        ok = xm_dispatch_emit_x64_fcmp_rr_cc(op, &ctx->buf, rd, fn, fm, X64_RAX);
        x64_pop_r(&ctx->buf, X64_RAX);
    } else {
        ok = xm_dispatch_emit_x64_fcmp_rr_cc(op, &ctx->buf, rd, fn, fm, X64_SCRATCH_REG);
    }
    CODEGEN_CHECK(ctx, ok, "x64 generated fcmp_rr_cc dispatch rejected op");
}

static void x64_h_cmp_float(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
    if (fn == X64_SCRATCH_XMM && ins->args[0] != ins->args[1] &&
        x64_arg_needs_scratch_fp(ctx, ins->args[1])) {
        x64_sub_ri(&ctx->buf, X64_RSP, 16);
        x64_movsd_mr(&ctx->buf, X64_RSP, 0, X64_XMM14);
        x64_movsd_rr(&ctx->buf, X64_XMM14, X64_SCRATCH_XMM);
        X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
        CODEGEN_CHECK(ctx, fm == X64_SCRATCH_XMM,
                      "x64_h_cmp_float: expected arg1 to land in scratch");
        x64_emit_generated_fcmp(ctx, ins->op, rd, X64_XMM14, fm);
        x64_movsd_rm(&ctx->buf, X64_XMM14, X64_RSP, 0);
        x64_add_ri(&ctx->buf, X64_RSP, 16);
        return;
    }
    X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
    x64_emit_generated_fcmp(ctx, ins->op, rd, fn, fm);
}

/* ========== Move / Redefine ========== */

static void x64_h_mov(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    if (x64_is_fp_vreg(ctx, ins->dst)) {
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
        if (fn != fd)
            x64_movsd_rr(&ctx->buf, fd, fn);
    } else {
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        if (rn != rd)
            x64_mov_rr(&ctx->buf, rd, rn);
    }
}

/* ========== Select ========== */

static void x64_h_select_cond(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    x64_test_rr(&ctx->buf, rn, rn);
}

static void x64_h_select(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg true_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    if (true_reg == X64_SCRATCH_REG && ins->args[0] != ins->args[1] &&
        x64_arg_needs_scratch_gp(ctx, ins->args[1])) {
        X64Reg temp = x64_saved_temp_reg(rd);
        x64_push_r(&ctx->buf, temp);
        x64_mov_rr(&ctx->buf, temp, X64_SCRATCH_REG);
        X64Reg false_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        if (false_reg != rd)
            x64_mov_rr(&ctx->buf, rd, false_reg);
        x64_cmov_rr(&ctx->buf, X64_CC_NE, rd, temp);
        x64_pop_r(&ctx->buf, temp);
        return;
    }
    X64Reg false_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
    if (false_reg != rd)
        x64_mov_rr(&ctx->buf, rd, false_reg);
    x64_cmov_rr(&ctx->buf, X64_CC_NE, rd, true_reg);
}

/* ========== Load / Store ========== */

static void x64_emit_gp_store(X64CodegenCtx *ctx, XmRef addr_ref, XmRef value_ref,
                              X64StoreGpEmit emit_store) {
    X64Reg addr = x64_get_operand(ctx, addr_ref, X64_SCRATCH_REG);
    if (addr == X64_SCRATCH_REG && addr_ref != value_ref &&
        x64_arg_needs_scratch_gp(ctx, value_ref)) {
        X64Reg temp = X64_RAX;
        x64_push_r(&ctx->buf, temp);
        x64_mov_rr(&ctx->buf, temp, X64_SCRATCH_REG);
        X64Reg val = x64_get_operand(ctx, value_ref, X64_SCRATCH_REG);
        emit_store(&ctx->buf, temp, 0, val);
        x64_pop_r(&ctx->buf, temp);
        return;
    }
    X64Reg val = x64_get_operand(ctx, value_ref, X64_SCRATCH_REG);
    if (val == addr) {
        x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, val);
        val = X64_SCRATCH_REG;
    }
    emit_store(&ctx->buf, addr, 0, val);
}

static void x64_h_load(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    if (ins->rep == XR_REP_F64) {
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        x64_movsd_rm(&ctx->buf, fd, rn, 0);
    } else {
        x64_mov_rm(&ctx->buf, rd, rn, 0);
    }
}

static void x64_h_store(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    bool store_fp = false;
    if (xm_ref_is_vreg(ins->args[1])) {
        uint32_t vi = XM_REF_INDEX(ins->args[1]);
        if (vi < ctx->func->nvreg)
            store_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
    }
    if (store_fp) {
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
        x64_movsd_mr(&ctx->buf, rn, 0, fm);
    } else {
        x64_emit_gp_store(ctx, ins->args[0], ins->args[1], x64_mov_mr);
    }
}

/* ========== Sub-word Load/Store ========== */

static void x64_h_subword(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    switch (ins->op) {
        case XM_LOAD8Z: {
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_movzx_rm8(&ctx->buf, rd, addr, 0);
            break;
        }
        case XM_LOAD8S: {
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_movsx_rm8(&ctx->buf, rd, addr, 0);
            break;
        }
        case XM_STORE8: {
            x64_emit_gp_store(ctx, ins->args[0], ins->args[1], x64_mov_mr8);
            break;
        }
        case XM_LOAD16Z: {
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_movzx_rm16(&ctx->buf, rd, addr, 0);
            break;
        }
        case XM_LOAD16S: {
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_movsx_rm16(&ctx->buf, rd, addr, 0);
            break;
        }
        case XM_STORE16: {
            x64_emit_gp_store(ctx, ins->args[0], ins->args[1], x64_mov_mr16);
            break;
        }
        case XM_LOAD32Z: {
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_mov_rm32(&ctx->buf, rd, addr, 0);
            break;
        }
        case XM_LOAD32S: {
            X64Reg base = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            int32_t offset = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            x64_movsxd_rm(&ctx->buf, rd, base, offset);
            break;
        }
        case XM_STORE32: {
            x64_emit_gp_store(ctx, ins->args[0], ins->args[1], x64_mov_mr32);
            break;
        }
        default:
            CODEGEN_CHECK(ctx, false, "x64_h_subword: unreachable opcode");
            break;
    }
}

/* ========== Float sub-word memory ========== */

static void x64_h_load_f32(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
    X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    x64_movss_rm(&ctx->buf, X64_SCRATCH_XMM, addr, 0);
    x64_cvtss2sd(&ctx->buf, fd, X64_SCRATCH_XMM);
}

static void x64_h_store_f32(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
    x64_cvtsd2ss(&ctx->buf, X64_SCRATCH_XMM, fm);
    x64_movss_mr(&ctx->buf, addr, 0, X64_SCRATCH_XMM);
}

/* ========== Coro/context access ========== */

static void x64_h_coro(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    switch (ins->op) {
        case XM_LOAD_CORO: {
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->args[0]) && xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            x64_mov_rm(&ctx->buf, rd, X64_JIT_CTX_REG, offset);
            break;
        }
        case XM_LOAD_CORO_BYTE: {
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->args[0]) && xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            x64_movzx_rm8(&ctx->buf, rd, X64_JIT_CTX_REG, offset);
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
                X64Xmm fm = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
                x64_movsd_mr(&ctx->buf, X64_JIT_CTX_REG, offset, fm);
            } else {
                X64Reg val = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, offset, val);
            }
            break;
        }
        case XM_STORE_CORO_BYTE: {
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                uint32_t ci = XM_REF_INDEX(ins->dst);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            X64Reg val = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, offset, val);
            break;
        }
        default:
            CODEGEN_CHECK(ctx, false, "x64_h_coro: unreachable opcode");
            break;
    }
}

/* ========== Object field load ========== */

static void x64_h_field_load(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    int32_t offset = 0;
    if (xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        offset = (int32_t) ctx->func->consts[ci].val.i64;
    }
    /* Save runtime tag to jit_ctx scratch if type is unknown */
    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t vi = XM_REF_INDEX(ins->dst);
        if (vi < ctx->func->nvreg && ins->ctype.kind == XM_TK_UNKNOWN && ins->rep == XR_REP_I64) {
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, obj, offset + XM_XRVALUE_TAG_OFFSET);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_TAG_SCRATCH_OFFSET,
                       X64_SCRATCH_REG);
        }
    }
    if (ins->rep == XR_REP_F64) {
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        x64_movsd_rm(&ctx->buf, fd, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET);
    } else {
        x64_mov_rm(&ctx->buf, rd, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET);
    }
}

/* ========== Object field store ========== */

static void x64_h_field_store(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    int32_t offset = 0;
    if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
        uint32_t ci = XM_REF_INDEX(ins->dst);
        offset = (int32_t) ctx->func->consts[ci].val.i64;
    }

    bool is_fp = false;
    if (xm_ref_is_vreg(ins->args[1])) {
        uint32_t vi = XM_REF_INDEX(ins->args[1]);
        if (vi < ctx->func->nvreg)
            is_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
    }

    /* Store payload at XrValue byte 8 */
    if (is_fp) {
        X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
        x64_movsd_mr(&ctx->buf, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET, fm);
    } else {
        X64Reg val = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        if (val == obj) {
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, val);
            val = X64_SCRATCH_REG;
        }
        x64_mov_mr(&ctx->buf, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET, val);
    }

    /* Store tag (descriptor: tag + flags + heap_type) as 32-bit */
    uint8_t xr_tag = ins->rep;
    bool is_ptr_val = false;
    uint32_t tag_val = 0;

    if (xr_tag == XM_SF_TAG_RUNTIME) {
        tag_val = XR_TAG_PTR;
        if (xm_ref_is_vreg(ins->args[1])) {
            XmType vct = xm_ref_ctype(ctx->func, ins->args[1]);
            uint8_t vk = type_kind_to_vtag(vct.kind);
            if (vtag_is_concrete(vk)) {
                tag_val = vtag_to_value_tag(vk);
                is_ptr_val = xm_type_is_ptr(vct.kind);
            } else {
                uint32_t vi = XM_REF_INDEX(ins->args[1]);
                uint8_t vt = (vi < ctx->func->nvreg) ? ctx->func->vregs[vi].rep : XR_REP_TAGGED;
                if (vt == XR_REP_F64)
                    tag_val = XR_TAG_F64;
                else if (vt == XR_REP_I64)
                    tag_val = XR_TAG_I64;
                is_ptr_val = (vt == XR_REP_PTR || vt == XR_REP_TAGGED);
            }
        }
    } else {
        tag_val = xr_tag;
        is_ptr_val = (xr_tag == XR_TAG_PTR);
    }

    if (is_ptr_val) {
        X64Reg val = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        x64_movzx_rm16(&ctx->buf, X64_SCRATCH_REG, val, (int32_t) XM_GC_TYPE_OFFSET);
        x64_shl_ri(&ctx->buf, X64_SCRATCH_REG, 16);
        x64_or_ri(&ctx->buf, X64_SCRATCH_REG, (int32_t) tag_val);
        x64_mov_mr32(&ctx->buf, obj, offset + XM_XRVALUE_TAG_OFFSET, X64_SCRATCH_REG);
    } else {
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, tag_val);
        x64_mov_mr32(&ctx->buf, obj, offset + XM_XRVALUE_TAG_OFFSET, X64_SCRATCH_REG);
    }
}

/* ========== Tag load ========== */

static void x64_h_tag_load(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    int32_t offset = 0;
    if (!xm_ref_is_none(ins->args[1]) && xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        offset = (int32_t) ctx->func->consts[ci].val.i64;
    }
    x64_movzx_rm8(&ctx->buf, rd, ptr, offset);
}

/* ========== Box / Unbox ========== */

static void x64_h_box(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    /* BOX is a no-op inside JIT (values are always raw/untagged) */
    X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    if (rd != rn)
        x64_mov_rr(&ctx->buf, rd, rn);
}

static void x64_h_unbox_i64(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    uint8_t src_type = XR_REP_I64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t vi = XM_REF_INDEX(ins->args[0]);
        if (vi < ctx->func->nvreg)
            src_type = ctx->func->vregs[vi].rep;
    }
    if (src_type == XR_REP_PTR) {
        X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_mov_rm(&ctx->buf, rd, ptr, XM_XRVALUE_PAYLOAD_OFFSET);
    } else {
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        if (rd != rn)
            x64_mov_rr(&ctx->buf, rd, rn);
    }
}

static void x64_h_unbox_f64(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    uint8_t src_type = XR_REP_F64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t vi = XM_REF_INDEX(ins->args[0]);
        if (vi < ctx->func->nvreg)
            src_type = ctx->func->vregs[vi].rep;
    }
    if (src_type == XR_REP_PTR) {
        X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        x64_movsd_rm(&ctx->buf, fd, ptr, XM_XRVALUE_PAYLOAD_OFFSET);
    } else {
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        if (rd != rn)
            x64_mov_rr(&ctx->buf, rd, rn);
    }
}

/* ========== Guard ops ========== */

static void x64_h_guard(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    switch (ins->op) {
        case XM_GUARD_TAG: {
            X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, val_reg, XM_XRVALUE_TAG_OFFSET);
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
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
        case XM_GUARD_BOUNDS: {
            X64Reg idx_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg len_reg = x64_get_operand(
                ctx, ins->args[1], idx_reg == X64_SCRATCH_REG ? X64_RCX : X64_SCRATCH_REG);
            x64_cmp_rr(&ctx->buf, idx_reg, len_reg);
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_AE);
            break;
        }
        case XM_GUARD_NONNULL: {
            X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_test_rr(&ctx->buf, val_reg, val_reg);
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_E);
            break;
        }
        case XM_GUARD_CLASS: {
            X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_movzx_rm16(&ctx->buf, X64_SCRATCH_REG, obj, (int32_t) XM_GC_EXTRA_OFFSET);
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
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
        case XM_GUARD_KLASS: {
            X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, obj, (int32_t) XM_INSTANCE_KLASS_OFFSET);
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                uint64_t expected = (uint64_t) ctx->func->consts[ci].val.i64;
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
        case XM_TAG_CHECK: {
            X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, val_reg, XM_XRVALUE_TAG_OFFSET);
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                int32_t expected = (int32_t) ctx->func->consts[ci].val.raw;
                x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, expected);
            }
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            break;
        }
        default:
            CODEGEN_CHECK(ctx, false, "x64_h_guard: unreachable opcode");
            break;
    }
}

/* ========== Deopt ========== */

static void x64_h_deopt(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    x64_emit_deopt_id(ctx, ins);
    CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
    X64BranchPatch *p = &ctx->patches[ctx->npatch];
    x64_jmp_rel32(&ctx->buf, 0);
    p->emit_pos = ctx->buf.pos - 4;
    p->target_blk = 0;
    p->type = X64_PATCH_DEOPT_JMP;
    p->cc = X64_CC_E;
    ctx->npatch++;
    ctx->has_deopt = true;
}

/* ========== Safepoint ========== */

static void x64_h_safepoint(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    (void) ins;
    uint32_t smap_id_sp = x64_record_safepoint(ctx);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_sp);
    x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                 X64_SCRATCH_REG);
    x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG, (int32_t) XM_JIT_SAFEPOINT_PAGE_OFFSET);
    x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG, 0);
}

/* ========== Runtime arithmetic (mixed-type) ========== */

static void x64_h_rt_arith(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
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
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        if (fd != fa)
            x64_movsd_rr(&ctx->buf, fd, fa);
        switch (ins->op) {
            case XM_RT_ADD:
                x64_addsd(&ctx->buf, fd, fb);
                break;
            case XM_RT_SUB:
                x64_subsd(&ctx->buf, fd, fb);
                break;
            case XM_RT_MUL:
                x64_mulsd(&ctx->buf, fd, fb);
                break;
            case XM_RT_DIV:
                x64_divsd(&ctx->buf, fd, fb);
                break;
            case XM_RT_MOD: {
                x64_movsd_rr(&ctx->buf, X64_XMM14, fd);
                x64_divsd(&ctx->buf, X64_XMM14, fb);
                x64_cvttsd2si(&ctx->buf, X64_SCRATCH_REG, X64_XMM14);
                x64_cvtsi2sd(&ctx->buf, X64_XMM14, X64_SCRATCH_REG);
                x64_mulsd(&ctx->buf, X64_XMM14, fb);
                x64_subsd(&ctx->buf, fd, X64_XMM14);
                break;
            }
            default:
                CODEGEN_CHECK(ctx, false, "x64_h_rt_arith: unreachable float op");
                break;
        }
    } else {
        x64_emit_deopt_id(ctx, ins);
        CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
        X64BranchPatch *p = &ctx->patches[ctx->npatch];
        x64_jmp_rel32(&ctx->buf, 0);
        p->emit_pos = ctx->buf.pos - 4;
        p->target_blk = 0;
        p->type = X64_PATCH_DEOPT_JMP;
        p->cc = X64_CC_E;
        ctx->npatch++;
        ctx->has_deopt = true;
    }
}

/* ========== Runtime unary minus ========== */

static void x64_h_rt_unm(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    uint8_t ta = XR_REP_I64;
    if (xm_ref_is_vreg(ins->args[0])) {
        uint32_t ai = XM_REF_INDEX(ins->args[0]);
        if (ai < ctx->func->nvreg)
            ta = ctx->func->vregs[ai].rep;
    }
    X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
    if (ta == XR_REP_F64) {
        X64Xmm fa = x64_get_fp_operand(ctx, ins->args[0], X64_XMM14);
        x64_xorpd(&ctx->buf, fd, fd);
        x64_subsd(&ctx->buf, fd, fa);
    } else {
        X64Reg ga = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_cvtsi2sd(&ctx->buf, fd, ga);
        x64_xorpd(&ctx->buf, X64_XMM15, X64_XMM15);
        x64_subsd(&ctx->buf, X64_XMM15, fd);
        x64_movsd_rr(&ctx->buf, fd, X64_XMM15);
    }
}

/* ========== Runtime comparison ========== */

static void x64_h_rt_cmp(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
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
        XmOp cmp_op = XM_FEQ;
        if (ins->op == XM_RT_LT)
            cmp_op = XM_FLT;
        else if (ins->op == XM_RT_LE)
            cmp_op = XM_FLE;
        bool ok;
        if (rd == X64_SCRATCH_REG) {
            x64_push_r(&ctx->buf, X64_RAX);
            ok = xm_dispatch_emit_x64_fcmp_rr_cc(cmp_op, &ctx->buf, rd, fa, fb, X64_RAX);
            x64_pop_r(&ctx->buf, X64_RAX);
        } else {
            ok = xm_dispatch_emit_x64_fcmp_rr_cc(cmp_op, &ctx->buf, rd, fa, fb, X64_SCRATCH_REG);
        }
        CODEGEN_CHECK(ctx, ok, "x64 generated runtime fcmp_rr_cc dispatch rejected op");
    } else {
        x64_emit_deopt_id(ctx, ins);
        CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
        X64BranchPatch *p = &ctx->patches[ctx->npatch];
        x64_jmp_rel32(&ctx->buf, 0);
        p->emit_pos = ctx->buf.pos - 4;
        p->target_blk = 0;
        p->type = X64_PATCH_DEOPT_JMP;
        p->cc = X64_CC_E;
        ctx->npatch++;
        ctx->has_deopt = true;
    }
}

/* ========== Runtime collection new ========== */

static void x64_h_rt_collection(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    x64_emit_ptr_spill_writeback(ctx);

    uint32_t smap_id = x64_record_safepoint(ctx);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id);
    x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                 X64_SCRATCH_REG);

    if (xm_ref_is_const(ins->args[0])) {
        uint32_t ci = XM_REF_INDEX(ins->args[0]);
        uint64_t cval = (uint64_t) ctx->func->consts[ci].val.raw;
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, cval);
    } else {
        X64Reg r = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        if (r != X64_SCRATCH_REG)
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, r);
    }
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET, X64_SCRATCH_REG);

    x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
    x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET, X64_SCRATCH_REG);

    void *fn = (ins->op == XM_RT_ARRAY_NEW) ? xm_helper_func(XM_HELPER_rt_array_new)
                                            : xm_helper_func(XM_HELPER_rt_map_new);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) fn);

    CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
    X64BranchPatch *p_an = &ctx->patches[ctx->npatch];
    x64_call_rel32(&ctx->buf, 0);
    p_an->emit_pos = ctx->buf.pos - 4;
    p_an->target_blk = 0;
    p_an->type = X64_PATCH_CALL_C;
    p_an->cc = X64_CC_E;
    ctx->npatch++;
    ctx->has_call_c = true;

    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t dvi = XM_REF_INDEX(ins->dst);
        if (rd != X64_RAX)
            x64_mov_rr(&ctx->buf, rd, X64_RAX);
        if (dvi < ctx->func->nvreg && dvi < XR_JIT_MAX_VREG_TAGS) {
            int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) dvi;
            x64_mov_ri32(&ctx->buf, X64_SCRATCH_REG, XR_TAG_PTR);
            x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, tag_off, X64_SCRATCH_REG);
        }
    }
}

/* ========== Runtime array push ========== */

static void x64_h_rt_array_push(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    x64_emit_ptr_spill_writeback(ctx);

    uint32_t smap_id_ap = x64_record_safepoint(ctx);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_ap);
    x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                 X64_SCRATCH_REG);

    int32_t ca0 = (int32_t) XM_JIT_CALL_ARGS_OFFSET;
    X64Reg arr_r = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, ca0, arr_r);

    int32_t ca1 = (int32_t) (XM_JIT_CALL_ARGS_OFFSET + 8);
    if (xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        uint64_t cval = (uint64_t) ctx->func->consts[ci].val.raw;
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, cval);
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, ca1, X64_SCRATCH_REG);
    } else {
        X64Reg vr = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, ca1, vr);
    }

    uint8_t t0 = 4; /* XR_TAG_PTR */
    uint8_t t1 = XR_RTAG_UNKNOWN;
    if (xm_ref_is_const(ins->args[1])) {
        uint32_t ci = XM_REF_INDEX(ins->args[1]);
        t1 = const_rep_to_value_tag(ctx->func->consts[ci].rep);
    } else {
        XmType ct = xm_ref_ctype(ctx->func, ins->args[1]);
        uint8_t vk = type_kind_to_vtag(ct.kind);
        if (vtag_is_concrete(vk))
            t1 = vtag_to_value_tag(vk);
    }
    uint64_t tpk = (uint64_t) t0 | ((uint64_t) t1 << 8);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, tpk);
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET, X64_SCRATCH_REG);

    if (t1 == XR_RTAG_UNKNOWN && xm_ref_is_vreg(ins->args[1])) {
        uint32_t ai = XM_REF_INDEX(ins->args[1]);
        if (ai < ctx->func->nvreg && ai < XR_JIT_MAX_VREG_TAGS) {
            int32_t soff = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) ai;
            int32_t doff = (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET + 1;
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG, soff);
            x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, doff, X64_SCRATCH_REG);
        }
    }

    x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET, X64_SCRATCH_REG);
    x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET, X64_SCRATCH_REG);

    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG,
                   (uint64_t) (uintptr_t) xm_helper_func(XM_HELPER_rt_array_push));

    CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
    X64BranchPatch *p_ap = &ctx->patches[ctx->npatch];
    x64_call_rel32(&ctx->buf, 0);
    p_ap->emit_pos = ctx->buf.pos - 4;
    p_ap->target_blk = 0;
    p_ap->type = X64_PATCH_CALL_C;
    p_ap->cc = X64_CC_E;
    ctx->npatch++;
    ctx->has_call_c = true;
}

/* ========== Runtime simple ops ========== */

static bool x64_isnull_uses_runtime_tag(X64CodegenCtx *ctx, XmRef ref, uint32_t *out_vi) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t vi = XM_REF_INDEX(ref);
    if (vi >= ctx->func->nvreg || vi >= XR_JIT_MAX_VREG_TAGS)
        return false;
    XmType ct = xm_ref_ctype(ctx->func, ref);
    if (ctx->func->vregs[vi].rep == XR_REP_TAGGED) {
        *out_vi = vi;
        return true;
    }
    if (ct.kind != XM_TK_TAGGED)
        return false;
    XmIns *def = ctx->func->vregs[vi].def;
    if (!def) {
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

static void x64_h_rt_simple(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    if (ins->op == XM_RT_ISNULL) {
        if (xm_ref_is_vreg(ins->args[0])) {
            XmType ct = xm_ref_ctype(ctx->func, ins->args[0]);
            if (ct.kind == XM_TK_NULL) {
                x64_mov_ri32(&ctx->buf, rd, 1);
                return;
            }
            if (ct.kind == XM_TK_INT || ct.kind == XM_TK_FLOAT || ct.kind == XM_TK_BOOL) {
                x64_mov_ri32(&ctx->buf, rd, 0);
                return;
            }
            uint32_t vi = 0;
            if (x64_isnull_uses_runtime_tag(ctx, ins->args[0], &vi)) {
                int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
                x64_movzx_rm8(&ctx->buf, rd, X64_JIT_CTX_REG, tag_off);
                x64_cmp_ri(&ctx->buf, rd, XR_TAG_NULL);
                x64_mov_ri32(&ctx->buf, rd, 0);
                x64_setcc(&ctx->buf, X64_CC_E, rd);
                return;
            }
        }
        X64Reg val = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_xor_rr(&ctx->buf, rd, rd);
        x64_test_rr(&ctx->buf, val, val);
        x64_setcc(&ctx->buf, X64_CC_E, rd);
    } else {
        /* RT_PRINT, RT_ARRAY_LEN, RT_INDEX_GET, RT_INDEX_SET — not yet
         * lowered. These ops are side-effect-only (dst is unused), so
         * emitting a NOP is semantically safe. The NOP itself is
         * mandatory: the arm64 backend does the same in xm_codegen_mem.c,
         * and the surrounding pass machinery assumes every XmIns
         * contributes at least one machine instruction. Skipping the
         * emit corrupts subsequent branch/jump offsets and surfaces as
         * STATUS_HEAP_CORRUPTION (0xC0000374) at runtime on Win64. */
        xr_log_debug("x64-cg", "RT opcode %d fell through to NOP (expected CALL_C)", ins->op);
        x64_nop(&ctx->buf);
    }
}

/* ========== No-op handlers ========== */

static void x64_h_nop(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) ctx;
    (void) ins;
    (void) rd;
}

/* ========== Suspend (coroutine await/channel) ========== */

static void x64_h_suspend(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
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

    uint32_t smap_id = x64_record_safepoint(ctx);

    /* Load suspend_state pointer: R11 = jit_state->suspend */
    x64_emit_load_jit_state(&ctx->buf, X64_SCRATCH_REG);
    x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
               (int32_t) XM_JIT_STATE_SUSPEND_PTR_OFFSET);
    CODEGEN_CHECK(ctx, suspend_id < XM_MAX_SUSPEND_ENTRIES, "suspend_id out of range");

    /* Save caller-saved GP regs */
    for (int i = 0; i < X64_NGPR_CALLER_SAVE && i < X64_MAX_PHYS_REGS; i++)
        x64_mov_mr(&ctx->buf, X64_SCRATCH_REG, (int32_t) XM_SUSPEND_CALLER_SAVED_OFF + i * 8,
                   x64_alloc_regs[i]);

    /* Save callee-saved GP regs */
    for (int i = 0; i < X64_NGPR_CALLEE_SAVE_ALLOC; i++)
        x64_mov_mr(&ctx->buf, X64_SCRATCH_REG, (int32_t) XM_SUSPEND_CALLEE_SAVED_OFF + i * 8,
                   x64_alloc_regs[X64_NGPR_CALLER_SAVE + i]);

    /* Save spill slots */
    {
        uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
        if (ns > XM_SUSPEND_SPILL_MAX)
            ns = XM_SUSPEND_SPILL_MAX;
        for (uint32_t s = 0; s < ns; s++) {
            int32_t frame_off = -(int32_t) (X64_SPILL_BASE + s * 8);
            int32_t regs_off = (int32_t) (XM_SUSPEND_SPILL_OFF + s * 8);
            x64_mov_rm(&ctx->buf, X64_RCX, X64_RBP, frame_off);
            x64_mov_mr(&ctx->buf, X64_SCRATCH_REG, regs_off, X64_RCX);
        }
    }

    /* Store suspend_id and smap_id */
    x64_emit_load_jit_state(&ctx->buf, X64_RCX);
    x64_load_imm64(&ctx->buf, X64_RAX, (uint64_t) suspend_id);
    x64_mov_mr32(&ctx->buf, X64_RCX, (int32_t) XM_JIT_STATE_SUSPEND_ID_OFFSET, X64_RAX);
    x64_load_imm64(&ctx->buf, X64_RAX, (uint64_t) smap_id);
    x64_mov_mr32(&ctx->buf, X64_RCX, (int32_t) XM_JIT_STATE_SUSPEND_SMAP_OFFSET, X64_RAX);
    x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET, X64_RAX);

    /* Pre-store resume info (gopark pattern) */
    x64_mov_rm(&ctx->buf, X64_RAX, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_PROTO_OFFSET);
    x64_mov_mr(&ctx->buf, X64_RCX, (int32_t) XM_JIT_STATE_RESUME_PROTO_OFFSET, X64_RAX);
    x64_mov_rm(&ctx->buf, X64_RAX, X64_RAX, (int32_t) XM_PROTO_JIT_RESUME_ENTRY_OFFSET);
    x64_mov_mr(&ctx->buf, X64_RCX, (int32_t) XM_JIT_STATE_RESUME_ENTRY_OFFSET, X64_RAX);

    /* Call block_helper(coro, extra_arg) */
    void *block_helper = ctx->func->suspend_block_helpers[suspend_id];
    int64_t helper_extra_arg = 0;
    if (!block_helper) {
        block_helper = xm_helper_func(XM_HELPER_await_block);
        helper_extra_arg = discard_result;
    }
#ifdef _WIN32
    x64_sub_ri(&ctx->buf, X64_RSP, X64_ABI_SHADOW_BYTES);
    x64_mov_rr(&ctx->buf, X64_RCX, X64_CORO_REG);
    x64_load_imm64(&ctx->buf, X64_RDX, (uint64_t) helper_extra_arg);
#else
    x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
    x64_load_imm64(&ctx->buf, X64_RSI, (uint64_t) helper_extra_arg);
#endif
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) block_helper);
    x64_call_r(&ctx->buf, X64_SCRATCH_REG);
#ifdef _WIN32
    x64_add_ri(&ctx->buf, X64_RSP, X64_ABI_SHADOW_BYTES);
#endif

    /* Check result: RAX == 0 -> blocked, RAX != 0 -> inline resume */
    x64_test_rr(&ctx->buf, X64_RAX, X64_RAX);
    x64_jcc_rel32(&ctx->buf, X64_CC_NE, 0);
    uint32_t jne_not_blocked = ctx->buf.pos - 4;

    /* Blocked path: return SUSPEND_MARKER */
    x64_load_imm64(&ctx->buf, X64_RAX, (uint64_t) XM_SUSPEND_MARKER);
    x64_emit_epilogue(ctx);

    /* Not-blocked path: inline resume */
    x64_patch_rel32(&ctx->buf, jne_not_blocked, ctx->buf.pos);

    /* Reload suspend pointer (R11 clobbered by CALL) */
    x64_emit_load_jit_state(&ctx->buf, X64_SCRATCH_REG);
    x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
               (int32_t) XM_JIT_STATE_SUSPEND_PTR_OFFSET);

    /* Reload caller-saved GP regs */
    for (int i = 0; i < X64_NGPR_CALLER_SAVE && i < X64_MAX_PHYS_REGS; i++)
        x64_mov_rm(&ctx->buf, x64_alloc_regs[i], X64_SCRATCH_REG,
                   (int32_t) XM_SUSPEND_CALLER_SAVED_OFF + i * 8);

    /* Load await/channel result into dst */
    if (rd != X64_SCRATCH_REG) {
        x64_mov_rm(&ctx->buf, rd, X64_SCRATCH_REG, (int32_t) XM_SUSPEND_RESULT_OFF);
    }

    /* Load result_tag -> runtime_tags[bc_slot] */
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
            x64_movzx_rm8(&ctx->buf, X64_RCX, X64_SCRATCH_REG, (int32_t) XM_SUSPEND_RESULT_TAG_OFF);
            x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, res_vreg_off, X64_RCX);
        }
        if (suspend_id < XM_MAX_SUSPEND_ENTRIES) {
            ctx->suspend_result_bc_slots[suspend_id] = res_bc_slot;
            ctx->suspend_result_tag_offs[suspend_id] = res_vreg_off;
        }
    }

    /* Record continuation point for resume entry jump table */
    if (suspend_id < XM_MAX_SUSPEND_ENTRIES) {
        ctx->suspend_cont_offsets[suspend_id] = ctx->buf.pos;
        ctx->suspend_smap_ids[suspend_id] = smap_id;
        ctx->suspend_result_regs[suspend_id] =
            (uint8_t) (xm_ref_is_vreg(ins->dst) ? rd : X64_SCRATCH_REG);
        if (suspend_id >= ctx->nsuspend)
            ctx->nsuspend = suspend_id + 1;
    }
}

/* ========== Write barriers ========== */

static void x64_h_barrier_fwd(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Reg parent_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    X64Reg child_reg = x64_get_operand(ctx, ins->args[1], X64_RCX);
    if (parent_reg != X64_SCRATCH_REG)
        x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, parent_reg);
    if (child_reg != X64_RCX)
        x64_mov_rr(&ctx->buf, X64_RCX, child_reg);
    CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
    X64BranchPatch *bp = &ctx->patches[ctx->npatch];
    x64_call_rel32(&ctx->buf, 0);
    bp->emit_pos = ctx->buf.pos - 4;
    bp->target_blk = 0;
    bp->type = X64_PATCH_BARRIER_FWD;
    bp->cc = X64_CC_E;
    ctx->npatch++;
    ctx->has_barriers = true;
}

static void x64_h_barrier_back(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Reg container_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    if (container_reg != X64_SCRATCH_REG)
        x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, container_reg);
    CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
    X64BranchPatch *bp = &ctx->patches[ctx->npatch];
    x64_call_rel32(&ctx->buf, 0);
    bp->emit_pos = ctx->buf.pos - 4;
    bp->target_blk = 0;
    bp->type = X64_PATCH_BARRIER_BACK;
    bp->cc = X64_CC_E;
    ctx->npatch++;
    ctx->has_barriers = true;
}

/* ========== GC allocation ========== */

static void x64_h_alloc(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
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
    CODEGEN_CHECK(ctx, alloc_size > 0 && alloc_size < 65536, "alloc_size OOB");

    x64_emit_alloc_ins(ctx, ins, rd, gc_type, gc_extra, alloc_size);
}

/* ========== Catch ========== */

static void x64_h_catch(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    int16_t spill_slot = -1;
    x64_mov_rm(&ctx->buf, rd, X64_JIT_CTX_REG, (int32_t) XM_JIT_EXCEPTION_OFFSET);
    if (rd == X64_SCRATCH_REG && xm_ref_is_vreg(ins->dst)) {
        uint32_t dvi = XM_REF_INDEX(ins->dst);
        spill_slot = xra_vreg_spill(ctx->xra, dvi);
        if (spill_slot >= 0) {
            int32_t spill_off = -(X64_SPILL_BASE + spill_slot * 8);
            x64_mov_mr(&ctx->buf, X64_RBP, spill_off, X64_SCRATCH_REG);
        }
    }
    x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_EXCEPTION_OFFSET, X64_SCRATCH_REG);
    if (xm_ref_is_vreg(ins->dst)) {
        uint32_t dvi = XM_REF_INDEX(ins->dst);
        if (dvi < ctx->func->nvreg && dvi < XR_JIT_MAX_VREG_TAGS) {
            int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) dvi;
            x64_mov_ri32(&ctx->buf, X64_SCRATCH_REG, XR_TAG_PTR);
            x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, tag_off, X64_SCRATCH_REG);
        }
    }
    if (rd == X64_SCRATCH_REG && spill_slot >= 0) {
        int32_t spill_off = -(X64_SPILL_BASE + spill_slot * 8);
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_RBP, spill_off);
    }
}

/* ========== Call ops — delegated to xm_codegen_x64_call.c ========== */

static void x64_h_call(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    if (!x64_emit_call_ins(ctx, ins, rd))
        ctx->had_error = true;
}

/* ========== Return ========== */

static void x64_h_ret(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd) {
    (void) rd;
    X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
    if (rn != X64_RAX)
        x64_mov_rr(&ctx->buf, X64_RAX, rn);
}

/* ========== Dispatch Table ========== */

static const X64InsHandler x64_ins_handlers[XM_OP_COUNT] = {
    [XM_CONST_I64] = x64_h_const,
    [XM_CONST_PTR] = x64_h_const,
    [XM_CONST_F64] = x64_h_const_f64,
    [XM_ADD] = x64_h_add,
    [XM_SUB] = x64_h_sub,
    [XM_MUL] = x64_h_mul,
    [XM_DIV] = x64_h_div_mod,
    [XM_MOD] = x64_h_div_mod,
    [XM_NEG] = x64_h_neg,
    [XM_AND] = x64_h_and,
    [XM_OR] = x64_h_or,
    [XM_XOR] = x64_h_xor,
    [XM_NOT] = x64_h_not,
    [XM_SHL] = x64_h_shl,
    [XM_SHR] = x64_h_shr,
    [XM_EQ] = x64_h_cmp_int,
    [XM_NE] = x64_h_cmp_int,
    [XM_LT] = x64_h_cmp_int,
    [XM_LE] = x64_h_cmp_int,
    [XM_GT] = x64_h_cmp_int,
    [XM_GE] = x64_h_cmp_int,
    [XM_FADD] = x64_h_fadd,
    [XM_FSUB] = x64_h_fsub,
    [XM_FMUL] = x64_h_fmul,
    [XM_FDIV] = x64_h_fdiv,
    [XM_FNEG] = x64_h_fneg,
    [XM_I2F] = x64_h_i2f,
    [XM_F2I] = x64_h_f2i,
    [XM_FEQ] = x64_h_cmp_float,
    [XM_FNE] = x64_h_cmp_float,
    [XM_FLT] = x64_h_cmp_float,
    [XM_FLE] = x64_h_cmp_float,
    [XM_MOV] = x64_h_mov,
    [XM_REDEFINE] = x64_h_mov,
    [XM_SELECT_COND] = x64_h_select_cond,
    [XM_SELECT] = x64_h_select,
    [XM_LOAD] = x64_h_load,
    [XM_STORE] = x64_h_store,
    [XM_LOAD8Z] = x64_h_subword,
    [XM_LOAD8S] = x64_h_subword,
    [XM_STORE8] = x64_h_subword,
    [XM_LOAD16Z] = x64_h_subword,
    [XM_LOAD16S] = x64_h_subword,
    [XM_STORE16] = x64_h_subword,
    [XM_LOAD32Z] = x64_h_subword,
    [XM_LOAD32S] = x64_h_subword,
    [XM_STORE32] = x64_h_subword,
    [XM_LOAD_F32] = x64_h_load_f32,
    [XM_STORE_F32] = x64_h_store_f32,
    [XM_LOAD_CORO] = x64_h_coro,
    [XM_LOAD_CORO_BYTE] = x64_h_coro,
    [XM_STORE_CORO] = x64_h_coro,
    [XM_STORE_CORO_BYTE] = x64_h_coro,
    [XM_LOAD_FIELD] = x64_h_field_load,
    [XM_STORE_FIELD] = x64_h_field_store,
    [XM_TAG_LOAD] = x64_h_tag_load,
    [XM_BOX_I64] = x64_h_box,
    [XM_BOX_F64] = x64_h_box,
    [XM_UNBOX_I64] = x64_h_unbox_i64,
    [XM_UNBOX_F64] = x64_h_unbox_f64,
    [XM_GUARD_TAG] = x64_h_guard,
    [XM_GUARD_BOUNDS] = x64_h_guard,
    [XM_GUARD_NONNULL] = x64_h_guard,
    [XM_GUARD_CLASS] = x64_h_guard,
    [XM_GUARD_KLASS] = x64_h_guard,
    [XM_TAG_CHECK] = x64_h_guard,
    [XM_DEOPT] = x64_h_deopt,
    [XM_SAFEPOINT] = x64_h_safepoint,
    [XM_RT_ADD] = x64_h_rt_arith,
    [XM_RT_SUB] = x64_h_rt_arith,
    [XM_RT_MUL] = x64_h_rt_arith,
    [XM_RT_DIV] = x64_h_rt_arith,
    [XM_RT_MOD] = x64_h_rt_arith,
    [XM_RT_UNM] = x64_h_rt_unm,
    [XM_RT_LT] = x64_h_rt_cmp,
    [XM_RT_LE] = x64_h_rt_cmp,
    [XM_RT_EQ] = x64_h_rt_cmp,
    [XM_RT_ARRAY_NEW] = x64_h_rt_collection,
    [XM_RT_MAP_NEW] = x64_h_rt_collection,
    [XM_RT_ARRAY_PUSH] = x64_h_rt_array_push,
    [XM_RT_PRINT] = x64_h_rt_simple,
    [XM_RT_ARRAY_LEN] = x64_h_rt_simple,
    [XM_RT_INDEX_GET] = x64_h_rt_simple,
    [XM_RT_INDEX_SET] = x64_h_rt_simple,
    [XM_RT_ISNULL] = x64_h_rt_simple,
    [XM_TRY_BEGIN] = x64_h_nop,
    [XM_TRY_END] = x64_h_nop,
    [XM_THROW] = x64_h_nop,
    [XM_NOP] = x64_h_nop,
    [XM_PHI] = x64_h_nop,
    [XM_SUSPEND] = x64_h_suspend,
    [XM_BARRIER_FWD] = x64_h_barrier_fwd,
    [XM_BARRIER_BACK] = x64_h_barrier_back,
    [XM_ALLOC] = x64_h_alloc,
    [XM_CATCH] = x64_h_catch,
    [XM_CALL_C] = x64_h_call,
    [XM_CALL_C_LEAF] = x64_h_call,
    [XM_CALL_SELF_DIRECT] = x64_h_call,
    [XM_CALL_KNOWN] = x64_h_call,
    [XM_CALL_KNOWN_REG] = x64_h_call,
    [XM_CALL_DIRECT] = x64_h_call,
    [XM_CALL] = x64_h_call,
    [XM_RET] = x64_h_ret,
};

/* ========== Instruction Dispatch ========== */

/* Ops that legitimately emit 0 bytes (metadata-only or resolved elsewhere) */
static bool x64_op_allows_zero_emit(XmOp op) {
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

XR_FUNC void x64_emit_xm_ins(X64CodegenCtx *ctx, XmIns *ins) {
    CODEGEN_CHECK(ctx, ctx != NULL, "x64_emit_xm_ins: NULL ctx");
    CODEGEN_CHECK(ctx, ins != NULL, "x64_emit_xm_ins: NULL ins");
    X64Reg rd = x64_get_reg(ctx, ins->dst);

    CODEGEN_CHECK(ctx, ins->op < XM_OP_COUNT, "x64_emit_xm_ins: op out of range");
    const XmDispatchMeta *meta = xm_dispatch_meta_find((XmOp) ins->op, XM_DISPATCH_BACKEND_X64);
    if (!meta) {
        xr_log_warning("x64-cg", "missing generated dispatch metadata for %s (%u)",
                       xm_op_name(ins->op), (uint32_t) ins->op);
        CODEGEN_CHECK(ctx, false, "missing generated dispatch metadata");
    }
    X64InsHandler handler = x64_ins_handlers[ins->op];
    if (handler) {
        uint32_t pos_before = ctx->buf.pos;
        handler(ctx, ins, rd);
        /* Byte self-check: non-metadata ops must emit ≥ 1 byte */
        if (ctx->buf.pos == pos_before && !x64_op_allows_zero_emit(ins->op)) {
            xr_log_warning("x64-cg", "handler emitted 0 bytes for %s; declared mcinsns=%s",
                           xm_op_name(ins->op), meta->mcinsns);
            CODEGEN_CHECK(ctx, false, "x64 handler emitted 0 bytes for non-metadata op");
        }
    } else {
        xr_log_warning("x64-cg", "unhandled %s in generated dispatch metadata; mcinsns=%s",
                       xm_op_name(ins->op), meta->mcinsns);
        CODEGEN_CHECK(ctx, false, "unhandled Xm opcode in x64 backend");
    }
}

#endif /* __x86_64__ || _M_X64 */
