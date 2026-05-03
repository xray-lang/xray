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

XR_FUNC void x64_emit_xm_ins(X64CodegenCtx *ctx, XmIns *ins) {
    CODEGEN_CHECK(ctx, ctx != NULL, "x64_emit_xm_ins: NULL ctx");
    CODEGEN_CHECK(ctx, ins != NULL, "x64_emit_xm_ins: NULL ins");
    X64Reg rd = x64_get_reg(ctx, ins->dst);

    switch (ins->op) {
        /* ========== Constants ========== */
        case XM_CONST_I64:
        case XM_CONST_PTR: {
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "const index OOB");
                x64_load_imm64(&ctx->buf, rd, ctx->func->consts[ci].val.raw);
            }
            break;
        }

        /* ========== Integer Arithmetic ========== */
        case XM_ADD: {
            X64Reg rm =
                x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
            x64_add_rr(&ctx->buf, rd, rm);
            break;
        }
        case XM_SUB: {
            x64_emit_noncommutative_gp(ctx, rd, ins->args[0], ins->args[1], x64_sub_rr);
            break;
        }
        case XM_MUL: {
            X64Reg rm =
                x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
            x64_imul_rr(&ctx->buf, rd, rm);
            break;
        }
        case XM_DIV:
        case XM_MOD: {
            /* x86-64 IDIV: RAX = RDX:RAX / src, RDX = remainder.
             * We need: save RAX/RDX if live, move dividend to RAX, CQO, IDIV, get result. */
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);

            /* Move dividend to RAX if not already there */
            if (rn != X64_RAX)
                x64_mov_rr(&ctx->buf, X64_RAX, rn);
            /* Ensure divisor is not in RAX or RDX (would be clobbered by CQO) */
            if (rm == X64_RAX || rm == X64_RDX) {
                x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, rm);
                rm = X64_SCRATCH_REG;
            }
            /* Sign-extend RAX into RDX:RAX */
            x64_cqo(&ctx->buf);
            /* IDIV rm */
            x64_idiv_r(&ctx->buf, rm);
            /* Result: RAX=quotient, RDX=remainder */
            X64Reg result_reg = (ins->op == XM_DIV) ? X64_RAX : X64_RDX;
            if (rd != result_reg)
                x64_mov_rr(&ctx->buf, rd, result_reg);
            break;
        }
        case XM_NEG: {
            X64Reg rm = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (rm != rd)
                x64_mov_rr(&ctx->buf, rd, rm);
            x64_neg_r(&ctx->buf, rd);
            break;
        }

        /* ========== Logical ========== */
        case XM_AND: {
            X64Reg rm =
                x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
            x64_and_rr(&ctx->buf, rd, rm);
            break;
        }
        case XM_OR: {
            X64Reg rm =
                x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
            x64_or_rr(&ctx->buf, rd, rm);
            break;
        }
        case XM_XOR: {
            X64Reg rm =
                x64_prepare_commutative_gp(ctx, rd, ins->args[0], ins->args[1], X64_SCRATCH_REG);
            x64_xor_rr(&ctx->buf, rd, rm);
            break;
        }
        case XM_NOT: {
            X64Reg rm = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (rm != rd)
                x64_mov_rr(&ctx->buf, rd, rm);
            x64_not_r(&ctx->buf, rd);
            break;
        }
        case XM_SHL: {
            /* Shift amount must be in CL (low byte of RCX) */
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
            if (rm != X64_RCX)
                x64_mov_rr(&ctx->buf, X64_RCX, rm);
            if (rn != rd)
                x64_mov_rr(&ctx->buf, rd, rn);
            x64_shl_rcl(&ctx->buf, rd);
            break;
        }
        case XM_SHR: {
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
            if (rm != X64_RCX)
                x64_mov_rr(&ctx->buf, X64_RCX, rm);
            if (rn != rd)
                x64_mov_rr(&ctx->buf, rd, rn);
            x64_sar_rcl(&ctx->buf, rd);
            break;
        }

        /* ========== Comparison ========== */
        case XM_EQ:
        case XM_NE:
        case XM_LT:
        case XM_LE:
        case XM_GT:
        case XM_GE: {
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
            if (rn == rm) {
                /* CMP with itself: need scratch for one operand */
                x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, rm);
                rm = X64_SCRATCH_REG;
            }
            x64_cmp_rr(&ctx->buf, rn, rm);
            X64Cond cc;
            switch (ins->op) {
                case XM_EQ:
                    cc = X64_CC_E;
                    break;
                case XM_NE:
                    cc = X64_CC_NE;
                    break;
                case XM_LT:
                    cc = X64_CC_L;
                    break;
                case XM_LE:
                    cc = X64_CC_LE;
                    break;
                case XM_GT:
                    cc = X64_CC_G;
                    break;
                case XM_GE:
                    cc = X64_CC_GE;
                    break;
                default:
                    cc = X64_CC_E;
                    break;
            }
            /* Zero destination without clobbering CMP flags (MOV doesn't affect flags,
             * unlike XOR which would reset ZF/SF/OF and break the SETcc). */
            x64_mov_ri32(&ctx->buf, rd, 0);
            x64_setcc(&ctx->buf, cc, rd);
            break;
        }

        /* ========== Float Arithmetic (SSE2 scalar double) ========== */
        case XM_FADD: {
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            X64Xmm fm =
                x64_prepare_commutative_fp(ctx, fd, ins->args[0], ins->args[1], X64_SCRATCH_XMM);
            x64_addsd(&ctx->buf, fd, fm);
            break;
        }
        case XM_FSUB: {
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            x64_emit_noncommutative_fp(ctx, fd, ins->args[0], ins->args[1], x64_subsd);
            break;
        }
        case XM_FMUL: {
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            X64Xmm fm =
                x64_prepare_commutative_fp(ctx, fd, ins->args[0], ins->args[1], X64_SCRATCH_XMM);
            x64_mulsd(&ctx->buf, fd, fm);
            break;
        }
        case XM_FDIV: {
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            x64_emit_noncommutative_fp(ctx, fd, ins->args[0], ins->args[1], x64_divsd);
            break;
        }
        case XM_FNEG: {
            /* x86-64 has no scalar FNEG; XOR sign bit with 0x8000000000000000.
             * Load mask into GP scratch → MOVQ to xmm scratch → XORPD. */
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
            if (fn != fd)
                x64_movsd_rr(&ctx->buf, fd, fn);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, 0x8000000000000000ULL);
            x64_movq_xmm_gp(&ctx->buf, X64_SCRATCH_XMM, X64_SCRATCH_REG);
            x64_xorpd(&ctx->buf, fd, X64_SCRATCH_XMM);
            break;
        }
        case XM_CONST_F64: {
            /* Load f64 constant: raw bits into GP, then MOVQ to xmm */
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "CONST_F64: const OOB");
                uint64_t raw = ctx->func->consts[ci].val.raw;
                if (raw == 0) {
                    /* XORPD xmm, xmm → +0.0 (3 bytes, faster than MOVQ) */
                    x64_xorpd(&ctx->buf, fd, fd);
                } else {
                    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, raw);
                    x64_movq_xmm_gp(&ctx->buf, fd, X64_SCRATCH_REG);
                }
            }
            break;
        }

        /* ========== Type Conversion ========== */
        case XM_I2F: {
            /* CVTSI2SD xmm, r64 */
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_cvtsi2sd(&ctx->buf, fd, rn);
            break;
        }
        case XM_F2I: {
            /* CVTTSD2SI r64, xmm (truncation toward zero) */
            X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
            x64_cvttsd2si(&ctx->buf, rd, fn);
            break;
        }

        /* ========== Float Comparison ========== */
        case XM_FEQ:
        case XM_FNE:
        case XM_FLT:
        case XM_FLE: {
            /* UCOMISD sets ZF/PF/CF; use SETcc for the result.
             * Unordered (NaN): PF=1, CF=1, ZF=1 → all comparisons false.
             * We handle NaN by using SETNP+SETcc+AND for EQ,
             * or just appropriate condition codes for ordered comparisons. */
            X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
            X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
            x64_ucomisd(&ctx->buf, fn, fm);
            x64_xor_rr(&ctx->buf, rd, rd); /* zero-extend */
            switch (ins->op) {
                case XM_FEQ:
                    /* Equal and not unordered: SETE + SETNP + AND */
                    x64_setcc(&ctx->buf, X64_CC_E, rd);
                    x64_setcc(&ctx->buf, X64_CC_NP, X64_SCRATCH_REG);
                    x64_and_rr(&ctx->buf, rd, X64_SCRATCH_REG);
                    break;
                case XM_FNE:
                    /* Not-equal or unordered: SETNE + SETP + OR */
                    x64_setcc(&ctx->buf, X64_CC_NE, rd);
                    x64_setcc(&ctx->buf, X64_CC_P, X64_SCRATCH_REG);
                    x64_or_rr(&ctx->buf, rd, X64_SCRATCH_REG);
                    break;
                case XM_FLT:
                    /* a < b (ordered): UCOMISD(a,b) then SETB (CF=1, ZF=0) */
                    x64_setcc(&ctx->buf, X64_CC_B, rd);
                    break;
                case XM_FLE:
                    /* a <= b (ordered): UCOMISD(a,b) then SETBE (CF=1 or ZF=1) */
                    x64_setcc(&ctx->buf, X64_CC_BE, rd);
                    break;
                default:
                    break;
            }
            break;
        }

        /* ========== Move ========== */
        case XM_MOV:
        case XM_REDEFINE: {
            /* Handle both GP and FP moves */
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
            break;
        }

        /* ========== Select (conditional move) ========== */
        case XM_SELECT_COND: {
            /* Compare condition to zero, set flags for subsequent SELECT */
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_test_rr(&ctx->buf, rn, rn);
            break;
        }
        case XM_SELECT: {
            /* CMOVNE dst, true_val  (if NE=nonzero → take true_val) */
            X64Reg true_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg false_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
            if (false_reg != rd)
                x64_mov_rr(&ctx->buf, rd, false_reg);
            x64_cmov_rr(&ctx->buf, X64_CC_NE, rd, true_reg);
            break;
        }

        /* ========== Memory — basic load/store ========== */
        case XM_LOAD: {
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            bool load_fp = (ins->rep == XR_REP_F64);
            if (load_fp) {
                X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
                x64_movsd_rm(&ctx->buf, fd, rn, 0);
            } else {
                x64_mov_rm(&ctx->buf, rd, rn, 0);
            }
            break;
        }
        case XM_STORE: {
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            bool store_fp = false;
            if (xm_ref_is_vreg(ins->args[1])) {
                uint32_t vi = XM_REF_INDEX(ins->args[1]);
                if (vi < ctx->func->nvreg)
                    store_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
            }
            if (store_fp) {
                X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
                x64_movsd_mr(&ctx->buf, rn, 0, fm);
            } else {
                X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                if (rm == rn) {
                    x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, rm);
                    rm = X64_SCRATCH_REG;
                }
                x64_mov_mr(&ctx->buf, rn, 0, rm);
            }
            break;
        }

        /* ========== Sub-word loads/stores ========== */
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
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg val = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
            if (val == addr) {
                x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, val);
                val = X64_SCRATCH_REG;
            }
            x64_mov_mr8(&ctx->buf, addr, 0, val);
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
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg val = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
            if (val == addr) {
                x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, val);
                val = X64_SCRATCH_REG;
            }
            x64_mov_mr16(&ctx->buf, addr, 0, val);
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
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg val = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
            if (val == addr) {
                x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, val);
                val = X64_SCRATCH_REG;
            }
            x64_mov_mr32(&ctx->buf, addr, 0, val);
            break;
        }

        /* ========== Float sub-word memory ========== */
        case XM_LOAD_F32: {
            /* Load 32-bit float, promote to f64.
             * x86-64: CVTSS2SD xmm, [addr] — but we don't have that encoded.
             * Use: MOV r32d, [addr] → MOVD xmm, r32 → CVTSS2SD xmm, xmm.
             * Simpler: just do MOVSS + CVTSS2SD via raw encoding. */
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            /* MOVSS xmm_scratch, [addr]:  F3 0F 10 /r */
            x64_emit8(&ctx->buf, 0xF3);
            if (X64_SCRATCH_XMM > 7 || addr > 7)
                x64_emit8(&ctx->buf, 0x40 | ((X64_SCRATCH_XMM > 7) ? 4 : 0) | ((addr > 7) ? 1 : 0));
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, 0x10);
            x64_modrm_mem(&ctx->buf, X64_SCRATCH_XMM & 7, addr, 0);
            /* CVTSS2SD fd, xmm_scratch:  F3 0F 5A /r */
            x64_emit8(&ctx->buf, 0xF3);
            if ((int) fd > 7 || X64_SCRATCH_XMM > 7)
                x64_emit8(&ctx->buf,
                          0x40 | (((int) fd > 7) ? 4 : 0) | ((X64_SCRATCH_XMM > 7) ? 1 : 0));
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, 0x5A);
            x64_emit8(&ctx->buf, 0xC0 | (((int) fd & 7) << 3) | (X64_SCRATCH_XMM & 7));
            break;
        }
        case XM_STORE_F32: {
            /* Truncate f64 to f32, store 32-bit float.
             * CVTSD2SS xmm_scratch, src → MOVSS [addr], xmm_scratch */
            X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
            /* CVTSD2SS xmm_scratch, fm:  F2 0F 5A /r */
            x64_emit8(&ctx->buf, 0xF2);
            if (X64_SCRATCH_XMM > 7 || (int) fm > 7)
                x64_emit8(&ctx->buf,
                          0x40 | ((X64_SCRATCH_XMM > 7) ? 4 : 0) | (((int) fm > 7) ? 1 : 0));
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, 0x5A);
            x64_emit8(&ctx->buf, 0xC0 | ((X64_SCRATCH_XMM & 7) << 3) | ((int) fm & 7));
            /* MOVSS [addr], xmm_scratch:  F3 0F 11 /r */
            x64_emit8(&ctx->buf, 0xF3);
            if (X64_SCRATCH_XMM > 7 || addr > 7)
                x64_emit8(&ctx->buf, 0x40 | ((X64_SCRATCH_XMM > 7) ? 4 : 0) | ((addr > 7) ? 1 : 0));
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, 0x11);
            x64_modrm_mem(&ctx->buf, X64_SCRATCH_XMM & 7, addr, 0);
            break;
        }

        /* ========== Coro/context loads/stores ========== */
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

        /* ========== Object field load/store ========== */
        case XM_LOAD_FIELD: {
            X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            int32_t offset = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            /* Save runtime tag to jit_ctx scratch if type is unknown */
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg && ins->ctype.kind == XM_TK_UNKNOWN &&
                    ins->rep == XR_REP_I64) {
                    x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, obj, offset + XM_XRVALUE_TAG_OFFSET);
                    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_LOAD_TAG_SCRATCH,
                               X64_SCRATCH_REG);
                }
            }
            /* Load payload at byte 8 */
            if (ins->rep == XR_REP_F64) {
                X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
                x64_movsd_rm(&ctx->buf, fd, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET);
            } else {
                x64_mov_rm(&ctx->buf, rd, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET);
            }
            break;
        }
        case XM_STORE_FIELD: {
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
                        uint8_t vt =
                            (vi < ctx->func->nvreg) ? ctx->func->vregs[vi].rep : XR_REP_TAGGED;
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
                /* PTR: read gc_type from GC header, build tag|(gc_type<<16) */
                X64Reg val = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                /* gc_type = *(uint8_t*)(val + XM_GC_TYPE_OFFSET) */
                x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, val, (int32_t) XM_GC_TYPE_OFFSET);
                /* scratch = gc_type << 16 */
                x64_shl_ri(&ctx->buf, X64_SCRATCH_REG, 16);
                /* scratch |= tag_val */
                x64_or_ri(&ctx->buf, X64_SCRATCH_REG, (int32_t) tag_val);
                /* Store 32-bit descriptor */
                x64_mov_mr32(&ctx->buf, obj, offset + XM_XRVALUE_TAG_OFFSET, X64_SCRATCH_REG);
            } else {
                /* Non-PTR: tag with heap_type=0 */
                x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, tag_val);
                x64_mov_mr32(&ctx->buf, obj, offset + XM_XRVALUE_TAG_OFFSET, X64_SCRATCH_REG);
            }
            break;
        }

        /* ========== Tag load/check ========== */
        case XM_TAG_LOAD: {
            X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->args[1]) && xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            x64_movzx_rm8(&ctx->buf, rd, ptr, offset);
            break;
        }

        /* ========== BOX/UNBOX ========== */
        case XM_BOX_I64:
        case XM_BOX_F64: {
            /* BOX is a no-op inside JIT (values are always raw/untagged) */
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (rd != rn)
                x64_mov_rr(&ctx->buf, rd, rn);
            break;
        }

        case XM_UNBOX_I64: {
            uint8_t src_type = XR_REP_I64;
            if (xm_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XM_REF_INDEX(ins->args[0]);
                if (vi < ctx->func->nvreg)
                    src_type = ctx->func->vregs[vi].rep;
            }
            if (src_type == XR_REP_PTR) {
                /* Source is pointer to XrValue — load payload from [ptr+8] */
                X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                x64_mov_rm(&ctx->buf, rd, ptr, XM_XRVALUE_PAYLOAD_OFFSET);
            } else {
                X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                if (rd != rn)
                    x64_mov_rr(&ctx->buf, rd, rn);
            }
            break;
        }

        case XM_UNBOX_F64: {
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
            break;
        }

        /* ========== Guard ops ========== */
        case XM_GUARD_TAG: {
            /* args[0] = tagged value ptr, args[1] = expected tag (const i64) */
            X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            /* Load tag byte: MOVZX r64, byte [val_reg + tag_offset] */
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
            /* deopt if (unsigned)index >= (unsigned)length */
            X64Reg idx_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg len_reg = x64_get_operand(
                ctx, ins->args[1], idx_reg == X64_SCRATCH_REG ? X64_RCX : X64_SCRATCH_REG);
            x64_cmp_rr(&ctx->buf, idx_reg, len_reg);
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_AE); /* unsigned >= */
            break;
        }

        case XM_GUARD_NONNULL: {
            X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_test_rr(&ctx->buf, val_reg, val_reg);
            x64_emit_deopt_id(ctx, ins);
            x64_emit_deopt_jcc(ctx, X64_CC_E); /* ZF=1 → null → deopt */
            break;
        }

        case XM_GUARD_CLASS: {
            /* Check shape_id in GC header (uint16 at gc_extra_offset) */
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
            /* Check inst->klass pointer (at offset XM_INSTANCE_KLASS_OFFSET) */
            X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, obj, (int32_t) XM_INSTANCE_KLASS_OFFSET);
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
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

        case XM_GUARD_SHAPE: {
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
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, obj, (int32_t) XM_GC_HDR_TYPE_OFFSET);
            x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, 23);
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            /* Load gc.extra (uint16 at offset 10) */
            x64_movzx_rm16(&ctx->buf, X64_SCRATCH_REG, obj, (int32_t) XM_GC_HDR_EXTRA_OFFSET);
            /* shape_id = extra >> 2 */
            x64_shr_ri(&ctx->buf, X64_SCRATCH_REG, 2);
            /* Compare with expected shape_id */
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                int32_t expected_id = (int32_t) ctx->func->consts[ci].val.raw;
                x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, expected_id);
            }
            x64_emit_deopt_jcc(ctx, X64_CC_NE);
            break;
        }

        case XM_DEOPT: {
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

        case XM_TAG_CHECK: {
            /* Same as GUARD_TAG but may have different dst semantics */
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

        case XM_SAFEPOINT: {
            /* Record safepoint bitmap so GC can identify roots if we fault */
            uint32_t smap_id_sp = x64_record_safepoint(ctx);
            /* Store smap_id to jit_ctx (GC reads this in signal handler) */
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_sp);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_SCRATCH_REG);
            /* Guard page poll: load from safepoint_page → faults if armed */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG,
                       (int32_t) XM_JIT_SAFEPOINT_PAGE_OFFSET);
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG, 0);
            break;
        }

        /* ========== Mixed-type runtime arithmetic ========== */
        case XM_RT_ADD:
        case XM_RT_SUB:
        case XM_RT_MUL:
        case XM_RT_DIV:
        case XM_RT_MOD: {
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

        case XM_RT_UNM: {
            uint8_t ta = XR_REP_I64;
            if (xm_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XM_REF_INDEX(ins->args[0]);
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

        case XM_RT_LT:
        case XM_RT_LE:
        case XM_RT_EQ: {
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
                x64_ucomisd(&ctx->buf, fa, fb);
                /* UCOMISD sets ZF/CF for unordered float comparison.
                 * SETcc to get 0/1 result in rd. */
                X64Cond cc;
                if (ins->op == XM_RT_LT)
                    cc = X64_CC_B; /* CF=1 → below */
                else if (ins->op == XM_RT_LE)
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

        case XM_RT_ARRAY_NEW:
        case XM_RT_MAP_NEW: {
            /* Inline CALL_C: alloc array/map via runtime helper.
             * args[0] = capacity, dst = result ptr. */
            x64_emit_ptr_spill_writeback(ctx);

            uint32_t smap_id = x64_record_safepoint(ctx);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_SCRATCH_REG);

            /* extra_arg = capacity */
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

            /* Clear deopt_id */
            x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET,
                         X64_SCRATCH_REG);

            /* Load runtime helper pointer */
            void *fn = (ins->op == XM_RT_ARRAY_NEW) ? (void *) (uintptr_t) xr_jit_rt_array_new
                                                     : (void *) (uintptr_t) xr_jit_rt_map_new;
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) fn);

            /* CALL call_c_stub */
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *p_an = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            p_an->emit_pos = ctx->buf.pos;
            p_an->target_blk = 0;
            p_an->type = X64_PATCH_CALL_C;
            p_an->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_call_c = true;

            /* Result ptr in RAX */
            if (xm_ref_is_vreg(ins->dst) && rd != X64_RAX)
                x64_mov_rr(&ctx->buf, rd, X64_RAX);
            break;
        }

        case XM_RT_ARRAY_PUSH: {
            /* Inline CALL_C: push value into array.
             * args[0] = array ptr, args[1] = value to push. */
            x64_emit_ptr_spill_writeback(ctx);

            uint32_t smap_id_ap = x64_record_safepoint(ctx);
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) smap_id_ap);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_SCRATCH_REG);

            /* Store call_args[0] = array ptr */
            int32_t ca0 = (int32_t) XM_JIT_CALL_ARGS_OFFSET;
            X64Reg arr_r = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, ca0, arr_r);

            /* Store call_args[1] = value payload */
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

            /* Store call_arg_tags: tag[0]=PTR(arr), tag[1]=inferred */
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
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET,
                       X64_SCRATCH_REG);

            /* Dynamic tag patch: if t1 unknown, read from slot_runtime_tags */
            if (t1 == XR_RTAG_UNKNOWN && xm_ref_is_vreg(ins->args[1])) {
                uint32_t ai = XM_REF_INDEX(ins->args[1]);
                if (ai < ctx->func->nvreg) {
                    int16_t bc_slot = ctx->func->vregs[ai].bc_slot;
                    if (bc_slot >= 0 && bc_slot < 256) {
                        int32_t soff = (int32_t) XM_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot;
                        int32_t doff = (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET + 1;
                        x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG, soff);
                        x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, doff, X64_SCRATCH_REG);
                    }
                }
            }

            /* extra_arg = 0, deopt_id = 0 (already zeroed together) */
            x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET, X64_SCRATCH_REG);
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET,
                         X64_SCRATCH_REG);

            /* Load runtime helper pointer */
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_rt_array_push);

            /* CALL call_c_stub */
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *p_ap = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            p_ap->emit_pos = ctx->buf.pos;
            p_ap->target_blk = 0;
            p_ap->type = X64_PATCH_CALL_C;
            p_ap->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_call_c = true;
            break;
        }

        case XM_RT_PRINT:
        case XM_RT_ARRAY_LEN:
        case XM_RT_INDEX_GET:
        case XM_RT_INDEX_SET:
            /* Not yet lowered to CALL_C */
            xr_log_warning("x64-cg", "RT opcode %d should use CALL_C path", ins->op);
            break;

        case XM_RT_ISNULL: {
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

        /* ========== Exception handling ========== */
        /* TRY_BEGIN / TRY_END / THROW are never emitted directly by the builder
         * in JIT mode; builder lowers them to CALL_C (xr_jit_throw) + GOTO/RET.
         * XM_CATCH is handled above as a direct exception load + clear. */
        case XM_TRY_BEGIN:
        case XM_TRY_END:
        case XM_THROW:
            break; /* no-op in JIT codegen */

        /* ========== Coroutine suspend (await/channel) ========== */
        case XM_SUSPEND: {
            /* Extract suspend_id from vreg metadata */
            uint32_t suspend_id = 0;
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg)
                    suspend_id = ctx->func->vregs[vi].call_arg_start;
            }

            /* Extract discard_result flag from args[1] */
            int64_t discard_result = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                discard_result = ctx->func->consts[ci].val.i64;
            }

            /* 1. Record safepoint bitmap for GC */
            uint32_t smap_id = x64_record_safepoint(ctx);

            /* 2. Load suspend_state pointer: R11 = coro->jit_suspend */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_CORO_REG,
                       (int32_t) XM_CORO_SUSPEND_PTR_OFFSET);
            CODEGEN_CHECK(ctx, suspend_id < 16, "suspend_id out of range");

            /* 3. Save caller-saved GP regs to caller_saved[] */
            for (int i = 0; i < X64_NGPR_CALLER_SAVE && i < X64_MAX_PHYS_REGS; i++)
                x64_mov_mr(&ctx->buf, X64_SCRATCH_REG, i * 8, x64_alloc_regs[i]);

            /* 4. Save callee-saved GP regs to callee_saved[] */
            for (int i = 0; i < X64_NGPR_CALLEE_SAVE_ALLOC; i++)
                x64_mov_mr(&ctx->buf, X64_SCRATCH_REG,
                           (int32_t) XM_SUSPEND_CALLEE_SAVED_OFF + i * 8,
                           x64_alloc_regs[X64_NGPR_CALLER_SAVE + i]);

            /* 5. Save spill slots to suspend_state.spill[] */
            {
                uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
                if (ns > XM_SUSPEND_SPILL_MAX)
                    ns = XM_SUSPEND_SPILL_MAX;
                for (uint32_t s = 0; s < ns; s++) {
                    int32_t frame_off = -(int32_t) (X64_SPILL_BASE + s * 8);
                    int32_t regs_off = (int32_t) (XM_SUSPEND_SPILL_OFF + s * 8);
                    /* Load from stack frame, store to suspend_state */
                    x64_mov_rm(&ctx->buf, X64_RCX, X64_RBP, frame_off);
                    x64_mov_mr(&ctx->buf, X64_SCRATCH_REG, regs_off, X64_RCX);
                }
            }

            /* 6. Store suspend_id and smap_id to coro fields */
            x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t) suspend_id);
            x64_mov_mr32(&ctx->buf, X64_CORO_REG, (int32_t) XM_CORO_SUSPEND_ID_OFFSET, X64_RCX);
            x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t) smap_id);
            x64_mov_mr32(&ctx->buf, X64_CORO_REG, (int32_t) XM_CORO_SUSPEND_SMAP_OFFSET, X64_RCX);
            /* Update jit_ctx active_smap_id for GC during blocked state */
            x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                         X64_RCX);

            /* 7. Pre-store resume info (gopark pattern: must be set before
             * block_helper, since another worker may wake this coro immediately) */
            x64_mov_rm(&ctx->buf, X64_RCX, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_PROTO_OFFSET);
            x64_mov_mr(&ctx->buf, X64_CORO_REG, (int32_t) XM_CORO_RESUME_PROTO_OFFSET, X64_RCX);
            x64_mov_rm(&ctx->buf, X64_RCX, X64_RCX, (int32_t) XM_PROTO_JIT_RESUME_ENTRY_OFFSET);
            x64_mov_mr(&ctx->buf, X64_CORO_REG, (int32_t) XM_CORO_RESUME_ENTRY_OFFSET, X64_RCX);

            /* 8. Call block_helper(coro, extra_arg) via platform ABI */
            void *block_helper = ctx->func->suspend_block_helpers[suspend_id];
            int64_t helper_extra_arg = 0;
            if (!block_helper) {
                block_helper = (void *) xr_jit_await_block;
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

            /* 9. Check result: RAX == 0 → blocked, RAX != 0 → inline resume */
            x64_test_rr(&ctx->buf, X64_RAX, X64_RAX);
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_NE));
            uint32_t jne_not_blocked = ctx->buf.pos;
            x64_emit32(&ctx->buf, 0);

            /* === Blocked path: return SUSPEND_MARKER === */
            x64_load_imm64(&ctx->buf, X64_RAX, (uint64_t) XM_SUSPEND_MARKER);
            x64_emit_epilogue(ctx);

            /* === Not-blocked path: inline resume === */
            x64_patch_rel32(&ctx->buf, jne_not_blocked, ctx->buf.pos);

            /* Reload suspend pointer (R11 clobbered by CALL) */
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_CORO_REG,
                       (int32_t) XM_CORO_SUSPEND_PTR_OFFSET);

            /* Reload caller-saved GP regs (callee-saved survived the CALL) */
            for (int i = 0; i < X64_NGPR_CALLER_SAVE && i < X64_MAX_PHYS_REGS; i++)
                x64_mov_rm(&ctx->buf, x64_alloc_regs[i], X64_SCRATCH_REG, i * 8);

            /* Load await/channel result from suspend_state.result into dst */
            if (rd != X64_SCRATCH_REG) {
                x64_mov_rm(&ctx->buf, rd, X64_SCRATCH_REG, (int32_t) XM_SUSPEND_RESULT_OFF);
            }

            /* Load result_tag → runtime_tags[bc_slot] */
            {
                int16_t res_bc_slot = -1;
                if (xm_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XM_REF_INDEX(ins->dst);
                    if (vi < ctx->func->nvreg)
                        res_bc_slot = ctx->func->vregs[vi].bc_slot;
                }
                if (res_bc_slot >= 0 && res_bc_slot < 256) {
                    x64_movzx_rm8(&ctx->buf, X64_RCX, X64_SCRATCH_REG,
                                  (int32_t) XM_SUSPEND_RESULT_TAG_OFF);
                    int32_t tag_off = (int32_t) XM_JIT_SLOT_RUNTIME_TAGS_OFFSET + res_bc_slot;
                    x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, tag_off, X64_RCX);
                }
                if (suspend_id < 16)
                    ctx->suspend_result_bc_slots[suspend_id] = res_bc_slot;
            }

            /* Record continuation point for resume entry jump table */
            if (suspend_id < 16) {
                ctx->suspend_cont_offsets[suspend_id] = ctx->buf.pos;
                ctx->suspend_smap_ids[suspend_id] = smap_id;
                ctx->suspend_result_regs[suspend_id] =
                    (uint8_t) (xm_ref_is_vreg(ins->dst) ? rd : X64_SCRATCH_REG);
                if (suspend_id >= ctx->nsuspend)
                    ctx->nsuspend = suspend_id + 1;
            }
            break;
        }

        case XM_BARRIER_FWD: {
            /* Forward write barrier: parent = args[0], child = args[1].
             * Move to scratch regs then CALL shared barrier_fwd_stub. */
            X64Reg parent_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Reg child_reg = x64_get_operand(ctx, ins->args[1], X64_RCX);
            if (parent_reg != X64_SCRATCH_REG)
                x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, parent_reg);
            if (child_reg != X64_RCX)
                x64_mov_rr(&ctx->buf, X64_RCX, child_reg);
            /* CALL rel32 → barrier_fwd_stub */
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *bp = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            bp->emit_pos = ctx->buf.pos;
            bp->target_blk = 0;
            bp->type = X64_PATCH_BARRIER_FWD;
            bp->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_barriers = true;
            break;
        }
        case XM_BARRIER_BACK: {
            /* Back write barrier: container = args[0].
             * Move to scratch reg then CALL shared barrier_back_stub. */
            X64Reg container_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (container_reg != X64_SCRATCH_REG)
                x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, container_reg);
            /* CALL rel32 → barrier_back_stub */
            CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *bp = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            bp->emit_pos = ctx->buf.pos;
            bp->target_blk = 0;
            bp->type = X64_PATCH_BARRIER_BACK;
            bp->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
            ctx->has_barriers = true;
            break;
        }

        /* ========== GC allocation (inline fast path + slow path fallback) ========== */
        case XM_ALLOC: {
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
            break;
        }

        case XM_CATCH: {
            /* Load exception from jit_ctx->exception, then clear it */
            x64_mov_rm(&ctx->buf, rd, X64_JIT_CTX_REG, (int32_t) XM_JIT_EXCEPTION_OFFSET);
            x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_EXCEPTION_OFFSET,
                       X64_SCRATCH_REG);
            break;
        }

        /* ========== Call ops — delegated to xm_codegen_x64_call.c ========== */
        case XM_CALL_C:
        case XM_CALL_C_LEAF:
        case XM_CALL_SELF_DIRECT:
        case XM_CALL_KNOWN:
        case XM_CALL_KNOWN_REG:
        case XM_CALL_DIRECT:
        case XM_CALL:
            if (!x64_emit_call_ins(ctx, ins, rd))
                ctx->had_error = true;
            break;

        case XM_RET: {
            /* Return value is in the register assigned to args[0].
             * JIT calling convention: return value in RAX. */
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (rn != X64_RAX)
                x64_mov_rr(&ctx->buf, X64_RAX, rn);
            /* Epilogue will be emitted as part of block terminator */
            break;
        }

        case XM_NOP:
        case XM_PHI:
            break;

        default:
            /* Unsupported opcode — flag error, codegen will report failure */
            xr_log_warning("x64-cg", "unsupported Xm opcode %d", ins->op);
            ctx->had_error = true;
            break;
    }
}

#endif  /* __x86_64__ || _M_X64 */
