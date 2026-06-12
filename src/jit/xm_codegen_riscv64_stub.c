/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_riscv64_stub.c - Shared stubs and safepoint helpers (RISC-V 64)
 *
 * KEY CONCEPT:
 *   Emits shared code stubs appended after the main function body.
 *   Each call site emits a JAL to these stubs, which handle the
 *   save/restore protocol around C runtime helper calls.
 *
 * STUBS:
 *   call_c_stub:  saves all allocatable regs, calls C func via JALR,
 *                 restores all regs, returns result in a0.
 *   deopt_stub:   saves all reg state to jit_ctx->deopt_regs,
 *                 returns DEOPT_MARKER in a0.
 *   barrier_fwd:  saves caller-saved, calls xr_jit_barrier_fwd.
 *   barrier_back: saves caller-saved, calls xr_jit_barrier_back.
 *
 * RISC-V SPECIFICS:
 *   - LP64D ABI: args in a0-a7, return in a0/a1, ra is explicit
 *   - No push/pop instructions: use SD/LD with SP adjustments
 *   - Stub entry via JAL ra, stub_offset (saves return addr in ra)
 *   - Within stubs, ra is saved/restored explicitly on the stack
 */

#ifdef __riscv

#include "xm_codegen_riscv64_internal.h"
#include "xm_offsets.h"
#include "xm_jit_runtime.h"
#define XM_RUNTIME_STUBS_ENTRIES
#include "xm_runtime_stubs_gen.h"

#define RV64_SCRATCH_FP_REG RV64_FT11

/* ========== Call-C Stub ========== */

/* Protocol:
 *   t6 (SCRATCH_REG) = C function pointer
 *   [s10 + EXTRA_ARG_OFFSET] = extra argument (pre-stored by codegen)
 *
 * Stub saves all allocatable GP + FP regs, sets up LP64D ABI call
 * (a0=coro, a1=extra_arg), calls via JALR t6, saves result payload/tag,
 * restores all regs, returns payload in a0.
 *
 * Stack layout during stub (relative to sp at entry):
 *   [saved ra]        sp + save_frame - 8
 *   [GP regs]         below saved ra
 *   [FP regs]         below saved GP regs
 *   [alignment pad]   if needed
 */
XR_FUNC void rv64_emit_call_c_stub(Rv64CodegenCtx *ctx) {
    if (!ctx->has_call_c)
        return;
    ctx->call_c_stub = ctx->buf.count;

    int32_t save_frame = (int32_t) ((8 + RV64_MAX_PHYS_REGS * 8 + RV64_MAX_FP_REGS * 8 + 15) & ~15);
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, -save_frame));

    /* Save ra (stub was reached via JAL, ra holds return address) */
    int32_t off = save_frame - 8;
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_RA, RV64_SP, off));
    off -= 8;

    /* Save all allocatable GP registers */
    for (int i = 0; i < RV64_MAX_PHYS_REGS; i++) {
        rv64_buf_emit(&ctx->buf, rv64_sd(rv64_alloc_regs[i], RV64_SP, off));
        off -= 8;
    }

    /* Save all allocatable FP registers */
    for (int i = 0; i < RV64_MAX_FP_REGS; i++) {
        rv64_buf_emit(&ctx->buf, rv64_fsd(rv64_alloc_fp_regs[i], RV64_SP, off));
        off -= 8;
    }

    XR_DCHECK(off >= 0 && off < 16, "rv64 call_c_stub: save frame size mismatch");

    /* Save SP to jit_ctx for GC stack map access */
    rv64_buf_emit(&ctx->buf,
                  rv64_sd(RV64_SP, RV64_JIT_CTX_REG, (int32_t) XM_JIT_SAFEPOINT_SAVED_SP_OFFSET));

    /* Set up LP64D call: a0=coro, a1=extra_arg, then JALR t6 */
    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A0, RV64_CORO_REG));
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_A1, RV64_JIT_CTX_REG, (int32_t) RV64_EXTRA_ARG_OFFSET));

    /* Save t6 (fn pointer) before clobbering it with JALR.
     * Move fn pointer to t5 (SCRATCH_REG2), then JALR via t5. */
    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG2, RV64_SCRATCH_REG));
    rv64_buf_emit(&ctx->buf, rv64_jalr(RV64_RA, RV64_SCRATCH_REG2, 0));

    /* XrJitResult returned in a0(payload), a1(tag) by LP64D.
     * Save payload to t6 (safe — just finished the call, t6 is dead).
     * Save tag to jit_ctx->call_result_tag. */
    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, RV64_A0));
    rv64_buf_emit(&ctx->buf,
                  rv64_sb(RV64_A1, RV64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET));

    /* Restore all allocatable FP registers */
    for (int i = 0; i < RV64_MAX_FP_REGS; i++) {
        rv64_buf_emit(&ctx->buf, rv64_fld(rv64_alloc_fp_regs[i], RV64_SP,
                                          save_frame - 8 - RV64_MAX_PHYS_REGS * 8 - (i + 1) * 8));
    }

    /* Restore all allocatable GP registers */
    for (int i = 0; i < RV64_MAX_PHYS_REGS; i++) {
        rv64_buf_emit(&ctx->buf,
                      rv64_ld(rv64_alloc_regs[i], RV64_SP, save_frame - 8 - (i + 1) * 8));
    }

    /* Restore ra */
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_RA, RV64_SP, save_frame - 8));

    /* Restore SP */
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, save_frame));

    /* Move result payload to a0 and return */
    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A0, RV64_SCRATCH_REG));
    rv64_buf_emit(&ctx->buf, rv64_jalr(RV64_X0, RV64_RA, 0)); /* RET */
}

