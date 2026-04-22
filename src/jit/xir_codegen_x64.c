/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_codegen_x64.c - XIR → x86-64 machine code generation
 *
 * KEY CONCEPT:
 *   Translates XIR SSA instructions into x86-64 machine code.
 *   Single-pass emit with deferred rel32 branch patching.
 *
 * x86-64 DIFFERENCES FROM ARM64:
 *   - 2-operand ISA: dst = dst OP src (need MOV dst,src1 first)
 *   - DIV/MOD use implicit RAX/RDX (need register shuffling)
 *   - Shifts use implicit CL register for shift amount
 *   - Variable-length encoding → byte offsets, not instruction indices
 *   - No link register → CALL pushes return address on stack
 *   - Condition codes persist until next flag-setting instruction
 *
 * STATUS: Phase F.4.2 — integer arithmetic + control flow.
 *
 * RELATED MODULES:
 *   - xir_x64.h/c: x86-64 instruction encoding
 *   - xir_target_x64.c: register inventory and frame layout
 *   - xir_codegen.h: shared codegen result structure
 *   - xir_codegen_x64_internal.h: internal context types
 */

#ifdef __x86_64__

#include "xir_codegen_x64_internal.h"
#include "xir_coalesce.h"
#include "xir_code_alloc.h"
#include "xir_offsets.h"
#include "xir_jit_runtime.h"
#include <string.h>

/* Forward declarations for helpers (defined after emit_xir_ins) */
static void x64_emit_call_args_from_pool(X64CodegenCtx *ctx, XirIns *ins);
static inline uint8_t const_rep_to_value_tag(uint8_t rep);
static void x64_emit_deopt_jcc(X64CodegenCtx *ctx, X64Cond cc);
static void x64_emit_deopt_id(X64CodegenCtx *ctx, XirIns *ins);
static uint32_t x64_record_safepoint(X64CodegenCtx *ctx);
static void x64_emit_ptr_spill_writeback(X64CodegenCtx *ctx);
static int x64_live_gp(X64CodegenCtx *ctx, X64Reg *out, X64Reg exclude);
static int x64_live_fp(X64CodegenCtx *ctx, X64Xmm *out);

/* ========== Register Mapping Tables ========== */

const X64Reg x64_alloc_regs[X64_MAX_PHYS_REGS] = {
    /* Caller-saved (first 8) */
    X64_RAX, X64_RCX, X64_RDX, X64_RSI, X64_RDI,
    X64_R8,  X64_R9,  X64_R10,
    /* Callee-saved (next 3) — r14=jit_ctx, r15=coro, rbp=FP excluded */
    X64_RBX, X64_R12, X64_R13,
};

const X64Reg x64_alloc_fp_regs[X64_MAX_FP_REGS] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

/* ========== Register Lookup ========== */

X64Reg x64_get_reg(X64CodegenCtx *ctx, XirRef ref) {
    if (xir_ref_is_none(ref)) return X64_SCRATCH_REG;
    if (!xir_ref_is_vreg(ref)) return X64_SCRATCH_REG;
    uint32_t idx = XIR_REF_INDEX(ref);

    /* FP vregs use xmm registers — return scratch GP to avoid misuse.
     * Callers handling FP ops use x64_get_fp_reg() instead. */
    if (idx < ctx->func->nvreg && ctx->func->vregs[idx].rep == XR_REP_F64)
        return X64_SCRATCH_REG;

    int8_t ri;
    if (ctx->vreg_override && idx < ctx->xra->nvreg &&
        ctx->vreg_override[idx] != -128)
        ri = ctx->vreg_override[idx];
    else {
        ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    }

    if (ri < 0) {
        /* Vreg not in register: check for spill reload */
        if (xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos) ||
            xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos + 1)) {
            int16_t slot = xra_vreg_spill(ctx->xra, idx);
            if (slot >= 0) {
                int32_t offset = -(X64_SPILL_BASE + slot * 8);
                x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_RBP, offset);
                return X64_SCRATCH_REG;
            }
        }
        ctx->had_error = true;
        return X64_SCRATCH_REG;
    }
    if (ri >= X64_MAX_PHYS_REGS) {
        ctx->had_error = true;
        return X64_SCRATCH_REG;
    }
    return x64_alloc_regs[ri];
}

X64Reg x64_get_operand(X64CodegenCtx *ctx, XirRef ref, X64Reg scratch) {
    /* Constants: load into scratch */
    if (xir_ref_is_const(ref)) {
        uint32_t ci = XIR_REF_INDEX(ref);
        XR_DCHECK(ci < ctx->func->nconst, "x64_get_operand: const OOB");
        uint64_t val = ctx->func->consts[ci].val.raw;
        x64_load_imm64(&ctx->buf, scratch, val);
        return scratch;
    }
    return x64_get_reg(ctx, ref);
}

/* ========== FP Register Lookup ========== */

static bool x64_is_fp_vreg(X64CodegenCtx *ctx, XirRef ref) {
    if (!xir_ref_is_vreg(ref)) return false;
    uint32_t idx = XIR_REF_INDEX(ref);
    return idx < ctx->func->nvreg && ctx->func->vregs[idx].rep == XR_REP_F64;
}

X64Xmm x64_get_fp_reg(X64CodegenCtx *ctx, XirRef ref) {
    XR_DCHECK(xir_ref_is_vreg(ref), "x64_get_fp_reg: not a vreg");
    uint32_t idx = XIR_REF_INDEX(ref);

    int8_t ri;
    if (ctx->vreg_override && idx < ctx->xra->nvreg &&
        ctx->vreg_override[idx] != -128)
        ri = ctx->vreg_override[idx];
    else {
        ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    }

    if (ri < 0) {
        /* FP spill reload via MOVSD */
        if (xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos) ||
            xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos + 1)) {
            int16_t slot = xra_vreg_spill(ctx->xra, idx);
            if (slot >= 0) {
                int32_t offset = -(X64_SPILL_BASE + slot * 8);
                x64_movsd_rm(&ctx->buf, X64_SCRATCH_XMM, X64_RBP, offset);
                return X64_SCRATCH_XMM;
            }
        }
        ctx->had_error = true;
        return X64_SCRATCH_XMM;
    }
    XR_DCHECK(ri < X64_MAX_FP_REGS, "x64_get_fp_reg: reg index OOB");
    return x64_alloc_fp_regs[ri];
}

X64Xmm x64_get_fp_operand(X64CodegenCtx *ctx, XirRef ref, X64Xmm scratch) {
    if (xir_ref_is_const(ref)) {
        /* Load float constant: GP load → MOVQ to xmm */
        uint32_t ci = XIR_REF_INDEX(ref);
        XR_DCHECK(ci < ctx->func->nconst, "x64_get_fp_operand: const OOB");
        uint64_t raw = ctx->func->consts[ci].val.raw;
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, raw);
        x64_movq_xmm_gp(&ctx->buf, scratch, X64_SCRATCH_REG);
        return scratch;
    }
    return x64_get_fp_reg(ctx, ref);
}

/* ========== Immediate Helpers ========== */

void x64_load_imm64(X64Buf *buf, X64Reg dst, uint64_t val) {
    if (val == 0) {
        /* XOR r32, r32 — shorter and faster than MOV r64, 0 */
        x64_xor_rr(buf, dst, dst);
    } else if (val <= 0xFFFFFFFF) {
        /* 32-bit MOV (zero-extends to 64 bits) */
        x64_mov_ri32(buf, dst, (uint32_t)val);
    } else {
        /* Full 64-bit MOV */
        x64_mov_ri64(buf, dst, val);
    }
}

void x64_maybe_spill(X64CodegenCtx *ctx, XirRef dst_ref) {
    if (!xir_ref_is_vreg(dst_ref)) return;
    uint32_t idx = XIR_REF_INDEX(dst_ref);
    int16_t slot = xra_vreg_spill(ctx->xra, idx);
    if (slot < 0) return;
    int8_t ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    if (ri < 0) return;
    int32_t offset = -(X64_SPILL_BASE + slot * 8);
    bool is_fp = x64_is_fp_vreg(ctx, dst_ref);
    if (is_fp)
        x64_movsd_mr(&ctx->buf, X64_RBP, offset, x64_alloc_fp_regs[ri]);
    else
        x64_mov_mr(&ctx->buf, X64_RBP, offset, x64_alloc_regs[ri]);
}

/* ========== Branch Patching ========== */

void x64_add_patch(X64CodegenCtx *ctx, X64PatchType type,
                    uint32_t target_blk, X64Cond cc) {
    if (ctx->npatch >= ctx->patches_cap) {
        uint32_t new_cap = ctx->patches_cap * 2;
        XR_REALLOC_OR_ABORT(ctx->patches, new_cap * sizeof(X64BranchPatch),
                            "x64 codegen patches grow");
        ctx->patches_cap = new_cap;
    }
    X64BranchPatch *p = &ctx->patches[ctx->npatch++];
    /* For JMP/Jcc rel32: the rel32 field is at buf.pos (about to emit).
     * We record the position of the rel32 displacement itself,
     * not the opcode. Caller must emit the instruction AFTER this call. */
    p->emit_pos = 0;  /* Set by caller after emitting opcode bytes */
    p->target_blk = target_blk;
    p->type = type;
    p->cc = cc;
}

/* ========== Gap Moves ========== */

static void x64_emit_gap_moves_before(X64CodegenCtx *ctx, uint32_t ins_idx) {
    if (!ctx->xra || !ctx->xra->gap_moves) return;
    uint32_t blk_id = ctx->cur_blk_id;
    uint32_t cursor = ctx->gap_move_cursor;
    while (cursor < ctx->xra->ngap_move) {
        XraGapMove *gm = &ctx->xra->gap_moves[cursor];
        if (gm->gap_blk > blk_id) break;
        if (gm->gap_blk < blk_id) { cursor++; continue; }
        if (gm->gap_ins_idx > ins_idx) break;
        if (gm->gap_ins_idx == ins_idx) {
            X64Reg src_hw = x64_alloc_regs[gm->src_reg];
            X64Reg dst_hw = x64_alloc_regs[gm->dst_reg];
            if (src_hw != dst_hw)
                x64_mov_rr(&ctx->buf, dst_hw, src_hw);
            /* Record override so subsequent lookups see the new register */
            if (ctx->vreg_override && gm->vreg < ctx->xra->nvreg)
                ctx->vreg_override[gm->vreg] = gm->dst_reg;
        }
        cursor++;
    }
    ctx->gap_move_cursor = cursor;
}

/* ========== Instruction Emission ========== */

/*
 * x86-64 2-operand pattern: dst = dst OP src.
 * If dst != src1, emit MOV dst, src1 first.
 * Returns the hardware register holding src1 (== dst after MOV).
 */
static X64Reg x64_ensure_dst(X64CodegenCtx *ctx, X64Reg rd,
                              XirRef arg0, X64Reg scratch) {
    X64Reg rn = x64_get_operand(ctx, arg0, scratch);
    if (rn != rd)
        x64_mov_rr(&ctx->buf, rd, rn);
    return rd;
}

