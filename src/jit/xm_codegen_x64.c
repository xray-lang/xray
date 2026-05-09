/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_x64.c - Xm → x86-64 machine code generation
 *
 * KEY CONCEPT:
 *   Translates Xm SSA instructions into x86-64 machine code.
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
 * COVERAGE: integer arithmetic, control flow, calls. Float ops and
 *           SIMD are not yet emitted by this backend.
 *
 * RELATED MODULES:
 *   - xm_x64.h/c: x86-64 instruction encoding
 *   - xm_target_x64.c: register inventory and frame layout
 *   - xm_codegen.h: shared codegen result structure
 *   - xm_codegen_x64_internal.h: internal context types
 */

#if defined(__x86_64__) || defined(_M_X64)

#include "xm_codegen_x64_internal.h"
#include "xm_coalesce.h"
#include "xm_code_alloc.h"
#include "xm_offsets.h"
#include "xm_jit_runtime.h"
#include "../coro/xcoroutine.h" /* XM_SUSPEND_SPILL_MAX */
#include <string.h>

/* Forward declarations for helpers (defined after emit_xm_ins).
 * These are XR_FUNC (not static) so xm_codegen_x64_call.c can use them. */

/* ========== Register Mapping Tables ========== */

#ifdef _WIN32
const X64Reg x64_alloc_regs[X64_MAX_PHYS_REGS] = {
    /* Caller-saved (first 6): Win64 treats RSI/RDI as callee-saved */
    X64_RAX,
    X64_RCX,
    X64_RDX,
    X64_R8,
    X64_R9,
    X64_R10,
    /* Callee-saved (next 5) — r14=jit_ctx, r15=coro, rbp=FP excluded */
    X64_RBX,
    X64_RDI,
    X64_RSI,
    X64_R12,
    X64_R13,
};
#else
const X64Reg x64_alloc_regs[X64_MAX_PHYS_REGS] = {
    /* Caller-saved (first 8) */
    X64_RAX,
    X64_RCX,
    X64_RDX,
    X64_RSI,
    X64_RDI,
    X64_R8,
    X64_R9,
    X64_R10,
    /* Callee-saved (next 3) — r14=jit_ctx, r15=coro, rbp=FP excluded */
    X64_RBX,
    X64_R12,
    X64_R13,
};
#endif

const X64Reg x64_alloc_fp_regs[X64_MAX_FP_REGS] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};

/* ========== Register Lookup ========== */

X64Reg x64_get_reg(X64CodegenCtx *ctx, XmRef ref) {
    if (xm_ref_is_none(ref))
        return X64_SCRATCH_REG;
    if (!xm_ref_is_vreg(ref))
        return X64_SCRATCH_REG;
    uint32_t idx = XM_REF_INDEX(ref);

    /* FP vregs use xmm registers — return scratch GP to avoid misuse.
     * Callers handling FP ops use x64_get_fp_reg() instead. */
    if (idx < ctx->func->nvreg && ctx->func->vregs[idx].rep == XR_REP_F64)
        return X64_SCRATCH_REG;

    int8_t ri;
    if (ctx->vreg_override && idx < ctx->xra->nvreg && ctx->vreg_override[idx] != -128)
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

X64Reg x64_get_operand(X64CodegenCtx *ctx, XmRef ref, X64Reg scratch) {
    /* Constants: load into scratch */
    if (xm_ref_is_const(ref)) {
        uint32_t ci = XM_REF_INDEX(ref);
        CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "x64_get_operand: const OOB");
        uint64_t val = ctx->func->consts[ci].val.raw;
        x64_load_imm64(&ctx->buf, scratch, val);
        return scratch;
    }
    return x64_get_reg(ctx, ref);
}

/* ========== FP Register Lookup ========== */

X64Xmm x64_get_fp_reg(X64CodegenCtx *ctx, XmRef ref) {
    CODEGEN_CHECK(ctx, xm_ref_is_vreg(ref), "x64_get_fp_reg: not a vreg");
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
    CODEGEN_CHECK(ctx, ri < X64_MAX_FP_REGS, "x64_get_fp_reg: reg index OOB");
    return x64_alloc_fp_regs[ri];
}