/* ========== Write Barrier Stubs ========== */

/* Forward barrier: saves caller-saved GP regs, calls xr_jit_barrier_fwd.
 * On entry: t6=parent, t5=child (pre-loaded by codegen). */
XR_FUNC void rv64_emit_barrier_stubs(Rv64CodegenCtx *ctx) {
    if (!ctx->has_barriers)
        return;

    uintptr_t barrier_fwd_entry =
        xm_runtime_stub_entry(XM_RUNTIME_STUB_barrier_fwd, XM_RUNTIME_STUB_ABI_BARRIER_FWD_FIXED);
    uintptr_t barrier_back_entry =
        xm_runtime_stub_entry(XM_RUNTIME_STUB_barrier_back, XM_RUNTIME_STUB_ABI_BARRIER_BACK_FIXED);
    RV64_CODEGEN_CHECK(ctx, barrier_fwd_entry != 0, "runtime stub barrier_fwd ABI mismatch");
    RV64_CODEGEN_CHECK(ctx, barrier_back_entry != 0, "runtime stub barrier_back ABI mismatch");

    /* Forward barrier stub */
    ctx->barrier_fwd_stub = ctx->buf.count;

    /* Save caller-saved GP regs (12 regs) + ra + child(t5) + parent(t6).
     * 15 * 8 = 120, next 16-aligned = 128. */
    int32_t bfwd_frame = 128;
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, -bfwd_frame));
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_RA, RV64_SP, bfwd_frame - 8));
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_SP, bfwd_frame - 16));  /* parent */
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG2, RV64_SP, bfwd_frame - 24)); /* child */

    int32_t boff = bfwd_frame - 32;
    for (int i = 0; i < RV64_NGPR_CALLER_SAVE && i < RV64_MAX_PHYS_REGS; i++) {
        rv64_buf_emit(&ctx->buf, rv64_sd(rv64_alloc_regs[i], RV64_SP, boff));
        boff -= 8;
    }

    /* Call xr_jit_barrier_fwd(coro, parent, child) via LP64D ABI */
    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A0, RV64_CORO_REG));
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_A1, RV64_SP, bfwd_frame - 16)); /* parent */
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_A2, RV64_SP, bfwd_frame - 24)); /* child */
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) barrier_fwd_entry);
    rv64_buf_emit(&ctx->buf, rv64_jalr(RV64_RA, RV64_SCRATCH_REG, 0));

    /* Restore caller-saved */
    boff = bfwd_frame - 32;
    for (int i = 0; i < RV64_NGPR_CALLER_SAVE && i < RV64_MAX_PHYS_REGS; i++) {
        rv64_buf_emit(&ctx->buf, rv64_ld(rv64_alloc_regs[i], RV64_SP, boff));
        boff -= 8;
    }
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG2, RV64_SP, bfwd_frame - 24));
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_SP, bfwd_frame - 16));
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_RA, RV64_SP, bfwd_frame - 8));
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, bfwd_frame));
    rv64_buf_emit(&ctx->buf, rv64_ret());

    /* Back barrier stub: xr_jit_barrier_back(coro, container)
     * On entry: t6=container. */
    ctx->barrier_back_stub = ctx->buf.count;

    int32_t bback_frame = 128;
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, -bback_frame));
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_RA, RV64_SP, bback_frame - 8));
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_SP, bback_frame - 16));

    boff = bback_frame - 24;
    for (int i = 0; i < RV64_NGPR_CALLER_SAVE && i < RV64_MAX_PHYS_REGS; i++) {
        rv64_buf_emit(&ctx->buf, rv64_sd(rv64_alloc_regs[i], RV64_SP, boff));
        boff -= 8;
    }

    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_A0, RV64_CORO_REG));
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_A1, RV64_SP, bback_frame - 16));
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) barrier_back_entry);
    rv64_buf_emit(&ctx->buf, rv64_jalr(RV64_RA, RV64_SCRATCH_REG, 0));

    boff = bback_frame - 24;
    for (int i = 0; i < RV64_NGPR_CALLER_SAVE && i < RV64_MAX_PHYS_REGS; i++) {
        rv64_buf_emit(&ctx->buf, rv64_ld(rv64_alloc_regs[i], RV64_SP, boff));
        boff -= 8;
    }
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_SP, bback_frame - 16));
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_RA, RV64_SP, bback_frame - 8));
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, bback_frame));
    rv64_buf_emit(&ctx->buf, rv64_ret());
}