static void x64_emit_xir_ins(X64CodegenCtx *ctx, XirIns *ins) {
    XR_DCHECK(ctx != NULL, "x64_emit_xir_ins: NULL ctx");
    XR_DCHECK(ins != NULL, "x64_emit_xir_ins: NULL ins");
    X64Reg rd = x64_get_reg(ctx, ins->dst);

    switch (ins->op) {
    /* ========== Constants ========== */
    case XIR_CONST_I64:
    case XIR_CONST_PTR: {
        if (xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            XR_DCHECK(ci < ctx->func->nconst, "const index OOB");
            x64_load_imm64(&ctx->buf, rd, ctx->func->consts[ci].val.raw);
        }
        break;
    }

    /* ========== Integer Arithmetic ========== */
    case XIR_ADD: {
        x64_ensure_dst(ctx, rd, ins->args[0], X64_SCRATCH_REG);
        X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        x64_add_rr(&ctx->buf, rd, rm);
        break;
    }
    case XIR_SUB: {
        x64_ensure_dst(ctx, rd, ins->args[0], X64_SCRATCH_REG);
        X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        x64_sub_rr(&ctx->buf, rd, rm);
        break;
    }
    case XIR_MUL: {
        x64_ensure_dst(ctx, rd, ins->args[0], X64_SCRATCH_REG);
        X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        x64_imul_rr(&ctx->buf, rd, rm);
        break;
    }
    case XIR_DIV:
    case XIR_MOD: {
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
        X64Reg result_reg = (ins->op == XIR_DIV) ? X64_RAX : X64_RDX;
        if (rd != result_reg)
            x64_mov_rr(&ctx->buf, rd, result_reg);
        break;
    }
    case XIR_NEG: {
        X64Reg rm = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        if (rm != rd)
            x64_mov_rr(&ctx->buf, rd, rm);
        x64_neg_r(&ctx->buf, rd);
        break;
    }

    /* ========== Logical ========== */
    case XIR_AND: {
        x64_ensure_dst(ctx, rd, ins->args[0], X64_SCRATCH_REG);
        X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        x64_and_rr(&ctx->buf, rd, rm);
        break;
    }
    case XIR_OR: {
        x64_ensure_dst(ctx, rd, ins->args[0], X64_SCRATCH_REG);
        X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        x64_or_rr(&ctx->buf, rd, rm);
        break;
    }
    case XIR_XOR: {
        x64_ensure_dst(ctx, rd, ins->args[0], X64_SCRATCH_REG);
        X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        x64_xor_rr(&ctx->buf, rd, rm);
        break;
    }
    case XIR_NOT: {
        X64Reg rm = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        if (rm != rd)
            x64_mov_rr(&ctx->buf, rd, rm);
        x64_not_r(&ctx->buf, rd);
        break;
    }
    case XIR_SHL: {
        /* Shift amount must be in CL (low byte of RCX) */
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        if (rm != X64_RCX) x64_mov_rr(&ctx->buf, X64_RCX, rm);
        if (rn != rd) x64_mov_rr(&ctx->buf, rd, rn);
        x64_shl_rcl(&ctx->buf, rd);
        break;
    }
    case XIR_SHR: {
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        X64Reg rm = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        if (rm != X64_RCX) x64_mov_rr(&ctx->buf, X64_RCX, rm);
        if (rn != rd) x64_mov_rr(&ctx->buf, rd, rn);
        x64_sar_rcl(&ctx->buf, rd);
        break;
    }

    /* ========== Comparison ========== */
    case XIR_EQ: case XIR_NE: case XIR_LT:
    case XIR_LE: case XIR_GT: case XIR_GE: {
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
            case XIR_EQ: cc = X64_CC_E;  break;
            case XIR_NE: cc = X64_CC_NE; break;
            case XIR_LT: cc = X64_CC_L;  break;
            case XIR_LE: cc = X64_CC_LE; break;
            case XIR_GT: cc = X64_CC_G;  break;
            case XIR_GE: cc = X64_CC_GE; break;
            default: cc = X64_CC_E; break;
        }
        /* Zero-extend destination first, then SETcc */
        x64_xor_rr(&ctx->buf, rd, rd);
        x64_setcc(&ctx->buf, cc, rd);
        break;
    }

    /* ========== Float Arithmetic (SSE2 scalar double) ========== */
    case XIR_FADD: {
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
        X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
        if (fn != fd) x64_movsd_rr(&ctx->buf, fd, fn);
        x64_addsd(&ctx->buf, fd, fm);
        break;
    }
    case XIR_FSUB: {
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
        X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
        if (fn != fd) x64_movsd_rr(&ctx->buf, fd, fn);
        x64_subsd(&ctx->buf, fd, fm);
        break;
    }
    case XIR_FMUL: {
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
        X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
        if (fn != fd) x64_movsd_rr(&ctx->buf, fd, fn);
        x64_mulsd(&ctx->buf, fd, fm);
        break;
    }
    case XIR_FDIV: {
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
        X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
        if (fn != fd) x64_movsd_rr(&ctx->buf, fd, fn);
        x64_divsd(&ctx->buf, fd, fm);
        break;
    }
    case XIR_FNEG: {
        /* x86-64 has no scalar FNEG; XOR sign bit with 0x8000000000000000.
         * Load mask into GP scratch → MOVQ to xmm scratch → XORPD. */
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
        if (fn != fd) x64_movsd_rr(&ctx->buf, fd, fn);
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, 0x8000000000000000ULL);
        x64_movq_xmm_gp(&ctx->buf, X64_SCRATCH_XMM, X64_SCRATCH_REG);
        x64_xorpd(&ctx->buf, fd, X64_SCRATCH_XMM);
        break;
    }
    case XIR_CONST_F64: {
        /* Load f64 constant: raw bits into GP, then MOVQ to xmm */
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        if (xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            XR_DCHECK(ci < ctx->func->nconst, "CONST_F64: const OOB");
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
    case XIR_I2F: {
        /* CVTSI2SD xmm, r64 */
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_cvtsi2sd(&ctx->buf, fd, rn);
        break;
    }
    case XIR_F2I: {
        /* CVTTSD2SI r64, xmm (truncation toward zero) */
        X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
        x64_cvttsd2si(&ctx->buf, rd, fn);
        break;
    }

    /* ========== Float Comparison ========== */
    case XIR_FEQ: case XIR_FNE: case XIR_FLT: case XIR_FLE: {
        /* UCOMISD sets ZF/PF/CF; use SETcc for the result.
         * Unordered (NaN): PF=1, CF=1, ZF=1 → all comparisons false.
         * We handle NaN by using SETNP+SETcc+AND for EQ,
         * or just appropriate condition codes for ordered comparisons. */
        X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
        X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
        x64_ucomisd(&ctx->buf, fn, fm);
        x64_xor_rr(&ctx->buf, rd, rd);  /* zero-extend */
        switch (ins->op) {
        case XIR_FEQ:
            /* Equal and not unordered: SETE + SETNP + AND */
            x64_setcc(&ctx->buf, X64_CC_E, rd);
            x64_setcc(&ctx->buf, X64_CC_NP, X64_SCRATCH_REG);
            x64_and_rr(&ctx->buf, rd, X64_SCRATCH_REG);
            break;
        case XIR_FNE:
            /* Not-equal or unordered: SETNE + SETP + OR */
            x64_setcc(&ctx->buf, X64_CC_NE, rd);
            x64_setcc(&ctx->buf, X64_CC_P, X64_SCRATCH_REG);
            x64_or_rr(&ctx->buf, rd, X64_SCRATCH_REG);
            break;
        case XIR_FLT:
            /* a < b (ordered): UCOMISD(a,b) then SETB (CF=1, ZF=0) */
            x64_setcc(&ctx->buf, X64_CC_B, rd);
            break;
        case XIR_FLE:
            /* a <= b (ordered): UCOMISD(a,b) then SETBE (CF=1 or ZF=1) */
            x64_setcc(&ctx->buf, X64_CC_BE, rd);
            break;
        default: break;
        }
        break;
    }

    /* ========== Move ========== */
    case XIR_MOV:
    case XIR_REDEFINE: {
        /* Handle both GP and FP moves */
        if (x64_is_fp_vreg(ctx, ins->dst)) {
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            X64Xmm fn = x64_get_fp_operand(ctx, ins->args[0], X64_SCRATCH_XMM);
            if (fn != fd) x64_movsd_rr(&ctx->buf, fd, fn);
        } else {
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (rn != rd)
                x64_mov_rr(&ctx->buf, rd, rn);
        }
        break;
    }

    /* ========== Select (conditional move) ========== */
    case XIR_SELECT_COND: {
        /* Compare condition to zero, set flags for subsequent SELECT */
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_test_rr(&ctx->buf, rn, rn);
        break;
    }
    case XIR_SELECT: {
        /* CMOVNE dst, true_val  (if NE=nonzero → take true_val) */
        X64Reg true_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        X64Reg false_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        if (false_reg != rd)
            x64_mov_rr(&ctx->buf, rd, false_reg);
        x64_cmov_rr(&ctx->buf, X64_CC_NE, rd, true_reg);
        break;
    }

    /* ========== Memory — basic load/store ========== */
    case XIR_LOAD: {
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
    case XIR_STORE: {
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        bool store_fp = false;
        if (xir_ref_is_vreg(ins->args[1])) {
            uint32_t vi = XIR_REF_INDEX(ins->args[1]);
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
    case XIR_LOAD8Z: {
        X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_movzx_rm8(&ctx->buf, rd, addr, 0);
        break;
    }
    case XIR_LOAD8S: {
        X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_movsx_rm8(&ctx->buf, rd, addr, 0);
        break;
    }
    case XIR_STORE8: {
        X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        X64Reg val = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        if (val == addr) {
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, val);
            val = X64_SCRATCH_REG;
        }
        x64_mov_mr8(&ctx->buf, addr, 0, val);
        break;
    }
    case XIR_LOAD16Z: {
        X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_movzx_rm16(&ctx->buf, rd, addr, 0);
        break;
    }
    case XIR_LOAD16S: {
        X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_movsx_rm16(&ctx->buf, rd, addr, 0);
        break;
    }
    case XIR_STORE16: {
        X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        X64Reg val = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
        if (val == addr) {
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, val);
            val = X64_SCRATCH_REG;
        }
        x64_mov_mr16(&ctx->buf, addr, 0, val);
        break;
    }
    case XIR_LOAD32Z: {
        X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_mov_rm32(&ctx->buf, rd, addr, 0);
        break;
    }
    case XIR_LOAD32S: {
        X64Reg base = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        int32_t offset = 0;
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            offset = (int32_t)ctx->func->consts[ci].val.i64;
        }
        x64_movsxd_rm(&ctx->buf, rd, base, offset);
        break;
    }
    case XIR_STORE32: {
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
    case XIR_LOAD_F32: {
        /* Load 32-bit float, promote to f64.
         * x86-64: CVTSS2SD xmm, [addr] — but we don't have that encoded.
         * Use: MOV r32d, [addr] → MOVD xmm, r32 → CVTSS2SD xmm, xmm.
         * Simpler: just do MOVSS + CVTSS2SD via raw encoding. */
        X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
        X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        /* MOVSS xmm_scratch, [addr]:  F3 0F 10 /r */
        x64_emit8(&ctx->buf, 0xF3);
        if (X64_SCRATCH_XMM > 7 || addr > 7)
            x64_emit8(&ctx->buf, 0x40 | ((X64_SCRATCH_XMM > 7) ? 4 : 0) |
                                         ((addr > 7) ? 1 : 0));
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, 0x10);
        x64_modrm_mem(&ctx->buf, X64_SCRATCH_XMM & 7, addr, 0);
        /* CVTSS2SD fd, xmm_scratch:  F3 0F 5A /r */
        x64_emit8(&ctx->buf, 0xF3);
        if ((int)fd > 7 || X64_SCRATCH_XMM > 7)
            x64_emit8(&ctx->buf, 0x40 | (((int)fd > 7) ? 4 : 0) |
                                         ((X64_SCRATCH_XMM > 7) ? 1 : 0));
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, 0x5A);
        x64_emit8(&ctx->buf, 0xC0 | (((int)fd & 7) << 3) | (X64_SCRATCH_XMM & 7));
        break;
    }
    case XIR_STORE_F32: {
        /* Truncate f64 to f32, store 32-bit float.
         * CVTSD2SS xmm_scratch, src → MOVSS [addr], xmm_scratch */
        X64Reg addr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
        /* CVTSD2SS xmm_scratch, fm:  F2 0F 5A /r */
        x64_emit8(&ctx->buf, 0xF2);
        if (X64_SCRATCH_XMM > 7 || (int)fm > 7)
            x64_emit8(&ctx->buf, 0x40 | ((X64_SCRATCH_XMM > 7) ? 4 : 0) |
                                         (((int)fm > 7) ? 1 : 0));
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, 0x5A);
        x64_emit8(&ctx->buf, 0xC0 | ((X64_SCRATCH_XMM & 7) << 3) | ((int)fm & 7));
        /* MOVSS [addr], xmm_scratch:  F3 0F 11 /r */
        x64_emit8(&ctx->buf, 0xF3);
        if (X64_SCRATCH_XMM > 7 || addr > 7)
            x64_emit8(&ctx->buf, 0x40 | ((X64_SCRATCH_XMM > 7) ? 4 : 0) |
                                         ((addr > 7) ? 1 : 0));
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, 0x11);
        x64_modrm_mem(&ctx->buf, X64_SCRATCH_XMM & 7, addr, 0);
        break;
    }

    /* ========== Coro/context loads/stores ========== */
    case XIR_LOAD_CORO: {
        int32_t offset = 0;
        if (!xir_ref_is_none(ins->args[0]) && xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            offset = (int32_t)ctx->func->consts[ci].val.i64;
        }
        x64_mov_rm(&ctx->buf, rd, X64_JIT_CTX_REG, offset);
        break;
    }
    case XIR_LOAD_CORO_BYTE: {
        int32_t offset = 0;
        if (!xir_ref_is_none(ins->args[0]) && xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            offset = (int32_t)ctx->func->consts[ci].val.i64;
        }
        x64_movzx_rm8(&ctx->buf, rd, X64_JIT_CTX_REG, offset);
        break;
    }
    case XIR_STORE_CORO: {
        int32_t offset = 0;
        if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
            uint32_t ci = XIR_REF_INDEX(ins->dst);
            offset = (int32_t)ctx->func->consts[ci].val.i64;
        }
        bool val_fp = false;
        if (xir_ref_is_vreg(ins->args[0])) {
            uint32_t vi = XIR_REF_INDEX(ins->args[0]);
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
    case XIR_STORE_CORO_BYTE: {
        int32_t offset = 0;
        if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
            uint32_t ci = XIR_REF_INDEX(ins->dst);
            offset = (int32_t)ctx->func->consts[ci].val.i64;
        }
        X64Reg val = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, offset, val);
        break;
    }

    /* ========== Object field load/store ========== */
    case XIR_LOAD_FIELD: {
        X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        int32_t offset = 0;
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            offset = (int32_t)ctx->func->consts[ci].val.i64;
        }
        /* Save runtime tag to jit_ctx scratch if type is unknown */
        if (xir_ref_is_vreg(ins->dst)) {
            uint32_t vi = XIR_REF_INDEX(ins->dst);
            if (vi < ctx->func->nvreg &&
                ins->ctype.kind == XIR_TK_UNKNOWN &&
                ins->rep == XR_REP_I64) {
                x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, obj,
                              offset + XIR_XRVALUE_TAG_OFFSET);
                x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                            (int32_t)XIR_JIT_LOAD_TAG_SCRATCH, X64_SCRATCH_REG);
            }
        }
        /* Load payload at byte 8 */
        if (ins->rep == XR_REP_F64) {
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            x64_movsd_rm(&ctx->buf, fd, obj, offset + XIR_XRVALUE_PAYLOAD_OFFSET);
        } else {
            x64_mov_rm(&ctx->buf, rd, obj, offset + XIR_XRVALUE_PAYLOAD_OFFSET);
        }
        break;
    }
    case XIR_STORE_FIELD: {
        X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        int32_t offset = 0;
        if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
            uint32_t ci = XIR_REF_INDEX(ins->dst);
            offset = (int32_t)ctx->func->consts[ci].val.i64;
        }

        bool is_fp = false;
        if (xir_ref_is_vreg(ins->args[1])) {
            uint32_t vi = XIR_REF_INDEX(ins->args[1]);
            if (vi < ctx->func->nvreg)
                is_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
        }

        /* Store payload at XrValue byte 8 */
        if (is_fp) {
            X64Xmm fm = x64_get_fp_operand(ctx, ins->args[1], X64_SCRATCH_XMM);
            x64_movsd_mr(&ctx->buf, obj, offset + XIR_XRVALUE_PAYLOAD_OFFSET, fm);
        } else {
            X64Reg val = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
            if (val == obj) {
                x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, val);
                val = X64_SCRATCH_REG;
            }
            x64_mov_mr(&ctx->buf, obj, offset + XIR_XRVALUE_PAYLOAD_OFFSET, val);
        }

        /* Store tag (descriptor: tag + flags + heap_type) as 32-bit */
        uint8_t xr_tag = ins->rep;
        bool is_ptr_val = false;
        uint32_t tag_val = 0;

        if (xr_tag == XIR_SF_TAG_RUNTIME) {
            tag_val = XR_TAG_PTR;
            if (xir_ref_is_vreg(ins->args[1])) {
                XirType vct = xir_ref_ctype(ctx->func, ins->args[1]);
                uint8_t vk = type_kind_to_vtag(vct.kind);
                if (vtag_is_concrete(vk)) {
                    tag_val = vtag_to_value_tag(vk);
                    is_ptr_val = xir_type_is_ptr(vct.kind);
                } else {
                    uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                    uint8_t vt = (vi < ctx->func->nvreg)
                                 ? ctx->func->vregs[vi].rep
                                 : XR_REP_TAGGED;
                    if (vt == XR_REP_F64)      tag_val = XR_TAG_F64;
                    else if (vt == XR_REP_I64)  tag_val = XR_TAG_I64;
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
            /* gc_type = *(uint8_t*)(val + XIR_GC_TYPE_OFFSET) */
            x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, val,
                          (int32_t)XIR_GC_TYPE_OFFSET);
            /* scratch = gc_type << 16 */
            x64_shl_ri(&ctx->buf, X64_SCRATCH_REG, 16);
            /* scratch |= tag_val */
            x64_or_ri(&ctx->buf, X64_SCRATCH_REG, (int32_t)tag_val);
            /* Store 32-bit descriptor */
            x64_mov_mr32(&ctx->buf, obj,
                         offset + XIR_XRVALUE_TAG_OFFSET, X64_SCRATCH_REG);
        } else {
            /* Non-PTR: tag with heap_type=0 */
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, tag_val);
            x64_mov_mr32(&ctx->buf, obj,
                         offset + XIR_XRVALUE_TAG_OFFSET, X64_SCRATCH_REG);
        }
        break;
    }

    /* ========== Tag load/check ========== */
    case XIR_TAG_LOAD: {
        X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        int32_t offset = 0;
        if (!xir_ref_is_none(ins->args[1]) && xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            offset = (int32_t)ctx->func->consts[ci].val.i64;
        }
        x64_movzx_rm8(&ctx->buf, rd, ptr, offset);
        break;
    }

    /* ========== BOX/UNBOX ========== */
    case XIR_BOX_I64:
    case XIR_BOX_F64: {
        /* BOX is a no-op inside JIT (values are always raw/untagged) */
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        if (rd != rn) x64_mov_rr(&ctx->buf, rd, rn);
        break;
    }

    case XIR_UNBOX_I64: {
        uint8_t src_type = XR_REP_I64;
        if (xir_ref_is_vreg(ins->args[0])) {
            uint32_t vi = XIR_REF_INDEX(ins->args[0]);
            if (vi < ctx->func->nvreg) src_type = ctx->func->vregs[vi].rep;
        }
        if (src_type == XR_REP_PTR) {
            /* Source is pointer to XrValue — load payload from [ptr+8] */
            X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            x64_mov_rm(&ctx->buf, rd, ptr, XIR_XRVALUE_PAYLOAD_OFFSET);
        } else {
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (rd != rn) x64_mov_rr(&ctx->buf, rd, rn);
        }
        break;
    }

    case XIR_UNBOX_F64: {
        uint8_t src_type = XR_REP_F64;
        if (xir_ref_is_vreg(ins->args[0])) {
            uint32_t vi = XIR_REF_INDEX(ins->args[0]);
            if (vi < ctx->func->nvreg) src_type = ctx->func->vregs[vi].rep;
        }
        if (src_type == XR_REP_PTR) {
            X64Reg ptr = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            x64_movsd_rm(&ctx->buf, fd, ptr, XIR_XRVALUE_PAYLOAD_OFFSET);
        } else {
            X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (rd != rn) x64_mov_rr(&ctx->buf, rd, rn);
        }
        break;
    }

    /* ========== Guard ops ========== */
    case XIR_GUARD_TAG: {
        /* args[0] = tagged value ptr, args[1] = expected tag (const i64) */
        X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        /* Load tag byte: MOVZX r64, byte [val_reg + tag_offset] */
        x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, val_reg,
                       XIR_XRVALUE_TAG_OFFSET);
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            int32_t expected = (int32_t)ctx->func->consts[ci].val.raw;
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
        X64Reg len_reg = x64_get_operand(ctx, ins->args[1],
                          idx_reg == X64_SCRATCH_REG ? X64_RCX : X64_SCRATCH_REG);
        x64_cmp_rr(&ctx->buf, idx_reg, len_reg);
        x64_emit_deopt_id(ctx, ins);
        x64_emit_deopt_jcc(ctx, X64_CC_AE);  /* unsigned >= */
        break;
    }

    case XIR_GUARD_NONNULL: {
        X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_test_rr(&ctx->buf, val_reg, val_reg);
        x64_emit_deopt_id(ctx, ins);
        x64_emit_deopt_jcc(ctx, X64_CC_E);  /* ZF=1 → null → deopt */
        break;
    }

    case XIR_GUARD_CLASS: {
        /* Check shape_id in GC header (uint16 at gc_extra_offset) */
        X64Reg obj = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_movzx_rm16(&ctx->buf, X64_SCRATCH_REG, obj,
                        (int32_t)XIR_GC_EXTRA_OFFSET);
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            int32_t expected = (int32_t)ctx->func->consts[ci].val.raw;
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
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, obj,
                   (int32_t)XIR_INSTANCE_KLASS_OFFSET);
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            uint64_t expected = (uint64_t)ctx->func->consts[ci].val.i64;
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
        x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, obj,
                       (int32_t)XIR_GC_HDR_TYPE_OFFSET);
        x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, 23);
        x64_emit_deopt_jcc(ctx, X64_CC_NE);
        /* Load gc.extra (uint16 at offset 10) */
        x64_movzx_rm16(&ctx->buf, X64_SCRATCH_REG, obj,
                        (int32_t)XIR_GC_HDR_EXTRA_OFFSET);
        /* shape_id = extra >> 2 */
        x64_shr_ri(&ctx->buf, X64_SCRATCH_REG, 2);
        /* Compare with expected shape_id */
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            int32_t expected_id = (int32_t)ctx->func->consts[ci].val.raw;
            x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, expected_id);
        }
        x64_emit_deopt_jcc(ctx, X64_CC_NE);
        break;
    }

    case XIR_DEOPT: {
        /* Unconditional deopt */
        x64_emit_deopt_id(ctx, ins);
        XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
        X64BranchPatch *p = &ctx->patches[ctx->npatch];
        x64_emit8(&ctx->buf, 0xE9);  /* JMP rel32 */
        p->emit_pos = ctx->buf.pos;
        p->target_blk = 0;
        p->type = X64_PATCH_DEOPT_JMP;
        p->cc = X64_CC_E;  /* unused */
        ctx->npatch++;
        x64_emit32(&ctx->buf, 0);
        ctx->has_deopt = true;
        break;
    }

    case XIR_TAG_CHECK: {
        /* Same as GUARD_TAG but may have different dst semantics */
        X64Reg val_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, val_reg,
                       XIR_XRVALUE_TAG_OFFSET);
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            int32_t expected = (int32_t)ctx->func->consts[ci].val.raw;
            x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, expected);
        }
        x64_emit_deopt_id(ctx, ins);
        x64_emit_deopt_jcc(ctx, X64_CC_NE);
        break;
    }

    case XIR_SAFEPOINT: {
        /* TODO: record_safepoint + guard page poll (F.4.6 full) */
        /* Placeholder: load from safepoint_page to trigger fault if armed */
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_SAFEPOINT_PAGE_OFFSET);
        /* Load from the safepoint page: movzx r11d, byte [r11] */
        x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG, 0);
        break;
    }

    /* ========== Mixed-type runtime arithmetic ========== */
    case XIR_RT_ADD: case XIR_RT_SUB: case XIR_RT_MUL:
    case XIR_RT_DIV: case XIR_RT_MOD: {
        uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
        if (xir_ref_is_vreg(ins->args[0])) {
            uint32_t ai = XIR_REF_INDEX(ins->args[0]);
            if (ai < ctx->func->nvreg) ta = ctx->func->vregs[ai].rep;
        }
        if (xir_ref_is_vreg(ins->args[1])) {
            uint32_t bi = XIR_REF_INDEX(ins->args[1]);
            if (bi < ctx->func->nvreg) tb = ctx->func->vregs[bi].rep;
        }

        if ((ta == XR_REP_I64 || ta == XR_REP_F64) &&
            (tb == XR_REP_I64 || tb == XR_REP_F64)) {
            /* Both numeric: convert to FP, operate, result in FP dst */
            X64Xmm fa;
            if (ta == XR_REP_F64) {
                fa = x64_get_fp_operand(ctx, ins->args[0], X64_XMM14);
            } else {
                X64Reg ga = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
                fa = X64_XMM14;  /* scratch FP */
                x64_cvtsi2sd(&ctx->buf, fa, ga);
            }
            X64Xmm fb;
            if (tb == XR_REP_F64) {
                fb = x64_get_fp_operand(ctx, ins->args[1], X64_XMM15);
            } else {
                X64Reg gb = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                fb = X64_XMM15;  /* scratch FP */
                x64_cvtsi2sd(&ctx->buf, fb, gb);
            }
            /* Ensure dst != operand aliasing issues */
            X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
            if (fd != fa) x64_movsd_rr(&ctx->buf, fd, fa);
            switch (ins->op) {
            case XIR_RT_ADD: x64_addsd(&ctx->buf, fd, fb); break;
            case XIR_RT_SUB: x64_subsd(&ctx->buf, fd, fb); break;
            case XIR_RT_MUL: x64_mulsd(&ctx->buf, fd, fb); break;
            case XIR_RT_DIV: x64_divsd(&ctx->buf, fd, fb); break;
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
            default: break;
            }
        } else {
            /* Unknown types: deopt */
            x64_emit_deopt_id(ctx, ins);
            XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
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
        uint8_t ta = XR_REP_I64;
        if (xir_ref_is_vreg(ins->args[0])) {
            uint32_t ai = XIR_REF_INDEX(ins->args[0]);
            if (ai < ctx->func->nvreg) ta = ctx->func->vregs[ai].rep;
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

    case XIR_RT_LT: case XIR_RT_LE: case XIR_RT_EQ: {
        uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
        if (xir_ref_is_vreg(ins->args[0])) {
            uint32_t ai = XIR_REF_INDEX(ins->args[0]);
            if (ai < ctx->func->nvreg) ta = ctx->func->vregs[ai].rep;
        }
        if (xir_ref_is_vreg(ins->args[1])) {
            uint32_t bi = XIR_REF_INDEX(ins->args[1]);
            if (bi < ctx->func->nvreg) tb = ctx->func->vregs[bi].rep;
        }
        if ((ta == XR_REP_I64 || ta == XR_REP_F64) &&
            (tb == XR_REP_I64 || tb == XR_REP_F64)) {
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
            if (ins->op == XIR_RT_LT) cc = X64_CC_B;   /* CF=1 → below */
            else if (ins->op == XIR_RT_LE) cc = X64_CC_BE; /* CF=1 or ZF=1 */
            else cc = X64_CC_E;  /* ZF=1 */
            x64_xor_rr(&ctx->buf, rd, rd);
            /* SETcc r8: 0F 9x /0 — set low byte of rd */
            x64_emit8(&ctx->buf, (rd > 7) ? 0x41 : 0x40);  /* REX prefix */
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, (uint8_t)(0x90 | cc));
            x64_emit8(&ctx->buf, (uint8_t)(0xC0 | ((uint8_t)rd & 7)));
        } else {
            x64_emit_deopt_id(ctx, ins);
            XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
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

    case XIR_RT_PRINT:
    case XIR_RT_ARRAY_NEW:
    case XIR_RT_ARRAY_PUSH:
    case XIR_RT_ARRAY_LEN:
    case XIR_RT_MAP_NEW:
    case XIR_RT_INDEX_GET:
    case XIR_RT_INDEX_SET:
        /* These are handled via CALL_C in the builder; shouldn't reach here */
        xr_log_warning("x64-cg", "RT opcode %d should use CALL_C path", ins->op);
        break;

    case XIR_RT_ISNULL: {
        X64Reg val = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        x64_xor_rr(&ctx->buf, rd, rd);
        x64_test_rr(&ctx->buf, val, val);
        /* SETcc: SETE rd (ZF=1 → null → result=1) */
        x64_emit8(&ctx->buf, (rd > 7) ? 0x41 : 0x40);
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, 0x94);  /* SETE */
        x64_emit8(&ctx->buf, (uint8_t)(0xC0 | ((uint8_t)rd & 7)));
        break;
    }

    /* ========== Exception handling ========== */
    /* TRY_BEGIN / TRY_END / THROW are never emitted directly by the builder
     * in JIT mode; builder lowers them to CALL_C (xr_jit_throw) + GOTO/RET.
     * XIR_CATCH is handled above as a direct exception load + clear. */
    case XIR_TRY_BEGIN:
    case XIR_TRY_END:
    case XIR_THROW:
        break;  /* no-op in JIT codegen */

    /* ========== Coroutine suspend ========== */
    case XIR_SUSPEND:
        /* TODO: coroutine await/channel suspend (F.4.9) — requires
         * suspend_state, resume entry tracking, gopark pattern.
         * Until implemented, force deopt for coro-using functions. */
        xr_log_warning("x64-cg", "XIR_SUSPEND not yet implemented on x86-64");
        ctx->had_error = true;
        break;

    case XIR_BARRIER_FWD:
    case XIR_BARRIER_BACK:
        /* TODO: write barrier stubs — skip for now */
        break;

    /* ========== GC allocation ========== */
    /* Slow-path only: always go through xr_jit_alloc C helper.
     * TODO: inline bump-pointer fast path (matches ARM64 ~100 lines of
     * immix cursor+limit check + GC header init + alloc_post bookkeeping). */
    case XIR_ALLOC: {
        uint8_t gc_type = 0;
        uint32_t alloc_size = 0;
        if (xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            int64_t packed_const = ctx->func->consts[ci].val.i64;
            gc_type = (uint8_t)(packed_const & 0xFF);
        }
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            alloc_size = (uint32_t)ctx->func->consts[ci].val.i64;
        }
        alloc_size = (alloc_size + 7) & ~7u;

        x64_emit_ptr_spill_writeback(ctx);
        uint32_t smap_id_a = x64_record_safepoint(ctx);
        x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t)smap_id_a);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_ACTIVE_SMAP_ID_OFFSET, X64_RCX);

        /* packed = type<<32 | size */
        uint64_t packed_arg = ((uint64_t)gc_type << 32) | (uint64_t)alloc_size;
        x64_load_imm64(&ctx->buf, X64_RCX, packed_arg);
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)X64_EXTRA_ARG_OFFSET, X64_RCX);

        /* Load helper + CALL call_c_stub */
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG,
                       (uint64_t)(uintptr_t)xr_jit_alloc);
        XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
        X64BranchPatch *cp_a = &ctx->patches[ctx->npatch];
        x64_emit8(&ctx->buf, 0xE8);
        cp_a->emit_pos = ctx->buf.pos;
        cp_a->target_blk = 0;
        cp_a->type = X64_PATCH_CALL_C;
        cp_a->cc = X64_CC_E;
        ctx->npatch++;
        x64_emit32(&ctx->buf, 0);
        ctx->has_call_c = true;

        /* Move RAX → rd, then deopt if NULL (allocation failure) */
        if (rd != X64_RAX)
            x64_mov_rr(&ctx->buf, rd, X64_RAX);
        x64_test_rr(&ctx->buf, rd, rd);
        x64_emit_deopt_id(ctx, ins);
        x64_emit_deopt_jcc(ctx, X64_CC_E);
        break;
    }

    case XIR_CATCH: {
        /* Load exception from jit_ctx->exception, then clear it */
        x64_mov_rm(&ctx->buf, rd, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_EXCEPTION_OFFSET);
        x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_EXCEPTION_OFFSET, X64_SCRATCH_REG);
        break;
    }

    /* ========== Call ops ========== */
    case XIR_CALL_C: {
        x64_emit_call_args_from_pool(ctx, ins);
        x64_emit_ptr_spill_writeback(ctx);

        /* Record safepoint + store safepoint_id to jit_ctx */
        uint32_t smap_id_cc = x64_record_safepoint(ctx);
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t)smap_id_cc);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_ACTIVE_SMAP_ID_OFFSET, X64_SCRATCH_REG);

        /* Store extra_arg to jit_ctx scratch slot */
        if (!xir_ref_is_none(ins->args[1])) {
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                uint64_t arg_val = (uint64_t)ctx->func->consts[ci].val.raw;
                x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, arg_val);
            } else {
                X64Reg arg_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
                if (arg_reg != X64_SCRATCH_REG)
                    x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, arg_reg);
            }
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                       (int32_t)X64_EXTRA_ARG_OFFSET, X64_SCRATCH_REG);
        } else {
            /* No extra arg — store 0 */
            x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                       (int32_t)X64_EXTRA_ARG_OFFSET, X64_SCRATCH_REG);
        }

        /* Clear deopt_id before call so helper can request deopt */
        x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_DEOPT_ID_OFFSET, X64_SCRATCH_REG);

        /* Load C function pointer into R11 (scratch) */
        if (xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            uint64_t fn_ptr = (uint64_t)ctx->func->consts[ci].val.raw;
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, fn_ptr);
        } else {
            X64Reg fn_reg = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
            if (fn_reg != X64_SCRATCH_REG)
                x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, fn_reg);
        }

        /* CALL call_c_stub (patched later as rel32) */
        XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
        X64BranchPatch *p = &ctx->patches[ctx->npatch];
        x64_emit8(&ctx->buf, 0xE8);  /* CALL rel32 */
        p->emit_pos = ctx->buf.pos;
        p->target_blk = 0;  /* unused; patched to call_c_stub */
        p->type = X64_PATCH_CALL_C;
        p->cc = X64_CC_E;   /* unused */
        ctx->npatch++;
        x64_emit32(&ctx->buf, 0);  /* placeholder rel32 */
        ctx->has_call_c = true;

        /* TODO: check deopt_id after return (F.4.6) */

        /* Move result payload (RAX) to dst register */
        if (xir_ref_is_vreg(ins->dst)) {
            uint32_t dvi = XIR_REF_INDEX(ins->dst);
            bool dst_fp = (dvi < ctx->func->nvreg &&
                          ctx->func->vregs[dvi].rep == XR_REP_F64);
            if (dst_fp) {
                X64Xmm fd = x64_get_fp_reg(ctx, ins->dst);
                x64_movq_xmm_gp(&ctx->buf, fd, X64_RAX);
            } else if (rd != X64_RAX) {
                x64_mov_rr(&ctx->buf, rd, X64_RAX);
            }
        }

        /* Store tag (from jit_ctx->call_result_tag) to slot_runtime_tags */
        if (xir_ref_is_vreg(ins->dst)) {
            uint32_t dvi = XIR_REF_INDEX(ins->dst);
            if (dvi < ctx->func->nvreg) {
                int16_t bc_slot = ctx->func->vregs[dvi].bc_slot;
                if (bc_slot >= 0 && bc_slot < 256) {
                    int32_t tag_off = (int32_t)XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot;
                    x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG,
                                  (int32_t)XIR_JIT_CALL_RESULT_TAG_OFFSET);
                    x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, tag_off,
                                X64_SCRATCH_REG);
                }
            }
        }
        break;
    }

    case XIR_CALL_C_LEAF: {
        x64_emit_call_args_from_pool(ctx, ins);

        /* Resolve extra arg value */
        uint64_t extra_arg_val = 0;
        bool extra_is_const = false;
        X64Reg extra_reg = X64_SCRATCH_REG;
        if (!xir_ref_is_none(ins->args[1])) {
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                extra_arg_val = (uint64_t)ctx->func->consts[ci].val.raw;
                extra_is_const = true;
            } else {
                extra_reg = x64_get_operand(ctx, ins->args[1], X64_SCRATCH_REG);
            }
        }

        /* Load function pointer */
        uint64_t fn_ptr_val = 0;
        if (xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            fn_ptr_val = (uint64_t)ctx->func->consts[ci].val.raw;
        }

        /* Save all caller-saved GP regs (RAX-R10 = first 8 alloc regs).
         * Callee-saved regs (RBX, R12, R13) don't need saving.
         * Save 8 regs = 64 bytes, +8 (return addr already on stack = misaligned)
         * → 8+64 = 72, need 1 push more for alignment → push 9 = 72 total → aligned. */
        int nsave_gp = 8;  /* first 8 alloc regs are caller-saved */
        if (nsave_gp % 2 == 0) {
            /* Push a dummy for 16-byte alignment before inner CALL */
            x64_push_r(&ctx->buf, X64_SCRATCH_REG);
            nsave_gp++;
        }
        for (int i = 0; i < 8 && i < X64_MAX_PHYS_REGS; i++)
            x64_push_r(&ctx->buf, x64_alloc_regs[i]);

        /* Setup System V call: RDI=coro, RSI=extra_arg */
        x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
        if (!xir_ref_is_none(ins->args[1])) {
            if (extra_is_const)
                x64_load_imm64(&ctx->buf, X64_RSI, extra_arg_val);
            else
                x64_mov_rr(&ctx->buf, X64_RSI, extra_reg);
        }

        /* Load func_ptr and CALL directly */
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, fn_ptr_val);
        x64_call_r(&ctx->buf, X64_SCRATCH_REG);

        /* Save result to R11 */
        x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_RAX);

        /* Restore caller-saved GP regs (reverse) */
        for (int i = 7; i >= 0 && i < X64_MAX_PHYS_REGS; i--)
            x64_pop_r(&ctx->buf, x64_alloc_regs[i]);
        if (nsave_gp > 8)
            x64_pop_r(&ctx->buf, X64_SCRATCH_REG);  /* pop dummy */

        /* Move result to dst */
        if (xir_ref_is_vreg(ins->dst)) {
            if (rd != X64_SCRATCH_REG)
                x64_mov_rr(&ctx->buf, rd, X64_SCRATCH_REG);
        }
        break;
    }

    /* ========== Cross-function calls ========== */
    case XIR_CALL_SELF_DIRECT: {
        x64_emit_call_args_from_pool(ctx, ins);
        x64_emit_ptr_spill_writeback(ctx);

        /* Record safepoint + store safepoint_id to jit_ctx */
        uint32_t smap_id_self = x64_record_safepoint(ctx);
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t)smap_id_self);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_ACTIVE_SMAP_ID_OFFSET, X64_SCRATCH_REG);

        bool reg_passing = !xir_ref_is_none(ins->args[0]);

        /* Resolve arg registers before save */
        X64Reg arg_regs[2] = { X64_SCRATCH_REG, X64_SCRATCH_REG };
        int nargs_reg = 0;
        if (reg_passing) {
            X64Reg scratches[2] = { X64_SCRATCH_REG, X64_RCX };
            for (int a = 0; a < 2; a++) {
                if (xir_ref_is_none(ins->args[a])) break;
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
            int extra = XIR_FLAG_EXTRA_ARGS_GET(ins->flags);
            for (int ei = 0; ei < extra; ei++) {
                X64Reg dst = x64_alloc_regs[2 + ei];
                int32_t off = (int32_t)(XIR_JIT_CALL_ARGS_OFFSET + (2 + ei) * 8);
                x64_mov_rm(&ctx->buf, dst, X64_JIT_CTX_REG, off);
            }
            /* RDI = coro for System V ABI */
            x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
            /* CALL to fast_entry (after param loading) */
            XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
            X64BranchPatch *p = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE8);
            p->emit_pos = ctx->buf.pos;
            p->target_blk = 0;
            p->type = X64_PATCH_CALL_SELF_FAST;
            p->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
        } else {
            /* Memory passing: RDI=coro, RSI=&call_args */
            x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
            x64_lea(&ctx->buf, X64_RSI, X64_JIT_CTX_REG,
                       (int32_t)XIR_JIT_CALL_ARGS_OFFSET);
            /* CALL to entry (offset 0) */
            XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
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
                   -(int32_t)X64_JIT_FRAME_BASE);  /* TODO: frame smap ptr slot */
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_FRAME_SP_OFFSET, X64_RBP);

        /* Save return value (RAX) to R11 before deopt check */
        x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_RAX);

        /* Store return tag (RCX = callee tag) to slot_runtime_tags if needed */
        int16_t self_bc_slot = -1;
        if (xir_ref_is_vreg(ins->dst)) {
            uint32_t vi = XIR_REF_INDEX(ins->dst);
            if (vi < ctx->func->nvreg)
                self_bc_slot = ctx->func->vregs[vi].bc_slot;
        }
        if (self_bc_slot >= 0 && self_bc_slot < 256) {
            int32_t stag_off = (int32_t)XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + self_bc_slot;
            x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, stag_off, X64_RCX);
        }

        /* Deopt propagation check: if result == DEOPT_MARKER, propagate */
        x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t)XIR_DEOPT_MARKER);
        x64_cmp_rr(&ctx->buf, X64_SCRATCH_REG, X64_RCX);
        /* JNE skip_deopt */
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t)(0x80 | X64_CC_NE));
        uint32_t jne_pos = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);  /* placeholder */

        /* Deopt path: clean up save frame and jump to deopt_stub */
        if (save_frame > 0)
            x64_add_ri(&ctx->buf, X64_RSP, save_frame);
        /* Store max deopt_id to indicate propagation */
        x64_load_imm64(&ctx->buf, X64_RCX, 0xFFFF);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_DEOPT_ID_OFFSET, X64_RCX);
        /* JMP to deopt_stub */
        XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
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
        if (xir_ref_is_vreg(ins->dst) && rd != X64_SCRATCH_REG)
            x64_mov_rr(&ctx->buf, rd, X64_SCRATCH_REG);
        break;
    }

    /* CALL_KNOWN: cross-function direct CALL with known callee proto.
     * args[0] = const_ptr(callee XrProto*), args[1] = const(nargs). */
    case XIR_CALL_KNOWN: {
        x64_emit_call_args_from_pool(ctx, ins);
        x64_emit_ptr_spill_writeback(ctx);

        /* Record safepoint + store safepoint_id to jit_ctx */
        uint32_t smap_id_k = x64_record_safepoint(ctx);
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t)smap_id_k);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_ACTIVE_SMAP_ID_OFFSET, X64_SCRATCH_REG);

        /* Extract callee proto pointer and nargs from consts */
        uint64_t callee_proto_ptr = 0;
        if (xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            callee_proto_ptr = (uint64_t)ctx->func->consts[ci].val.raw;
        }
        uint64_t nargs_val = 0;
        if (!xir_ref_is_none(ins->args[1]) && xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            nargs_val = (uint64_t)ctx->func->consts[ci].val.i64;
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
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_PROTO_OFFSET, X64_SCRATCH_REG);
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
                   (int32_t)XIR_PROTO_JIT_ENTRY_OFFSET);
        /* TEST r11, r11; JE slow_path (placeholder) */
        x64_test_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t)(0x80 | X64_CC_E));
        uint32_t je_slow_pos = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* Set call_closure from call_args[0] so callee can access upvalues */
        x64_mov_rm(&ctx->buf, X64_RCX, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_ARGS_OFFSET);
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_CLOSURE_OFFSET, X64_RCX);

        /* Fast path direct CALL: RDI=coro, RSI=&call_args[1], CALL r11 */
        x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
        x64_lea(&ctx->buf, X64_RSI, X64_JIT_CTX_REG,
                (int32_t)(XIR_JIT_CALL_ARGS_OFFSET + 8));
        x64_call_r(&ctx->buf, X64_SCRATCH_REG);

        /* Store RCX (callee tag from epilogue) to jit_ctx->call_result_tag */
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_RESULT_TAG_OFFSET, X64_RCX);

        /* Nested deopt guard: if RAX == DEOPT_MARKER, cascade to slow path */
        x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t)XIR_DEOPT_MARKER);
        x64_cmp_rr(&ctx->buf, X64_RAX, X64_RCX);
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t)(0x80 | X64_CC_E));
        uint32_t je_cascade_pos = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* JMP done (over slow path) */
        x64_emit8(&ctx->buf, 0xE9);
        uint32_t jmp_done_pos = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* Cascade target: clear stale deopt_id before C bridge retry */
        uint32_t cascade_pos = ctx->buf.pos;
        x64_xor_rr(&ctx->buf, X64_RCX, X64_RCX);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_DEOPT_ID_OFFSET, X64_RCX);

        /* Slow path: CALL_C to xr_jit_call_func(coro, nargs) */
        uint32_t slow_pos = ctx->buf.pos;
        /* Patch je_slow → slow_pos */
        x64_patch_rel32(&ctx->buf, je_slow_pos, slow_pos);
        /* Patch je_cascade → cascade_pos */
        x64_patch_rel32(&ctx->buf, je_cascade_pos, cascade_pos);

        /* Store extra_arg (nargs) via RCX; call_c_stub expects fn ptr in R11 */
        x64_load_imm64(&ctx->buf, X64_RCX, nargs_val);
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)X64_EXTRA_ARG_OFFSET, X64_RCX);
        /* Load helper function pointer to R11 (call_c_stub calls R11) */
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG,
                       (uint64_t)(uintptr_t)xr_jit_call_func);
        /* CALL rel32 → call_c_stub */
        XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
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
        if (xir_ref_is_vreg(ins->dst)) {
            uint32_t vi = XIR_REF_INDEX(ins->dst);
            if (vi < ctx->func->nvreg)
                bc_slot_k = ctx->func->vregs[vi].bc_slot;
        }
        if (bc_slot_k >= 0 && bc_slot_k < 256) {
            x64_movzx_rm8(&ctx->buf, X64_RCX, X64_JIT_CTX_REG,
                          (int32_t)XIR_JIT_CALL_RESULT_TAG_OFFSET);
            int32_t tag_off = (int32_t)XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot_k;
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
        if (xir_ref_is_vreg(ins->dst) && rd != X64_SCRATCH_REG)
            x64_mov_rr(&ctx->buf, rd, X64_SCRATCH_REG);
        break;
    }

    /* CALL_KNOWN_REG: register-passing variant of CALL_KNOWN (nargs <= 2).
     * args[0] = param0 XIR ref, args[1] = param1 XIR ref (or NONE).
     * Builder pre-stored callee proto in jit_ctx->call_proto. */
    case XIR_CALL_KNOWN_REG: {
        x64_emit_call_args_from_pool(ctx, ins);
        x64_emit_ptr_spill_writeback(ctx);

        /* Record safepoint */
        uint32_t smap_id_kr = x64_record_safepoint(ctx);
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t)smap_id_kr);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_ACTIVE_SMAP_ID_OFFSET, X64_SCRATCH_REG);

        /* Resolve arg vreg → physical reg mapping (before spill save) */
        int nargs_reg = 0;
        X64Reg arg_regs[2] = { X64_SCRATCH_REG, X64_SCRATCH_REG };
        {
            X64Reg scratches[2] = { X64_SCRATCH_REG, X64_RCX };
            for (int a = 0; a < 2; a++) {
                if (xir_ref_is_none(ins->args[a])) break;
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
                   (int32_t)XIR_JIT_CALL_PROTO_OFFSET);
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
                   (int32_t)XIR_PROTO_JIT_FAST_ENTRY_OFFSET);
        /* TEST R11, R11; JE slow_path */
        x64_test_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t)(0x80 | X64_CC_E));
        uint32_t je_slow_kr = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* Set call_closure from call_args[0] */
        x64_mov_rm(&ctx->buf, X64_RCX, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_ARGS_OFFSET);
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_CLOSURE_OFFSET, X64_RCX);

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
        /* RDI = coro (System V ABI) */
        x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
        /* CALL R11 (jit_fast_entry) */
        x64_call_r(&ctx->buf, X64_SCRATCH_REG);

        /* Store callee tag (RCX) to call_result_tag */
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_RESULT_TAG_OFFSET, X64_RCX);

        /* Nested deopt check */
        x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t)XIR_DEOPT_MARKER);
        x64_cmp_rr(&ctx->buf, X64_RAX, X64_RCX);
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t)(0x80 | X64_CC_E));
        uint32_t je_cascade_kr = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* JMP done */
        x64_emit8(&ctx->buf, 0xE9);
        uint32_t jmp_done_kr = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* Cascade: clear stale deopt_id */
        uint32_t cascade_kr_pos = ctx->buf.pos;
        x64_xor_rr(&ctx->buf, X64_RCX, X64_RCX);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_DEOPT_ID_OFFSET, X64_RCX);

        /* Slow path */
        uint32_t slow_kr_pos = ctx->buf.pos;
        x64_patch_rel32(&ctx->buf, je_slow_kr, slow_kr_pos);
        x64_patch_rel32(&ctx->buf, je_cascade_kr, cascade_kr_pos);

        /* Store nargs via RCX; call_c_stub calls R11 as func ptr */
        x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t)nargs_reg);
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)X64_EXTRA_ARG_OFFSET, X64_RCX);
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG,
                       (uint64_t)(uintptr_t)xr_jit_call_func);
        XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
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

        if (xir_ref_is_vreg(ins->dst) && rd != X64_SCRATCH_REG)
            x64_mov_rr(&ctx->buf, rd, X64_SCRATCH_REG);
        break;
    }

    /* CALL_DIRECT: cross-function JIT→JIT call with inline closure dispatch.
     * args[0] = nargs (vreg/const), args[1] = const_ptr(xr_jit_call_func)
     * Fast path: verify closure→proto→jit_entry chain inline, CALL via stub.
     * Slow path: fall through to xr_jit_call_func C bridge. */
    case XIR_CALL_DIRECT: {
        x64_emit_call_args_from_pool(ctx, ins);
        x64_emit_ptr_spill_writeback(ctx);

        uint32_t smap_id_d = x64_record_safepoint(ctx);
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t)smap_id_d);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_ACTIVE_SMAP_ID_OFFSET, X64_SCRATCH_REG);

        /* Fast path: load closure from call_args[0] into R11 */
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_ARGS_OFFSET);

        /* GC type guard: only XR_TFUNCTION (5) uses fast path.
         * Classes have different layout and need C bridge. */
        x64_movzx_rm8(&ctx->buf, X64_RCX, X64_SCRATCH_REG,
                      (int32_t)XIR_GC_TYPE_OFFSET);
        x64_cmp_ri(&ctx->buf, X64_RCX, 5);
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t)(0x80 | X64_CC_NE));
        uint32_t jne_type_d = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* Set call_closure for callee's upvalue access */
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_CLOSURE_OFFSET, X64_SCRATCH_REG);

        /* R11 = closure->proto */
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
                   (int32_t)XIR_CLOSURE_PROTO_OFFSET);
        /* Null proto guard */
        x64_test_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t)(0x80 | X64_CC_E));
        uint32_t je_proto_d = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* Save proto ptr to jit_ctx->call_proto */
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)XIR_JIT_CALL_PROTO_OFFSET, X64_SCRATCH_REG);

        /* R11 = proto->jit_entry */
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG,
                   (int32_t)XIR_PROTO_JIT_ENTRY_OFFSET);
        /* Null jit_entry guard */
        x64_test_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t)(0x80 | X64_CC_E));
        uint32_t je_entry_d = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* Fast path: CALL via call_c_stub with R11=jit_entry,
         * extra_arg = &call_args[1] */
        x64_lea(&ctx->buf, X64_RCX, X64_JIT_CTX_REG,
                (int32_t)(XIR_JIT_CALL_ARGS_OFFSET + 8));
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)X64_EXTRA_ARG_OFFSET, X64_RCX);
        XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
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
        x64_load_imm64(&ctx->buf, X64_RCX, (uint64_t)XIR_DEOPT_MARKER);
        x64_cmp_rr(&ctx->buf, X64_RAX, X64_RCX);
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t)(0x80 | X64_CC_E));
        uint32_t je_cascade_d = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* JMP done */
        x64_emit8(&ctx->buf, 0xE9);
        uint32_t jmp_done_d = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);

        /* Cascade: clear stale deopt_id */
        uint32_t cascade_d_pos = ctx->buf.pos;
        x64_xor_rr(&ctx->buf, X64_RCX, X64_RCX);
        x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                     (int32_t)XIR_JIT_DEOPT_ID_OFFSET, X64_RCX);

        /* Slow path: xr_jit_call_func(coro, nargs) via call_c_stub */
        uint32_t slow_d_pos = ctx->buf.pos;
        x64_patch_rel32(&ctx->buf, jne_type_d, slow_d_pos);
        x64_patch_rel32(&ctx->buf, je_proto_d, slow_d_pos);
        x64_patch_rel32(&ctx->buf, je_entry_d, slow_d_pos);
        x64_patch_rel32(&ctx->buf, je_cascade_d, cascade_d_pos);

        /* Extract nargs for the helper call */
        if (xir_ref_is_const(ins->args[0])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
            uint64_t nargs_const = (uint64_t)ctx->func->consts[ci].val.raw;
            x64_load_imm64(&ctx->buf, X64_RCX, nargs_const);
        } else {
            X64Reg nreg = x64_get_operand(ctx, ins->args[0], X64_RCX);
            if (nreg != X64_RCX) x64_mov_rr(&ctx->buf, X64_RCX, nreg);
        }
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)X64_EXTRA_ARG_OFFSET, X64_RCX);

        /* Load xr_jit_call_func into R11 (or from args[1] const) */
        if (xir_ref_is_const(ins->args[1])) {
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            uint64_t fn = (uint64_t)ctx->func->consts[ci].val.raw;
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, fn);
        } else {
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG,
                           (uint64_t)(uintptr_t)xr_jit_call_func);
        }
        XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
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
        if (xir_ref_is_vreg(ins->dst)) {
            uint32_t vi = XIR_REF_INDEX(ins->dst);
            if (vi < ctx->func->nvreg)
                bc_slot_d = ctx->func->vregs[vi].bc_slot;
        }
        if (bc_slot_d >= 0 && bc_slot_d < 256) {
            x64_movzx_rm8(&ctx->buf, X64_RCX, X64_JIT_CTX_REG,
                          (int32_t)XIR_JIT_CALL_RESULT_TAG_OFFSET);
            int32_t tag_off = (int32_t)XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot_d;
            x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, tag_off, X64_RCX);
        }

        /* Move result (RAX) → dst */
        if (xir_ref_is_vreg(ins->dst) && rd != X64_RAX)
            x64_mov_rr(&ctx->buf, rd, X64_RAX);
        break;
    }

    case XIR_CALL:
        /* CALL (generic) falls through to xr_jit_call_func via CALL_C */
        xr_log_warning("x64-cg", "generic XIR_CALL op not yet implemented");
        ctx->had_error = true;
        break;

    case XIR_RET: {
        /* Return value is in the register assigned to args[0].
         * JIT calling convention: return value in RAX. */
        X64Reg rn = x64_get_operand(ctx, ins->args[0], X64_SCRATCH_REG);
        if (rn != X64_RAX)
            x64_mov_rr(&ctx->buf, X64_RAX, rn);
        /* Epilogue will be emitted as part of block terminator */
        break;
    }

    case XIR_NOP:
    case XIR_PHI:
        break;

    default:
        /* Unsupported opcode — flag error, codegen will report failure */
        xr_log_warning("x64-cg", "unsupported XIR opcode %d", ins->op);
        ctx->had_error = true;
        break;
    }
}