X64Xmm x64_get_fp_operand(X64CodegenCtx *ctx, XmRef ref, X64Xmm scratch) {
    if (xm_ref_is_const(ref)) {
        /* Load float constant: GP load → MOVQ to xmm */
        uint32_t ci = XM_REF_INDEX(ref);
        CODEGEN_CHECK(ctx, ci < ctx->func->nconst, "x64_get_fp_operand: const OOB");
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
        x64_mov_ri32(buf, dst, (uint32_t) val);
    } else {
        /* Full 64-bit MOV */
        x64_mov_ri64(buf, dst, val);
    }
}

void x64_maybe_spill(X64CodegenCtx *ctx, XmRef dst_ref) {
    if (!xm_ref_is_vreg(dst_ref))
        return;
    uint32_t idx = XM_REF_INDEX(dst_ref);
    int16_t slot = xra_vreg_spill(ctx->xra, idx);
    if (slot < 0)
        return;
    int8_t ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    int32_t offset = -(X64_SPILL_BASE + slot * 8);
    bool is_fp = x64_is_fp_vreg(ctx, dst_ref);
    if (ri >= 0) {
        /* vreg is in an allocated register — spill from there */
        if (is_fp)
            x64_movsd_mr(&ctx->buf, X64_RBP, offset, x64_alloc_fp_regs[ri]);
        else
            x64_mov_mr(&ctx->buf, X64_RBP, offset, x64_alloc_regs[ri]);
    } else {
        /* vreg has no register (spill-only) — instruction result was placed
         * in SCRATCH by x64_get_reg; must spill from there. */
        if (is_fp)
            x64_movsd_mr(&ctx->buf, X64_RBP, offset, X64_SCRATCH_XMM);
        else
            x64_mov_mr(&ctx->buf, X64_RBP, offset, X64_SCRATCH_REG);
    }
}

/* ========== Branch Patching ========== */

void x64_add_patch(X64CodegenCtx *ctx, X64PatchType type, uint32_t target_blk, X64Cond cc) {
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
    p->emit_pos = 0; /* Set by caller after emitting opcode bytes */
    p->target_blk = target_blk;
    p->type = type;
    p->cc = cc;
}

/* ========== Gap Moves ========== */

static void x64_emit_gap_moves_before(X64CodegenCtx *ctx, uint32_t ins_idx) {
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
                    X64Xmm sf = x64_alloc_fp_regs[gm->src_reg];
                    X64Xmm df = x64_alloc_fp_regs[gm->dst_reg];
                    if (sf != df)
                        x64_movsd_rr(&ctx->buf, df, sf);
                } else {
                    X64Reg sh = x64_alloc_regs[gm->src_reg];
                    X64Reg dh = x64_alloc_regs[gm->dst_reg];
                    if (sh != dh)
                        x64_mov_rr(&ctx->buf, dh, sh);
                }
            } else if (gm->src_reg >= 0 && gm->dst_reg < 0) {
                /* reg-to-spill (store) */
                int32_t offset = -(X64_SPILL_BASE + gm->spill_slot * 8);
                if (gm->is_fp) {
                    X64Xmm sf = x64_alloc_fp_regs[gm->src_reg];
                    x64_movsd_mr(&ctx->buf, X64_RBP, offset, sf);
                } else {
                    X64Reg sh = x64_alloc_regs[gm->src_reg];
                    x64_mov_mr(&ctx->buf, X64_RBP, offset, sh);
                }
            } else if (gm->src_reg < 0 && gm->dst_reg >= 0) {
                /* spill-to-reg (reload) */
                int32_t offset = -(X64_SPILL_BASE + gm->spill_slot * 8);
                if (gm->is_fp) {
                    X64Xmm df = x64_alloc_fp_regs[gm->dst_reg];
                    x64_movsd_rm(&ctx->buf, df, X64_RBP, offset);
                } else {
                    X64Reg dh = x64_alloc_regs[gm->dst_reg];
                    x64_mov_rm(&ctx->buf, dh, X64_RBP, offset);
                }
            }
            /* Record override so subsequent lookups see the new register */
            if (ctx->vreg_override && gm->vreg < ctx->xra->nvreg)
                ctx->vreg_override[gm->vreg] = gm->dst_reg;
        }
        cursor++;
    }
    ctx->gap_move_cursor = cursor;
}

/* ========== Prologue / Epilogue ========== */