/* ========== Deopt Stub ========== */

/* Saves all register state to jit_ctx->deopt_regs, loads DEOPT_MARKER
 * into a0, then returns via epilogue.
 *
 * On entry to stub, [s10 + deopt_id_offset] already stores the deopt_id
 * (written by codegen before the branch to this stub). */
XR_FUNC void rv64_emit_deopt_stub(Rv64CodegenCtx *ctx) {
    if (!ctx->has_deopt)
        return;
    ctx->deopt_stub = ctx->buf.count;

    /* Save frame pointer for spill slot recovery */
    rv64_buf_emit(&ctx->buf,
                  rv64_sd(RV64_FP, RV64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_SPILL_BASE_OFFSET));

    /* Save all allocatable GP registers to jit_ctx->deopt_regs[phys_reg_num].
     * Index by hardware register number for direct phys→deopt mapping. */
    int32_t gp_base = (int32_t) XM_JIT_DEOPT_REGS_OFFSET;
    for (int i = 0; i < RV64_MAX_PHYS_REGS; i++) {
        Rv64Reg r = rv64_alloc_regs[i];
        rv64_buf_emit(&ctx->buf, rv64_sd(r, RV64_JIT_CTX_REG, gp_base + (int32_t) r * 8));
    }

    /* Save FP registers to jit_ctx->deopt_fp_regs[fpr_num] */
    int32_t fp_base = (int32_t) XM_JIT_DEOPT_FP_REGS_OFFSET;
    for (int i = 0; i < RV64_MAX_FP_REGS; i++) {
        Rv64Freg f = rv64_alloc_fp_regs[i];
        rv64_buf_emit(&ctx->buf, rv64_fsd(f, RV64_JIT_CTX_REG, fp_base + (int32_t) f * 8));
    }

    /* Copy spill slots from frame to jit_ctx->deopt_spill_save[] BEFORE
     * epilogue destroys the frame. Uses t6 (SCRATCH_REG). */
    {
        uint32_t nspill = ctx->xra ? ctx->xra->nspill : 0;
        if (nspill > 32)
            nspill = 32;
        int32_t save_base = (int32_t) XM_JIT_DEOPT_SPILL_SAVE_OFFSET;
        for (uint32_t s = 0; s < nspill; s++) {
            int32_t frame_off = -(int32_t) (RV64_SPILL_BASE + s * 8);
            rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_FP, frame_off));
            rv64_buf_emit(&ctx->buf,
                          rv64_sd(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, save_base + (int32_t) s * 8));
        }
    }

    /* Load DEOPT_MARKER into a0 as return value */
    rv64_load_imm64(&ctx->buf, RV64_A0, (uint64_t) XM_DEOPT_MARKER);

    /* Epilogue + RET */
    rv64_emit_epilogue(ctx);
}