/* ========== Call Helpers ========== */

/* Derive XR_TAG_* from const rep for call_arg_tags[] */
static inline uint8_t const_rep_to_value_tag(uint8_t rep) {
    switch (rep) {
    case XR_REP_I64: return 3;  /* XR_TAG_I64 */
    case XR_REP_F64: return 4;  /* XR_TAG_F64 */
    case XR_REP_PTR: return 5;  /* XR_TAG_PTR */
    default:         return 0xFF; /* XR_RTAG_UNKNOWN */
    }
}

/* Store call arguments from the pool to jit_ctx->call_args[] and
 * compile-time type tags to jit_ctx->call_arg_tags[]. */
static void x64_emit_call_args_from_pool(X64CodegenCtx *ctx, XirIns *ins) {
    XR_DCHECK(ctx != NULL && ins != NULL, "call_args: NULL");
    if (!xir_ref_is_vreg(ins->dst)) return;
    uint32_t vi = XIR_REF_INDEX(ins->dst);
    if (vi >= ctx->func->nvreg) return;
    XirVReg *vreg = &ctx->func->vregs[vi];
    if (vreg->call_nargs == 0) return;
    XirRef *pool = ctx->func->call_arg_pool;
    uint32_t start = vreg->call_arg_start;

    uint64_t tag_pack[2] = {0, 0};

    for (uint16_t i = 0; i < vreg->call_nargs; i++) {
        XirRef arg = pool[start + i];
        int32_t off = (int32_t)(XIR_JIT_CALL_ARGS_OFFSET + i * 8);
        uint8_t tag = XR_RTAG_UNKNOWN;
        if (xir_ref_is_none(arg)) goto store_tag;
        if (xir_ref_is_const(arg)) {
            uint32_t ci = XIR_REF_INDEX(arg);
            uint64_t val = (uint64_t)ctx->func->consts[ci].val.raw;
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, val);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, off, X64_SCRATCH_REG);
            tag = const_rep_to_value_tag(ctx->func->consts[ci].rep);
        } else {
            X64Reg reg = x64_get_operand(ctx, arg, X64_SCRATCH_REG);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, off, reg);
            XirType ct = xir_ref_ctype(ctx->func, arg);
            tag = vtag_to_value_tag(type_kind_to_vtag(ct.kind));
        }
    store_tag:
        if (i < 8)  tag_pack[0] |= ((uint64_t)tag << (i * 8));
        else         tag_pack[1] |= ((uint64_t)tag << ((i - 8) * 8));
    }

    /* Store packed tags as compile-time constants */
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, tag_pack[0]);
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
               (int32_t)XIR_JIT_CALL_ARG_TAGS_OFFSET, X64_SCRATCH_REG);
    if (vreg->call_nargs > 8) {
        x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, tag_pack[1]);
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   (int32_t)(XIR_JIT_CALL_ARG_TAGS_OFFSET + 8), X64_SCRATCH_REG);
    }

    /* Dynamic tag patch: overwrite unknown tags from slot_runtime_tags */
    for (uint16_t i = 0; i < vreg->call_nargs; i++) {
        uint8_t ct = (i < 8)
            ? (uint8_t)((tag_pack[0] >> (i * 8)) & 0xFF)
            : (uint8_t)((tag_pack[1] >> ((i - 8) * 8)) & 0xFF);
        if (ct != XR_RTAG_UNKNOWN) continue;
        XirRef arg = pool[start + i];
        if (!xir_ref_is_vreg(arg)) continue;
        uint32_t ai = XIR_REF_INDEX(arg);
        if (ai >= ctx->func->nvreg) continue;
        int16_t bc_slot = ctx->func->vregs[ai].bc_slot;
        if (bc_slot < 0 || bc_slot >= 256) continue;
        int32_t src_off = (int32_t)XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot;
        int32_t dst_off = (int32_t)XIR_JIT_CALL_ARG_TAGS_OFFSET + (int32_t)i;
        x64_movzx_rm8(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG, src_off);
        x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, dst_off, X64_SCRATCH_REG);
    }
}