static void x64_emit_prologue(X64CodegenCtx *ctx) {
    /* PUSH RBP; MOV RBP, RSP */
    x64_push_r(&ctx->buf, X64_RBP);
    x64_mov_rr(&ctx->buf, X64_RBP, X64_RSP);

    /* SUB RSP, frame_size (placeholder imm32 — patched later).
     * Always emit the imm32 form (REX.W 81 EC imm32) so the
     * displacement can be patched to any value up to 2 GiB. */
    CODEGEN_CHECK(ctx, ctx->nsub_patches < 16, "too many frame sub patches");
    x64_emit8(&ctx->buf, 0x48); /* REX.W */
    x64_emit8(&ctx->buf, 0x81); /* SUB r/m64, imm32 */
    x64_emit8(&ctx->buf, 0xEC); /* ModRM: mod=11, /5, rm=RSP */
    ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.pos;
    x64_emit32(&ctx->buf, X64_JIT_FRAME_BASE); /* placeholder */

#ifdef _WIN32
    /* Save XMM callee-saved registers xmm6-xmm14 into frame area (MOVSD, 8B each).
     * Only the low 64 bits are saved — JIT code uses scalar double only.
     * Full 128-bit XMM state from C callers is preserved because our allocatable
     * FP ops never touch upper 64 bits (SSE2 scalar instructions zero-extend). */
    for (int i = 0; i < X64_CALLEE_XMM_COUNT; i++) {
        int32_t off = -(int32_t) (X64_XMM_SAVE_OFFSET + i * 8);
        x64_movsd_mr(&ctx->buf, X64_RBP, off, (X64Xmm) (6 + i));
    }
#endif

    /* Save callee-saved GP registers */
    x64_push_r(&ctx->buf, X64_RBX);
#ifdef _WIN32
    x64_push_r(&ctx->buf, X64_RDI);
    x64_push_r(&ctx->buf, X64_RSI);
#endif
    x64_push_r(&ctx->buf, X64_R12);
    x64_push_r(&ctx->buf, X64_R13);
    x64_push_r(&ctx->buf, X64_R14);
    x64_push_r(&ctx->buf, X64_R15);

    /* Save coro pointer from platform-ABI first arg register */
    x64_mov_rr(&ctx->buf, X64_CORO_REG, X64_ABI_ARG1);

    /* Load jit_ctx from coro */
    x64_mov_rm(&ctx->buf, X64_JIT_CTX_REG, X64_CORO_REG, (int32_t) XM_CORO_JIT_CTX_OFFSET);

    /* Save stack_map_ptr from jit_ctx into frame for GC and smap restoration
     * after JIT→JIT calls. Mirrors ARM64's FRAME_SMAP_PTR_OFFSET store. */
    x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_OFFSET);
    x64_mov_mr(&ctx->buf, X64_RBP, -(int32_t) X64_FRAME_SMAP_PTR_OFFSET, X64_SCRATCH_REG);
    /* Initialize safepoint_id = UINT32_MAX (no active safepoint) */
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, 0xFFFFFFFF);
    x64_mov_mr32(&ctx->buf, X64_RBP, -(int32_t) X64_FRAME_SMAP_ID_OFFSET, X64_SCRATCH_REG);

    /* Load params from args array (ABI_ARG2 = args pointer).
     * Use xra_reg_at_pos with the entry block start position to get the
     * correct register at function entry (xra_vreg_first_reg returns
     * the first non-spilled segment which may be a later split).
     *
     * Two-pass: first load params that do NOT clobber ABI_ARG2,
     * then load the one that does. Prevents clobbering the args
     * pointer before all params are loaded. */
    uint32_t nparams = ctx->func->num_params;
    if (nparams > 0 && ctx->xra) {
        uint32_t entry_id = ctx->func->blocks[0]->id;
        int32_t entry_pos = (entry_id < ctx->xra->nblk) ? ctx->xra->blk_start[entry_id] : 0;
        int32_t deferred = -1; /* param index that clobbers ABI_ARG2 */
        for (uint32_t i = 0; i < nparams; i++) {
            bool is_fp = (i < ctx->func->nvreg && ctx->func->vregs[i].rep == XR_REP_F64);
            int8_t ri = xra_reg_at_pos(ctx->xra, i, entry_pos);
            if (ri < 0) {
                /* No register at entry — load via scratch to spill slot */
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0) {
                    int32_t offset = -(X64_SPILL_BASE + slot * 8);
                    if (is_fp) {
                        x64_movsd_rm(&ctx->buf, (X64Xmm) X64_SCRATCH_XMM, X64_ABI_ARG2,
                                     (int32_t) (i * 8));
                        x64_movsd_mr(&ctx->buf, X64_RBP, offset, (X64Xmm) X64_SCRATCH_XMM);
                    } else {
                        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_ABI_ARG2, (int32_t) (i * 8));
                        x64_mov_mr(&ctx->buf, X64_RBP, offset, X64_SCRATCH_REG);
                    }
                }
                continue;
            }

            /* Defer the param that would clobber the args pointer */
            if (!is_fp && x64_alloc_regs[ri] == X64_ABI_ARG2) {
                deferred = (int32_t) i;
                continue;
            }

            if (is_fp) {
                X64Xmm dst = x64_alloc_fp_regs[ri];
                x64_movsd_rm(&ctx->buf, dst, X64_ABI_ARG2, (int32_t) (i * 8));
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0) {
                    int32_t offset = -(X64_SPILL_BASE + slot * 8);
                    x64_movsd_mr(&ctx->buf, X64_RBP, offset, dst);
                }
            } else {
                X64Reg dst = x64_alloc_regs[ri];
                x64_mov_rm(&ctx->buf, dst, X64_ABI_ARG2, (int32_t) (i * 8));
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0) {
                    int32_t offset = -(X64_SPILL_BASE + slot * 8);
                    x64_mov_mr(&ctx->buf, X64_RBP, offset, dst);
                }
            }
        }

        /* Load the deferred param last (clobbers ABI_ARG2 = args pointer) */
        if (deferred >= 0) {
            uint32_t i = (uint32_t) deferred;
            int8_t ri = xra_reg_at_pos(ctx->xra, i, entry_pos);
            X64Reg dst = x64_alloc_regs[ri];
            x64_mov_rm(&ctx->buf, dst, X64_ABI_ARG2, (int32_t) (i * 8));
            int16_t slot = xra_vreg_spill(ctx->xra, i);
            if (slot >= 0) {
                int32_t offset = -(X64_SPILL_BASE + slot * 8);
                x64_mov_mr(&ctx->buf, X64_RBP, offset, dst);
            }
        }
    }

    /* Init vreg_runtime_tags for TAGGED params from param_tags[].
     * Params are vregs 0..n-1 by construction, so vreg index == param index. */
    for (uint32_t i = 0; i < nparams && i < 8; i++) {
        if (i >= ctx->func->nvreg || i >= XR_JIT_MAX_VREG_TAGS)
            continue;
        if (ctx->func->vregs[i].rep != XR_REP_TAGGED)
            continue;
        int32_t pt_off = (int32_t) (XM_JIT_PARAM_TAGS_OFFSET + i * 8);
        int32_t rt_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) i;
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG, pt_off);
        x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, rt_off, X64_SCRATCH_REG);
    }
}