/* Helper: emit a deopt branch — conditional BNE rs1,x0 to deopt stub.
 * Before calling this, codegen has already stored deopt_id into
 * [s10 + XM_JIT_DEOPT_ID_OFFSET]. */
XR_FUNC void rv64_emit_deopt_branch(Rv64CodegenCtx *ctx, Rv64Reg cond_reg) {
    rv64_add_patch(ctx, RV64_PATCH_DEOPT_BRANCH, 0, RV64_CC_NE, (uint8_t) cond_reg,
                   (uint8_t) RV64_X0);
    rv64_buf_emit(&ctx->buf, rv64_bne(cond_reg, RV64_X0, 0)); /* placeholder */
    ctx->has_deopt = true;
}

/* Helper: emit unconditional deopt jump */
XR_FUNC void rv64_emit_deopt_jmp(Rv64CodegenCtx *ctx) {
    rv64_add_patch(ctx, RV64_PATCH_DEOPT_JAL, 0, RV64_CC_EQ, 0, 0);
    rv64_buf_emit(&ctx->buf, rv64_j(0)); /* placeholder */
    ctx->has_deopt = true;
}

/* Helper: store deopt_id (from ins->dst const) to jit_ctx->deopt_id */
XR_FUNC void rv64_emit_deopt_id(Rv64CodegenCtx *ctx, XmIns *ins) {
    uint32_t did = 0xFFFF;
    if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
        uint32_t dci = XM_REF_INDEX(ins->dst);
        did = (uint32_t) ctx->func->consts[dci].val.raw;
    }
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) did);
    rv64_buf_emit(&ctx->buf,
                  rv64_sw(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET));
}

/* ========== GC Stack Map + Spill Writeback ========== */

/* Record safepoint bitmap: which alloc_regs hold PTR vregs (reg_bitmap),
 * and which spill slots hold PTR vregs (spill_bitmap) at cur_ra_pos. */
XR_FUNC uint32_t rv64_record_safepoint(Rv64CodegenCtx *ctx) {
    XR_DCHECK(ctx != NULL, "rv64_record_safepoint: NULL ctx");
    if (ctx->nsmap >= XM_MAX_STACK_MAP_ENTRIES) {
        /* Fail-closed: a reused/wrong safepoint id makes GC scan the wrong
         * bitmaps (missed roots → use-after-free). Mark the compile as
         * failed so the function stays on the interpreter instead. */
        xr_log_warning("rv64-cg", "stack map table full (%u entries): bail", ctx->nsmap);
        ctx->had_error = true;
        return ctx->nsmap > 0 ? ctx->nsmap - 1 : 0;
    }

    int32_t pos = ctx->cur_ra_pos;
    uint32_t reg_bitmap = 0;
    uint32_t spill_bitmap = 0;

    for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
        if (ctx->func->vregs[v].rep != XR_REP_PTR)
            continue;

        int8_t ri = xra_reg_at_pos(ctx->xra, v, pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, v, pos + 1);
        if (ri >= 0 && ri < RV64_MAX_PHYS_REGS) {
            reg_bitmap |= (1u << ri);
            if (ctx->xra && v < ctx->xra->nvreg && ctx->xra->valloc[v].spill >= 0) {
                int16_t slot = ctx->xra->valloc[v].spill;
                if (slot >= 0 && slot < XM_MAX_SPILL_SLOTS)
                    spill_bitmap |= (1u << slot);
            }
            continue;
        }

        if (ctx->xra && v < ctx->xra->nvreg && ctx->xra->valloc[v].spill >= 0 &&
            xra_vreg_live_at(ctx->xra, v, pos)) {
            int16_t slot = ctx->xra->valloc[v].spill;
            if (slot >= 0 && slot < XM_MAX_SPILL_SLOTS)
                spill_bitmap |= (1u << slot);
        }
    }

    uint32_t sid = ctx->nsmap;
    ctx->smap_entries[sid].pc_offset = rv64_buf_offset(&ctx->buf);
    ctx->smap_entries[sid].reg_bitmap = reg_bitmap;
    ctx->smap_entries[sid].spill_bitmap = spill_bitmap;
    ctx->nsmap++;
    return sid;
}