/* Emit call_c_stub: shared trampoline that saves/restores all registers
 * around a C function call.
 *
 * Protocol:
 *   R11 = C function pointer
 *   [R14 + X64_EXTRA_ARG_OFFSET] = extra argument (pre-stored by codegen)
 *
 * Stub saves all 11 allocatable GP + 15 FP regs, calls the C function
 * with System V ABI (RDI=coro, RSI=extra_arg), saves result payload/tag,
 * restores all regs, returns payload in RAX. */
static void x64_emit_call_c_stub(X64CodegenCtx *ctx) {
    if (!ctx->has_call_c) return;
    ctx->call_c_stub = ctx->buf.pos;

    /* Save all 11 allocatable GP regs (88 bytes).
     * After CALL into stub: rsp is misaligned (-8 from return addr).
     * 11 pushes = 88 bytes → -96 total → 16-byte aligned. */
    for (int i = 0; i < X64_MAX_PHYS_REGS; i++)
        x64_push_r(&ctx->buf, x64_alloc_regs[i]);

    /* Save 15 FP regs (xmm0-xmm14, skip xmm15=scratch).
     * sub rsp, 128 (15*8=120 + 8 pad for alignment).
     * Stack: -96 - 128 = -224 → aligned. */
    x64_sub_ri(&ctx->buf, X64_RSP, 128);
    for (int i = 0; i < 15; i++)
        x64_movsd_mr(&ctx->buf, X64_RSP, i * 8, (X64Xmm)i);

    /* Save SP to jit_ctx for GC stack map access */
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
               (int32_t)XIR_JIT_SAFEPOINT_SAVED_SP_OFFSET, X64_RSP);

    /* Setup System V call: RDI=coro, RSI=extra_arg, func_ptr in R11 */
    x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
    x64_mov_rm(&ctx->buf, X64_RSI, X64_JIT_CTX_REG,
               (int32_t)X64_EXTRA_ARG_OFFSET);
    x64_call_r(&ctx->buf, X64_SCRATCH_REG);

    /* XrJitResult returned in RAX(payload), RDX(tag) on System V x86-64.
     * Save payload to R11 (scratch), store tag to jit_ctx. */
    x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_RAX);
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
               (int32_t)XIR_JIT_CALL_RESULT_TAG_OFFSET, X64_RDX);

    /* Restore 15 FP regs */
    for (int i = 0; i < 15; i++)
        x64_movsd_rm(&ctx->buf, (X64Xmm)i, X64_RSP, i * 8);
    x64_add_ri(&ctx->buf, X64_RSP, 128);

    /* Restore 11 GP regs (reverse order) */
    for (int i = X64_MAX_PHYS_REGS - 1; i >= 0; i--)
        x64_pop_r(&ctx->buf, x64_alloc_regs[i]);

    /* Return payload in RAX */
    x64_mov_rr(&ctx->buf, X64_RAX, X64_SCRATCH_REG);
    x64_ret(&ctx->buf);
}