/* Fast prologue: frame setup + coro/jit_ctx load, NO args-pointer param loading.
 * Used as CALL target for register-passing self-calls where args are
 * already in alloc_regs[0..N].  Moves them to RA-assigned registers. */
static void x64_emit_fast_prologue(X64CodegenCtx *ctx) {
    /* PUSH RBP; MOV RBP, RSP */
    x64_push_r(&ctx->buf, X64_RBP);
    x64_mov_rr(&ctx->buf, X64_RBP, X64_RSP);

    /* SUB RSP, frame_size (placeholder, patched later) */
    CODEGEN_CHECK(ctx, ctx->nsub_patches < 16, "too many frame sub patches");
    x64_emit8(&ctx->buf, 0x48);
    x64_emit8(&ctx->buf, 0x81);
    x64_emit8(&ctx->buf, 0xEC);
    ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.pos;
    x64_emit32(&ctx->buf, X64_JIT_FRAME_BASE);

#ifdef _WIN32
    /* Save callee-saved XMM registers */
    for (int i = 0; i < X64_CALLEE_XMM_COUNT; i++) {
        int32_t off = -(int32_t) (X64_XMM_SAVE_OFFSET + i * 8);
        x64_movsd_mr(&ctx->buf, X64_RBP, off, (X64Xmm) (6 + i));
    }
#endif

    /* Save callee-saved GP registers */
    x64_push_r(&ctx->buf, X64_RBX);
#ifdef _WIN32
    x64_push_r(&ctx->buf, X64_RDI);
    x64_push_r(&ctx->buf, X64_RSI);
#endif
    x64_push_r(&ctx->buf, X64_R12);
    x64_push_r(&ctx->buf, X64_R13);
    x64_push_r(&ctx->buf, X64_R14);
    x64_push_r(&ctx->buf, X64_R15);

    /* Save coro pointer from platform-ABI first arg register */
    x64_mov_rr(&ctx->buf, X64_CORO_REG, X64_ABI_ARG1);

    /* Load jit_ctx pointer */
    x64_mov_rm(&ctx->buf, X64_JIT_CTX_REG, X64_CORO_REG, (int32_t) XM_CORO_JIT_CTX_OFFSET);

    /* Save stack_map_ptr from jit_ctx into frame */
    x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_OFFSET);
    x64_mov_mr(&ctx->buf, X64_RBP, -(int32_t) X64_FRAME_SMAP_PTR_OFFSET, X64_SCRATCH_REG);
    /* Initialize safepoint_id = UINT32_MAX */
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, 0xFFFFFFFF);
    x64_mov_mr32(&ctx->buf, X64_RBP, -(int32_t) X64_FRAME_SMAP_ID_OFFSET, X64_SCRATCH_REG);

    /* Move params from alloc_regs[i] → RA-assigned registers.
     * Self-calls place arg i in alloc_regs[i]; if RA assigned param i
     * to a different register index ri, move alloc_regs[i] → alloc_regs[ri]. */
    uint32_t nparams = ctx->func->num_params;
    if (nparams > 0 && ctx->xra) {
        for (uint32_t i = 0; i < nparams; i++) {
            bool is_fp = (i < ctx->func->nvreg && ctx->func->vregs[i].rep == XR_REP_F64);
            int8_t ri = xra_vreg_first_reg(ctx->xra, i);
            if (ri < 0)
                continue;
            if (is_fp) {
                X64Xmm dst = x64_alloc_fp_regs[ri];
                X64Xmm src = x64_alloc_fp_regs[i];
                if (dst != src)
                    x64_movsd_rr(&ctx->buf, dst, src);
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0) {
                    int32_t offset = -(X64_SPILL_BASE + slot * 8);
                    x64_movsd_mr(&ctx->buf, X64_RBP, offset, dst);
                }
            } else {
                X64Reg dst = x64_alloc_regs[ri];
                X64Reg src = x64_alloc_regs[i];
                if (dst != src)
                    x64_mov_rr(&ctx->buf, dst, src);
                int16_t slot = xra_vreg_spill(ctx->xra, i);
                if (slot >= 0) {
                    int32_t offset = -(X64_SPILL_BASE + slot * 8);
                    x64_mov_mr(&ctx->buf, X64_RBP, offset, dst);
                }
            }
        }
    }
}