/* Write back all live PTR register values to their spill slots.
 * Called before cross-function calls so GC can find PTR values in outer
 * frames by scanning spill slots. */
XR_FUNC void rv64_emit_ptr_spill_writeback(Rv64CodegenCtx *ctx) {
    XR_DCHECK(ctx != NULL, "rv64_emit_ptr_spill_writeback: NULL ctx");
    int32_t pos = ctx->cur_ra_pos;
    for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
        if (ctx->func->vregs[v].rep != XR_REP_PTR)
            continue;
        int8_t ri = xra_reg_at_pos(ctx->xra, v, pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, v, pos + 1);
        if (ri < 0)
            continue;

        int16_t slot = ctx->xra->valloc[v].spill;
        if (slot < 0) {
            slot = (int16_t) ctx->xra->nspill++;
            ctx->xra->valloc[v].spill = slot;
        }
        if (slot >= 0 && slot < 32 && ri >= 0 && ri < RV64_MAX_PHYS_REGS) {
            Rv64Reg reg = rv64_alloc_regs[ri];
            int32_t offset = -(RV64_SPILL_BASE + slot * 8);
            rv64_buf_emit(&ctx->buf, rv64_sd(reg, RV64_FP, offset));
        }
    }
}

/* Collect live caller-saved GP regs in current block.
 * Returns count, fills out[] with hardware registers. */
XR_FUNC int rv64_live_gp(Rv64CodegenCtx *ctx, Rv64Reg *out, Rv64Reg exclude) {
    XR_DCHECK(ctx != NULL, "rv64_live_gp: NULL ctx");
    uint32_t bid = ctx->cur_blk_id;
    uint32_t mask = (ctx->xra && bid < ctx->xra->nblk) ? ctx->xra->blk_gp_live[bid] : 0;
    int n = 0;
    /* Caller-saved: alloc indices 0..11 (a0-a7 + t0-t3) */
    for (int r = 0; r < RV64_NGPR_CALLER_SAVE; r++) {
        if ((mask & (1u << r)) && rv64_alloc_regs[r] != exclude)
            out[n++] = rv64_alloc_regs[r];
    }
    return n;
}

/* Collect live caller-saved FP regs in current block. */
XR_FUNC int rv64_live_fp(Rv64CodegenCtx *ctx, Rv64Freg *out) {
    XR_DCHECK(ctx != NULL, "rv64_live_fp: NULL ctx");
    uint32_t bid = ctx->cur_blk_id;
    uint32_t mask = (ctx->xra && bid < ctx->xra->nblk) ? ctx->xra->blk_fp_live[bid] : 0;
    int n = 0;
    for (int r = 0; r < RV64_MAX_FP_REGS; r++) {
        if (mask & (1u << r))
            out[n++] = rv64_alloc_fp_regs[r];
    }
    return n;
}

/* ========== Epilogue (shared entry for deopt) ========== */

/* Re-export epilogue emitter for use by deopt stub.
 * Defined in xm_codegen_riscv64.c, declared in internal header. */

#endif /* __riscv */