/* ========== Deopt Stub ========== */

/* Emit deopt exit stub: saves all register state to jit_ctx->deopt_regs,
 * loads DEOPT_MARKER into RAX, then returns via epilogue.
 *
 * On entry to stub, [R14 + deopt_id_offset] already stores the deopt_id
 * (written by codegen before the Jcc/JMP to this stub). */
static void x64_emit_deopt_stub(X64CodegenCtx *ctx);

/* Forward declaration for epilogue (used by deopt stub) */
static void x64_emit_epilogue(X64CodegenCtx *ctx);

static void x64_emit_deopt_stub(X64CodegenCtx *ctx) {
    if (!ctx->has_deopt) return;
    ctx->deopt_stub = ctx->buf.pos;

    /* Save frame pointer for spill slot recovery */
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
               (int32_t)XIR_JIT_DEOPT_SPILL_BASE_OFFSET, X64_RBP);

    /* Save all allocatable GP registers to jit_ctx->deopt_regs[reg_num].
     * Index by hardware register number so the deopt reconstructor
     * can map phys_reg → deopt_regs[phys_reg] directly. */
    int32_t gp_base = (int32_t)XIR_JIT_DEOPT_REGS_OFFSET;
    for (int i = 0; i < X64_MAX_PHYS_REGS; i++) {
        X64Reg r = x64_alloc_regs[i];
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG,
                   gp_base + (int32_t)r * 8, r);
    }

    /* Save FP registers xmm0-xmm14 to jit_ctx->deopt_fp_regs[d_num] */
    int32_t fp_base = (int32_t)XIR_JIT_DEOPT_FP_REGS_OFFSET;
    for (int i = 0; i < 15; i++) {
        x64_movsd_mr(&ctx->buf, X64_JIT_CTX_REG,
                     fp_base + i * 8, (X64Xmm)i);
    }

    /* Load DEOPT_MARKER into RAX as return value */
    x64_load_imm64(&ctx->buf, X64_RAX, (uint64_t)XIR_DEOPT_MARKER);

    /* Epilogue + RET */
    x64_emit_epilogue(ctx);
    x64_ret(&ctx->buf);
}