void x64_emit_epilogue(X64CodegenCtx *ctx) {
    /* Restore callee-saved GP registers (reverse push order) */
    x64_pop_r(&ctx->buf, X64_R15);
    x64_pop_r(&ctx->buf, X64_R14);
    x64_pop_r(&ctx->buf, X64_R13);
    x64_pop_r(&ctx->buf, X64_R12);
#ifdef _WIN32
    x64_pop_r(&ctx->buf, X64_RSI);
    x64_pop_r(&ctx->buf, X64_RDI);
#endif
    x64_pop_r(&ctx->buf, X64_RBX);

#ifdef _WIN32
    /* Restore callee-saved XMM registers xmm6-xmm14 */
    for (int i = 0; i < X64_CALLEE_XMM_COUNT; i++) {
        int32_t off = -(int32_t) (X64_XMM_SAVE_OFFSET + i * 8);
        x64_movsd_rm(&ctx->buf, (X64Xmm) (6 + i), X64_RBP, off);
    }
#endif

    /* MOV RSP, RBP; POP RBP */
    x64_mov_rr(&ctx->buf, X64_RSP, X64_RBP);
    x64_pop_r(&ctx->buf, X64_RBP);
    x64_ret(&ctx->buf);
}

/* ========== Block Emission ========== */