/* Helper: emit a deopt branch patch (Jcc rel32 → deopt_stub).
 * Before calling this, codegen has already stored deopt_id into
 * [R14 + XIR_JIT_DEOPT_ID_OFFSET]. */
static void x64_emit_deopt_jcc(X64CodegenCtx *ctx, X64Cond cc) {
    XR_DCHECK(ctx->npatch < ctx->patches_cap, "too many patches");
    X64BranchPatch *p = &ctx->patches[ctx->npatch];
    x64_emit8(&ctx->buf, 0x0F);
    x64_emit8(&ctx->buf, (uint8_t)(0x80 | cc));  /* Jcc rel32 */
    p->emit_pos = ctx->buf.pos;
    p->target_blk = 0;
    p->type = X64_PATCH_DEOPT_JCC;
    p->cc = cc;
    ctx->npatch++;
    x64_emit32(&ctx->buf, 0);  /* placeholder rel32 */
    ctx->has_deopt = true;
}

/* Helper: store deopt_id (from ins->dst const) to jit_ctx->deopt_id */
static void x64_emit_deopt_id(X64CodegenCtx *ctx, XirIns *ins) {
    uint32_t did = 0xFFFF;
    if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
        uint32_t dci = XIR_REF_INDEX(ins->dst);
        did = (uint32_t)ctx->func->consts[dci].val.raw;
    }
    /* Store deopt_id as 32-bit value to jit_ctx */
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t)did);
    x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG,
                 (int32_t)XIR_JIT_DEOPT_ID_OFFSET, X64_SCRATCH_REG);
}

/* ========== GC Stack Map + Spill Writeback ========== */

/* Record safepoint bitmap: which alloc_regs[] hold PTR vregs (reg_bitmap),
 * and which spill slots hold PTR vregs (spill_bitmap) at cur_ra_pos. */
static uint32_t x64_record_safepoint(X64CodegenCtx *ctx) {
    if (ctx->nsmap >= XIR_MAX_STACK_MAP_ENTRIES) {
        xr_log_warning("x64-cg", "stack map table full (%u entries)", ctx->nsmap);
        return ctx->nsmap > 0 ? ctx->nsmap - 1 : 0;
    }

    int32_t pos = ctx->cur_ra_pos;
    uint32_t reg_bitmap = 0;
    uint32_t spill_bitmap = 0;

    for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
        if (ctx->func->vregs[v].rep != XR_REP_PTR) continue;

        int8_t ri = xra_reg_at_pos(ctx->xra, v, pos);
        if (ri < 0) ri = xra_reg_at_pos(ctx->xra, v, pos + 1);
        if (ri >= 0 && ri < X64_MAX_PHYS_REGS) {
            reg_bitmap |= (1u << ri);
            if (ctx->xra && v < ctx->xra->nvreg && ctx->xra->valloc[v].spill >= 0) {
                int16_t slot = ctx->xra->valloc[v].spill;
                if (slot >= 0 && slot < XIR_MAX_SPILL_SLOTS)
                    spill_bitmap |= (1u << slot);
            }
            continue;
        }

        if (ctx->xra && v < ctx->xra->nvreg && ctx->xra->valloc[v].spill >= 0
            && xra_vreg_live_at(ctx->xra, v, pos)) {
            int16_t slot = ctx->xra->valloc[v].spill;
            if (slot >= 0 && slot < XIR_MAX_SPILL_SLOTS)
                spill_bitmap |= (1u << slot);
        }
    }

    uint32_t sid = ctx->nsmap;
    ctx->smap_entries[sid].pc_offset = ctx->buf.pos;
    ctx->smap_entries[sid].reg_bitmap = reg_bitmap;
    ctx->smap_entries[sid].spill_bitmap = spill_bitmap;
    ctx->nsmap++;
    return sid;
}

/* Write back all live PTR register values to their spill slots.
 * Called before cross-function calls so GC can find PTR values in outer
 * frames by scanning spill slots. */
static void x64_emit_ptr_spill_writeback(X64CodegenCtx *ctx) {
    XR_DCHECK(ctx != NULL, "ptr_spill_writeback: NULL ctx");
    int32_t pos = ctx->cur_ra_pos;
    for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
        if (ctx->func->vregs[v].rep != XR_REP_PTR) continue;
        int8_t ri = xra_reg_at_pos(ctx->xra, v, pos);
        if (ri < 0) ri = xra_reg_at_pos(ctx->xra, v, pos + 1);
        if (ri < 0) continue;

        int16_t slot = ctx->xra->valloc[v].spill;
        if (slot < 0) {
            slot = (int16_t)ctx->xra->nspill++;
            ctx->xra->valloc[v].spill = slot;
        }
        if (slot >= 0 && slot < 32 && ri >= 0 && ri < X64_MAX_PHYS_REGS) {
            X64Reg reg = x64_alloc_regs[ri];
            int32_t offset = -(X64_SPILL_BASE + slot * 8);
            x64_mov_mr(&ctx->buf, X64_RBP, offset, reg);
        }
    }
}

/* Collect live caller-saved GP regs (alloc indices 0..7) in current block. */
static int x64_live_gp(X64CodegenCtx *ctx, X64Reg *out, X64Reg exclude) {
    uint32_t bid = ctx->cur_blk_id;
    uint32_t mask = (ctx->xra && bid < ctx->xra->nblk)
                  ? ctx->xra->blk_gp_live[bid] : 0;
    int n = 0;
    /* Caller-saved: alloc indices 0..7 (RAX,RCX,RDX,RSI,RDI,R8,R9,R10) */
    for (int r = 0; r < 8; r++) {
        if ((mask & (1u << r)) && x64_alloc_regs[r] != exclude)
            out[n++] = x64_alloc_regs[r];
    }
    return n;
}

/* Collect live caller-saved FP regs in current block. */
static int x64_live_fp(X64CodegenCtx *ctx, X64Xmm *out) {
    uint32_t bid = ctx->cur_blk_id;
    uint32_t mask = (ctx->xra && bid < ctx->xra->nblk)
                  ? ctx->xra->blk_fp_live[bid] : 0;
    int n = 0;
    /* All xmm0-xmm13 are caller-saved in System V */
    for (int r = 0; r < X64_MAX_FP_REGS; r++) {
        if (mask & (1u << r))
            out[n++] = x64_alloc_fp_regs[r];
    }
    return n;
}

/* ========== Prologue / Epilogue ========== */

static void x64_emit_prologue(X64CodegenCtx *ctx) {
    /* PUSH RBP; MOV RBP, RSP */
    x64_push_r(&ctx->buf, X64_RBP);
    x64_mov_rr(&ctx->buf, X64_RBP, X64_RSP);

    /* SUB RSP, frame_size (placeholder imm32 — patched later).
     * Always emit the imm32 form (REX.W 81 EC imm32) so the
     * displacement can be patched to any value up to 2 GiB. */
    XR_DCHECK(ctx->nsub_patches < 8, "too many frame sub patches");
    x64_emit8(&ctx->buf, 0x48);  /* REX.W */
    x64_emit8(&ctx->buf, 0x81);  /* SUB r/m64, imm32 */
    x64_emit8(&ctx->buf, 0xEC);  /* ModRM: mod=11, /5, rm=RSP */
    ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.pos;
    x64_emit32(&ctx->buf, X64_JIT_FRAME_BASE);  /* placeholder */

    /* Save callee-saved registers: rbx, r12, r13, r14, r15 */
    x64_push_r(&ctx->buf, X64_RBX);
    x64_push_r(&ctx->buf, X64_R12);
    x64_push_r(&ctx->buf, X64_R13);
    x64_push_r(&ctx->buf, X64_R14);
    x64_push_r(&ctx->buf, X64_R15);

    /* MOV R15, RDI  (save coro pointer — first arg in System V ABI) */
    x64_mov_rr(&ctx->buf, X64_CORO_REG, X64_RDI);

    /* LDR R14, [R15 + jit_ctx_offset]  (load per-Worker JIT scratch pointer) */
    x64_mov_rm(&ctx->buf, X64_JIT_CTX_REG, X64_CORO_REG,
               (int32_t)XIR_CORO_JIT_CTX_OFFSET);

    /* Load params from args array (RSI = args pointer, System V 2nd arg) */
    uint32_t nparams = ctx->func->num_params;
    if (nparams > 0 && ctx->xra) {
        for (uint32_t i = 0; i < nparams; i++) {
            bool is_fp = (i < ctx->func->nvreg &&
                          ctx->func->vregs[i].rep == XR_REP_F64);
            int8_t ri = xra_vreg_first_reg(ctx->xra, i);
            if (ri < 0) continue;

            if (is_fp) {
                X64Xmm dst = x64_alloc_fp_regs[ri];
                x64_movsd_rm(&ctx->buf, dst, X64_RSI, (int32_t)(i * 8));
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0) {
                    int32_t offset = -(X64_SPILL_BASE + slot * 8);
                    x64_movsd_mr(&ctx->buf, X64_RBP, offset, dst);
                }
            } else {
                X64Reg dst = x64_alloc_regs[ri];
                x64_mov_rm(&ctx->buf, dst, X64_RSI, (int32_t)(i * 8));
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0) {
                    int32_t offset = -(X64_SPILL_BASE + slot * 8);
                    x64_mov_mr(&ctx->buf, X64_RBP, offset, dst);
                }
            }
        }
    }
}

static void x64_emit_epilogue(X64CodegenCtx *ctx) {
    /* Restore callee-saved registers */
    x64_pop_r(&ctx->buf, X64_R15);
    x64_pop_r(&ctx->buf, X64_R14);
    x64_pop_r(&ctx->buf, X64_R13);
    x64_pop_r(&ctx->buf, X64_R12);
    x64_pop_r(&ctx->buf, X64_RBX);

    /* MOV RSP, RBP; POP RBP */
    x64_mov_rr(&ctx->buf, X64_RSP, X64_RBP);
    x64_pop_r(&ctx->buf, X64_RBP);
    x64_ret(&ctx->buf);
}