static void x64_emit_block(X64CodegenCtx *ctx, uint32_t block_idx) {
    XmBlock *blk = ctx->func->blocks[block_idx];

    ctx->cur_blk_id = blk->id;
    ctx->cur_ra_pos = (ctx->xra && blk->id < ctx->xra->nblk) ? ctx->xra->blk_start[blk->id] : 0;

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

    /* Snapshot loop-header blocks for later OSR stub emission.
     * Skip when the function has coroutine deopt (AWAIT/SCOPE_EXIT): OSR
     * cannot correctly recover full interpreter state in that case and
     * may double-execute side effects (matches ARM64 behaviour). */
    if (blk->is_loop_header && ctx->nosr_snap < XM_MAX_OSR_ENTRIES && !ctx->func->has_coro_deopt) {
        ctx->osr_snaps[ctx->nosr_snap].block_id = blk->id;
        ctx->osr_snaps[ctx->nosr_snap].block_offset = ctx->buf.pos;
        ctx->nosr_snap++;
    }

    /* Emit all instructions */
    for (uint32_t i = 0; i < blk->nins; i++) {
        ctx->cur_ra_pos = (ctx->xra && blk->id < ctx->xra->nblk)
                              ? ctx->xra->blk_start[blk->id] + 2 + (int32_t) i * 2
                              : 0;
        x64_emit_gap_moves_before(ctx, i);
        ctx->cur_ins_idx = i;
        x64_emit_xm_ins(ctx, &blk->ins[i]);
        x64_maybe_spill(ctx, blk->ins[i].dst);
    }

    /* Emit terminator */
    switch (blk->jmp.type) {
        case XM_JMP_JMP: {
            CODEGEN_CHECK(ctx, blk->s1 != NULL, "JMP: no s1");
            x64_emit_edge_copies(ctx, blk->s1, blk);
            /* Fall-through optimization */
            bool is_next =
                (block_idx + 1 < ctx->func->nblk) && (ctx->func->blocks[block_idx + 1] == blk->s1);
            if (!is_next) {
                /* Emit JMP rel32 with placeholder, record patch */
                X64BranchPatch *p = &ctx->patches[ctx->npatch];
                x64_emit8(&ctx->buf, 0xE9); /* JMP rel32 opcode */
                p->emit_pos = ctx->buf.pos;
                p->target_blk = blk->s1->id;
                p->type = X64_PATCH_JMP;
                p->cc = X64_CC_E; /* unused */
                ctx->npatch++;
                x64_emit32(&ctx->buf, 0); /* placeholder rel32 */
            }
            break;
        }
        case XM_JMP_BR: {
            CODEGEN_CHECK(ctx, blk->s1 != NULL && blk->s2 != NULL, "BR: no s1/s2");
            X64Reg cond_reg = x64_get_reg(ctx, blk->jmp.arg);
            x64_test_rr(&ctx->buf, cond_reg, cond_reg);

            bool s1_is_next =
                (block_idx + 1 < ctx->func->nblk) && (ctx->func->blocks[block_idx + 1] == blk->s1);
            bool s2_is_next =
                (block_idx + 1 < ctx->func->nblk) && (ctx->func->blocks[block_idx + 1] == blk->s2);

            if (s1_is_next) {
                /* Layout: JNE skip → edge_copies(s2) → JMP s2
                 *         skip: edge_copies(s1) → fallthrough to s1 */
                x64_emit8(&ctx->buf, 0x0F);
                x64_emit8(&ctx->buf, 0x85); /* JNE rel32 */
                uint32_t skip_pos = ctx->buf.pos;
                x64_emit32(&ctx->buf, 0);

                x64_emit_edge_copies(ctx, blk->s2, blk);
                X64BranchPatch *p = &ctx->patches[ctx->npatch];
                x64_emit8(&ctx->buf, 0xE9);
                p->emit_pos = ctx->buf.pos;
                p->target_blk = blk->s2->id;
                p->type = X64_PATCH_JMP;
                p->cc = X64_CC_E;
                ctx->npatch++;
                x64_emit32(&ctx->buf, 0);

                /* Patch JNE to here (true path) */
                int32_t rel = (int32_t) (ctx->buf.pos - (skip_pos + 4));
                memcpy(&ctx->buf.code[skip_pos], &rel, 4);
                x64_emit_edge_copies(ctx, blk->s1, blk);
                /* s1 falls through */
            } else {
                /* Layout: JE skip → edge_copies(s1) → JMP s1
                 *         skip: edge_copies(s2) → JMP/fallthrough s2 */
                x64_emit8(&ctx->buf, 0x0F);
                x64_emit8(&ctx->buf, 0x84); /* JE rel32 */
                uint32_t skip_pos = ctx->buf.pos;
                x64_emit32(&ctx->buf, 0);

                x64_emit_edge_copies(ctx, blk->s1, blk);
                X64BranchPatch *p1 = &ctx->patches[ctx->npatch];
                x64_emit8(&ctx->buf, 0xE9);
                p1->emit_pos = ctx->buf.pos;
                p1->target_blk = blk->s1->id;
                p1->type = X64_PATCH_JMP;
                p1->cc = X64_CC_E;
                ctx->npatch++;
                x64_emit32(&ctx->buf, 0);

                /* Patch JE to here (false path) */
                int32_t rel = (int32_t) (ctx->buf.pos - (skip_pos + 4));
                memcpy(&ctx->buf.code[skip_pos], &rel, 4);
                x64_emit_edge_copies(ctx, blk->s2, blk);
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
        case XM_JMP_RET: {
            /* Return convention: RAX = payload, RCX = tag (JIT-to-JIT).
             * SysV C callers: struct{i64,u64} returned in RAX+RDX.
             * Win64 C callers: int64_t returned in RAX, tag in jit_ctx.
             *
             * Must move payload to RAX FIRST — the return vreg may live in
             * RCX or RDX, which are clobbered by the tag writes below. */
            if (!xm_ref_is_none(blk->jmp.arg)) {
                uint32_t ret_idx = XM_REF_INDEX(blk->jmp.arg);
                bool is_fp =
                    (ret_idx < ctx->func->nvreg && ctx->func->vregs[ret_idx].rep == XR_REP_F64);

                /* Move payload to RAX before tag writes clobber RCX/RDX */
                if (is_fp) {
                    X64Xmm fsrc = x64_get_fp_reg(ctx, blk->jmp.arg);
                    x64_movq_gp_xmm(&ctx->buf, X64_RAX, fsrc);
                } else {
                    X64Reg ret_reg = x64_get_reg(ctx, blk->jmp.arg);
                    if (ret_reg != X64_RAX)
                        x64_mov_rr(&ctx->buf, X64_RAX, ret_reg);
                }

                /* Compute return type tag */
                uint8_t ret_xr_tag = 3; /* XR_TAG_I64 default */
                if (ret_idx < ctx->func->nvreg) {
                    XmType rct = xm_ref_ctype(ctx->func, blk->jmp.arg);
                    uint8_t ret_vtag = type_kind_to_vtag(rct.kind);
                    uint8_t vt = vtag_to_value_tag(ret_vtag);
                    if (vt != 0xFF) {
                        ret_xr_tag = vt;
                    } else if (ret_vtag == VTAG_TAGGED) {
                        uint8_t mt = ctx->func->vregs[ret_idx].rep;
                        if (mt == XR_REP_F64)
                            ret_xr_tag = 4; /* XR_TAG_F64 */
                        else if (mt == XR_REP_PTR)
                            ret_xr_tag = 5; /* XR_TAG_PTR */
                        else
                            ret_xr_tag = 0xFF; /* unknown */
                    }
                }
                /* RCX = tag for JIT-to-JIT callers (CALL_SELF_DIRECT reads RCX) */
                x64_mov_ri32(&ctx->buf, X64_RCX, (uint32_t) ret_xr_tag);
#ifdef _WIN32
                /* Win64: store tag to jit_ctx for C callers */
                x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET,
                           X64_RCX);
#else
                /* SysV: RDX = tag for C callers (struct return in RAX+RDX) */
                x64_mov_ri32(&ctx->buf, X64_RDX, (uint32_t) ret_xr_tag);
#endif
            }
            x64_emit_epilogue(ctx);
            break;
        }
        default:
            break;
    }
}

/* ========== Main Codegen Entry ========== */

XmCodegenResult xm_codegen_x64(XmFunc *func, XmCodeAlloc *alloc) {
    XR_DCHECK(func != NULL, "xm_codegen_x64: func is NULL");
    XR_DCHECK(alloc != NULL, "xm_codegen_x64: alloc is NULL");
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
    if (func->nvreg > X64_MAX_VREGS) {
        result.error = "too many virtual registers";
        return result;
    }

    /* Rebuild vreg def pointers (may be stale after optimization passes) */
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

    X64CodegenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = func;
    ctx.alloc = alloc;
    ctx.patches_cap = X64_INIT_PATCHES;
    ctx.patches = (X64BranchPatch *) xr_malloc(X64_INIT_PATCHES * sizeof(X64BranchPatch));

    /* Establish bail-out point: any CODEGEN_CHECK failure longjmps here */
    if (setjmp(ctx.bail_jmp) != 0) {
        result.error = ctx.error_reason ? ctx.error_reason : "codegen invariant violation";
        goto cleanup;
    }

    /* Pre-RA: aggressive MOV coalescing */
    xm_coalesce(func);

    /* Register allocation */
    ctx.xra = xra_run(func);
    if (ctx.xra && ctx.xra->had_error) {
        result.error = "regalloc refused: spill slot limit exceeded";
        goto cleanup;
    }

    /* Gap-move override array */
    if (ctx.xra && ctx.xra->nvreg > 0) {
        ctx.vreg_override = (int8_t *) xr_malloc(ctx.xra->nvreg * sizeof(int8_t));
        memset(ctx.vreg_override, -128, ctx.xra->nvreg);
    }

    /* Block offsets array */
    uint32_t max_blk_id = 0;
    for (uint32_t i = 0; i < func->nblk; i++) {
        if (func->blocks[i]->id > max_blk_id)
            max_blk_id = func->blocks[i]->id;
    }
    ctx.block_offsets = (uint32_t *) xr_calloc(max_blk_id + 1, sizeof(uint32_t));
    ctx.nblock_offsets = max_blk_id + 1;

    /* Allocate code buffer: x86-64 instructions are variable-length (avg ~5 bytes),
     * estimate 40 bytes per Xm instruction + overhead */
    uint32_t total_xm_ins = 0;
    for (uint32_t i = 0; i < func->nblk; i++)
        total_xm_ins += func->blocks[i]->nins + 4;
    uint32_t alloc_size = total_xm_ins * 40 + 1024;
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

    x64_buf_init(&ctx.buf, (uint8_t *) code_mem, alloc_size);

    /* Emit normal prologue (loads params from RSI memory) */
    x64_emit_prologue(&ctx);

    /* JMP over fast prologue to body start (placeholder, patched below) */
    x64_emit8(&ctx.buf, 0xE9); /* JMP rel32 */
    uint32_t skip_jmp_pos = ctx.buf.pos;
    x64_emit32(&ctx.buf, 0);

    /* Emit fast prologue (register-based param passing for self-calls) */
    ctx.fast_entry_offset = ctx.buf.pos;
    x64_emit_fast_prologue(&ctx);

    /* Patch skip JMP: normal prologue jumps over fast prologue to body */
    x64_patch_rel32(&ctx.buf, skip_jmp_pos, ctx.buf.pos);

    /* Emit all blocks */
    for (uint32_t i = 0; i < func->nblk; i++)
        x64_emit_block(&ctx, i);

    /* Emit stubs (after all blocks, before patching) */
    x64_emit_deopt_stub(&ctx);
    x64_emit_call_c_stub(&ctx);
    x64_emit_barrier_stubs(&ctx);
    x64_emit_osr_stubs(&ctx, &result);
    x64_emit_resume_entry(&ctx, &result);

    /* Patch branches + call stubs */
    x64_patch_branches(&ctx);

    /* Patch frame size: SPILL_BASE + spill area, 16-byte aligned.
     * Stack layout: push rbp (8), sub rsp frame_size, then N GP pushes.
     *   SysV:  N=5 (rbx,r12,r13,r14,r15)=40B.  Total = 8+frame_size+40+8(retaddr)
     *          Align: (frame_size+56)%16==0 → frame_size%16==8
     *   Win64: N=7 (rbx,rdi,rsi,r12,r13,r14,r15)=56B.  Total = 8+frame_size+56+8
     *          Align: (frame_size+72)%16==0 → frame_size%16==8
     */
    uint32_t frame_size =
        (X64_JIT_FRAME_BASE + (ctx.xra ? ctx.xra->nspill * 8 : 0) + 15) & ~(uint32_t) 15;
    uint32_t push_overhead = 8 + X64_NPUSH_CALLEE_SAVE * 8 + 8; /* rbp + callee GP + retaddr */
    if ((frame_size + push_overhead) % 16 != 0)
        frame_size = ((frame_size + push_overhead + 15) & ~(uint32_t) 15) - push_overhead;
    /* Patch the frame size in the prologue SUB RSP, imm32 */
    for (uint32_t i = 0; i < ctx.nsub_patches; i++) {
        memcpy(&ctx.buf.code[ctx.frame_patch_sub[i]], &frame_size, 4);
    }

    uint32_t code_size = x64_buf_offset(&ctx.buf);

    xm_code_make_executable(code_mem, code_size);
    /* x86-64 doesn't need icache flush (no split I/D cache) */

    if (ctx.had_error) {
        result.error = "x64 codegen: unsupported opcode or regalloc error";
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

#endif  // __x86_64__ || _M_X64