/* ========== Block Emission ========== */

static void x64_emit_block(X64CodegenCtx *ctx, uint32_t block_idx) {
    XirBlock *blk = ctx->func->blocks[block_idx];

    ctx->cur_blk_id = blk->id;
    ctx->cur_ra_pos = (ctx->xra && blk->id < ctx->xra->nblk)
                      ? ctx->xra->blk_start[blk->id] : 0;

    /* Reset gap-move overrides at block entry */
    if (ctx->vreg_override && ctx->xra)
        memset(ctx->vreg_override, -128, ctx->xra->nvreg);

    /* Advance gap_move_cursor */
    if (ctx->xra && ctx->xra->gap_moves) {
        while (ctx->gap_move_cursor < ctx->xra->ngap_move &&
               ctx->xra->gap_moves[ctx->gap_move_cursor].gap_blk < blk->id)
            ctx->gap_move_cursor++;
    }

    /* Record block start offset (bytes) */
    ctx->block_offsets[blk->id] = ctx->buf.pos;

    /* Emit all instructions */
    for (uint32_t i = 0; i < blk->nins; i++) {
        ctx->cur_ra_pos = (ctx->xra && blk->id < ctx->xra->nblk)
                          ? ctx->xra->blk_start[blk->id] + 2 + (int32_t)i * 2
                          : 0;
        x64_emit_gap_moves_before(ctx, i);
        ctx->cur_ins_idx = i;
        x64_emit_xir_ins(ctx, &blk->ins[i]);
        x64_maybe_spill(ctx, blk->ins[i].dst);
    }

    /* Emit terminator */
    switch (blk->jmp.type) {
    case XIR_JMP_JMP: {
        XR_DCHECK(blk->s1 != NULL, "JMP: no s1");
        /* Fall-through optimization */
        bool is_next = (block_idx + 1 < ctx->func->nblk) &&
                       (ctx->func->blocks[block_idx + 1] == blk->s1);
        if (!is_next) {
            /* Emit JMP rel32 with placeholder, record patch */
            X64BranchPatch *p = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0xE9);  /* JMP rel32 opcode */
            p->emit_pos = ctx->buf.pos;
            p->target_blk = blk->s1->id;
            p->type = X64_PATCH_JMP;
            p->cc = X64_CC_E;  /* unused */
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);    /* placeholder rel32 */
        }
        break;
    }
    case XIR_JMP_BR: {
        XR_DCHECK(blk->s1 != NULL && blk->s2 != NULL, "BR: no s1/s2");
        /* Branch condition is in jmp.cond vreg */
        X64Reg cond_reg = x64_get_reg(ctx, blk->jmp.arg);
        x64_test_rr(&ctx->buf, cond_reg, cond_reg);

        /* JNE → s1 (true branch) */
        bool s1_is_next = (block_idx + 1 < ctx->func->nblk) &&
                          (ctx->func->blocks[block_idx + 1] == blk->s1);
        bool s2_is_next = (block_idx + 1 < ctx->func->nblk) &&
                          (ctx->func->blocks[block_idx + 1] == blk->s2);

        if (s1_is_next) {
            /* Emit JE → s2 (false branch), s1 falls through */
            X64BranchPatch *p = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, 0x84);  /* JE rel32 */
            p->emit_pos = ctx->buf.pos;
            p->target_blk = blk->s2->id;
            p->type = X64_PATCH_JCC;
            p->cc = X64_CC_E;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);
        } else {
            /* Emit JNE → s1, then (if s2 not next) JMP → s2 */
            X64BranchPatch *p1 = &ctx->patches[ctx->npatch];
            x64_emit8(&ctx->buf, 0x0F);
            x64_emit8(&ctx->buf, 0x85);  /* JNE rel32 */
            p1->emit_pos = ctx->buf.pos;
            p1->target_blk = blk->s1->id;
            p1->type = X64_PATCH_JCC;
            p1->cc = X64_CC_NE;
            ctx->npatch++;
            x64_emit32(&ctx->buf, 0);

            if (!s2_is_next) {
                X64BranchPatch *p2 = &ctx->patches[ctx->npatch];
                x64_emit8(&ctx->buf, 0xE9);
                p2->emit_pos = ctx->buf.pos;
                p2->target_blk = blk->s2->id;
                p2->type = X64_PATCH_JMP;
                p2->cc = X64_CC_E;
                ctx->npatch++;
                x64_emit32(&ctx->buf, 0);
            }
        }
        break;
    }
    case XIR_JMP_RET:
        x64_emit_epilogue(ctx);
        break;
    default:
        break;
    }
}

/* ========== Branch Patching ========== */

static void x64_patch_branches(X64CodegenCtx *ctx) {
    for (uint32_t i = 0; i < ctx->npatch; i++) {
        X64BranchPatch *p = &ctx->patches[i];
        switch (p->type) {
        case X64_PATCH_CALL_C:
            x64_patch_rel32(&ctx->buf, p->emit_pos, ctx->call_c_stub);
            break;
        case X64_PATCH_DEOPT_JCC:
        case X64_PATCH_DEOPT_JMP:
            x64_patch_rel32(&ctx->buf, p->emit_pos, ctx->deopt_stub);
            break;
        case X64_PATCH_CALL_SELF:
            x64_patch_rel32(&ctx->buf, p->emit_pos, 0);  /* entry at offset 0 */
            break;
        case X64_PATCH_CALL_SELF_FAST:
            x64_patch_rel32(&ctx->buf, p->emit_pos, ctx->fast_entry_offset);
            break;
        default:
            XR_DCHECK(p->target_blk < ctx->nblock_offsets,
                      "x64_patch: target block OOB");
            x64_patch_rel32(&ctx->buf, p->emit_pos,
                           ctx->block_offsets[p->target_blk]);
            break;
        }
    }
}

/* ========== Main Codegen Entry ========== */

XirCodegenResult xir_codegen_x64(XirFunc *func, XirCodeAlloc *alloc) {
    XR_DCHECK(func != NULL, "xir_codegen_x64: func is NULL");
    XR_DCHECK(alloc != NULL, "xir_codegen_x64: alloc is NULL");
    XirCodegenResult result = {
        .code = NULL, .code_size = 0, .success = false, .error = NULL,
        .nosr = 0, .ndeopt = 0, .stack_map = NULL,
        .fast_entry_offset = 0, .resume_entry_offset = 0,
    };

    if (!func || !alloc || func->nblk == 0) {
        result.error = "invalid function or allocator";
        return result;
    }
    if (func->nvreg > X64_MAX_VREGS) {
        result.error = "too many virtual registers";
        return result;
    }

    /* Rebuild vreg def pointers (may be stale after optimization passes) */
    for (uint32_t v = 0; v < func->nvreg; v++)
        func->vregs[v].def = NULL;
    for (uint32_t b = 0; b < func->nblk; b++) {
        XirBlock *blk = func->blocks[b];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t vi = XIR_REF_INDEX(ins->dst);
                if (vi < func->nvreg)
                    func->vregs[vi].def = ins;
            }
        }
    }

    X64CodegenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = func;
    ctx.alloc = alloc;
    ctx.patches_cap = X64_INIT_PATCHES;
    ctx.patches = (X64BranchPatch *)xr_malloc(
        X64_INIT_PATCHES * sizeof(X64BranchPatch));

    /* Pre-RA: aggressive MOV coalescing */
    xir_coalesce(func);

    /* Register allocation */
    ctx.xra = xra_run(func);
    if (ctx.xra && ctx.xra->had_error) {
        result.error = "regalloc refused: spill slot limit exceeded";
        xr_free(ctx.patches);
        xra_result_free(ctx.xra);
        return result;
    }

    /* Gap-move override array */
    if (ctx.xra && ctx.xra->nvreg > 0) {
        ctx.vreg_override = (int8_t *)xr_malloc(ctx.xra->nvreg * sizeof(int8_t));
        memset(ctx.vreg_override, -128, ctx.xra->nvreg);
    }

    /* Block offsets array */
    uint32_t max_blk_id = 0;
    for (uint32_t i = 0; i < func->nblk; i++) {
        if (func->blocks[i]->id > max_blk_id)
            max_blk_id = func->blocks[i]->id;
    }
    ctx.block_offsets = (uint32_t *)xr_calloc(max_blk_id + 1, sizeof(uint32_t));
    ctx.nblock_offsets = max_blk_id + 1;

    /* Allocate code buffer: x86-64 instructions are variable-length (avg ~5 bytes),
     * estimate 40 bytes per XIR instruction + overhead */
    uint32_t total_xir_ins = 0;
    for (uint32_t i = 0; i < func->nblk; i++)
        total_xir_ins += func->blocks[i]->nins + 4;
    uint32_t alloc_size = total_xir_ins * 40 + 1024;
    alloc_size = (alloc_size + 4095) & ~(uint32_t)4095;
    if (alloc_size < 8192) alloc_size = 8192;

    void *code_mem = xir_code_alloc(alloc, alloc_size, 16);
    if (!code_mem) {
        result.error = "failed to allocate executable memory";
        xr_free(ctx.block_offsets);
        xr_free(ctx.patches);
        xr_free(ctx.vreg_override);
        xra_result_free(ctx.xra);
        return result;
    }

#ifdef __APPLE__
    xir_code_make_writable(code_mem, alloc_size);
#endif

    x64_buf_init(&ctx.buf, (uint8_t *)code_mem, alloc_size);

    /* Emit prologue */
    x64_emit_prologue(&ctx);

    /* Emit all blocks */
    for (uint32_t i = 0; i < func->nblk; i++)
        x64_emit_block(&ctx, i);

    /* Emit stubs (after all blocks, before patching) */
    x64_emit_deopt_stub(&ctx);
    x64_emit_call_c_stub(&ctx);

    /* Patch branches + call stubs */
    x64_patch_branches(&ctx);

    /* Patch frame size: SPILL_BASE + spill area, 16-byte aligned */
    uint32_t frame_size = (X64_JIT_FRAME_BASE +
                          (ctx.xra ? ctx.xra->nspill * 8 : 0) + 15) & ~(uint32_t)15;
    /* The push of 5 callee-saved regs (40 bytes) + push rbp (8) = 48 bytes
     * already on stack. frame_size is the SUB RSP amount.
     * Total stack = 48 + 8(return addr) + frame_size.
     * For 16-byte alignment: (48 + 8 + frame_size) % 16 == 0
     * → frame_size must be even multiple of 16 minus 56, or we adjust. */
    /* Simpler: ensure (frame_size + 48) is 16-byte aligned.
     * 48 = 5 * 8(callee push) + 0 (rbp is in the sub area).
     * Actually: push rbp=8, sub rsp,frame_size, then 5x push = 5*8=40.
     * Total below return addr = 8(rbp) + frame_size + 40.
     * For 16-byte align: (8 + frame_size + 40 + 8) % 16 == 0
     * → (frame_size + 56) % 16 == 0 → frame_size % 16 == 8 */
    if ((frame_size + 56) % 16 != 0)
        frame_size = ((frame_size + 56 + 15) & ~(uint32_t)15) - 56;
    /* Patch the frame size in the prologue SUB RSP, imm32 */
    for (uint32_t i = 0; i < ctx.nsub_patches; i++) {
        memcpy(&ctx.buf.code[ctx.frame_patch_sub[i]], &frame_size, 4);
    }

    uint32_t code_size = x64_buf_offset(&ctx.buf);

    xir_code_make_executable(code_mem, code_size);
    /* x86-64 doesn't need icache flush (no split I/D cache) */

    if (ctx.had_error) {
        result.error = "x64 codegen: unsupported opcode or regalloc error";
        xr_free(ctx.block_offsets);
        xr_free(ctx.patches);
        xr_free(ctx.vreg_override);
        xra_result_free(ctx.xra);
        return result;
    }

    result.code = code_mem;
    result.code_size = code_size;
    result.fast_entry_offset = 0;  /* No fast entry yet */
    result.success = true;

    xr_free(ctx.block_offsets);
    xr_free(ctx.patches);
    xr_free(ctx.vreg_override);
    xra_result_free(ctx.xra);
    return result;
}

#endif // __x86_64__
