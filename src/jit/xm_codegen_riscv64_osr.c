/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_riscv64_osr.c - OSR entry stubs and resume entry (RISC-V 64)
 *
 * Split from xm_codegen_riscv64.c for modularity.
 * Contains: OSR entry stub emission and JIT suspend/resume entry.
 */

#ifdef __riscv

#include "xm_codegen_riscv64_internal.h"
#include "xm_offsets.h"
#include "xm_jit_runtime.h"
#include "xjit_coro_state.h" /* XM_SUSPEND_SPILL_MAX */

/* ========== OSR Entry Stubs ========== */

/* Skip vregs that the OSR stub should NOT pre-load:
 *   1. Vregs defined by an instruction inside the loop header itself
 *      (will be assigned by that instruction; loading would be redundant
 *      or wrong).
 *   2. PHI inputs that have been coalesced to the PHI dst's register
 *      (loading would clobber the PHI dst's interpreter-side value).
 *
 * Mirrors the same logic used by x64/ARM64 OSR stubs. */
static bool rv64_osr_should_skip_vreg(Rv64CodegenCtx *ctx, XmBlock *osr_blk, uint32_t v,
                                      int8_t ri) {
    if (!osr_blk)
        return false;
    XmRef vref = XM_REF(XM_REF_VREG, v);
    for (uint32_t ii = 0; ii < osr_blk->nins; ii++) {
        if (osr_blk->ins[ii].dst == vref)
            return true;
    }
    for (XmPhi *phi = osr_blk->phis; phi; phi = phi->next) {
        for (uint16_t ai = 0; ai < phi->narg; ai++) {
            if (phi->args[ai] == vref) {
                uint32_t pdv = XM_REF_INDEX(phi->dst);
                int8_t pd_ri = xra_vreg_reg_at(ctx->xra, osr_blk->id, pdv);
                if (pd_ri >= 0 && pd_ri == ri)
                    return true;
                break;
            }
        }
    }
    return false;
}

/* Materialize a compile-time constant directly into a phys reg for OSR.
 * Does not require the values_ptr scratch — safe to invoke after the
 * primary load loop. */
static void rv64_osr_materialize_const(Rv64CodegenCtx *ctx, XmIns *def, int8_t phys_gp,
                                       int8_t phys_fp) {
    if (!def)
        return;
    if (!xm_ref_is_const(def->args[0]))
        return;
    uint32_t ci = XM_REF_INDEX(def->args[0]);
    if (ci >= ctx->func->nconst)
        return;
    uint64_t val = ctx->func->consts[ci].val.raw;

    if (def->op == XM_CONST_I64 || def->op == XM_CONST_PTR) {
        if (phys_gp >= 0)
            rv64_load_imm64(&ctx->buf, rv64_alloc_regs[phys_gp], val);
        else if (phys_fp >= 0) {
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, val);
            rv64_buf_emit(&ctx->buf, rv64_fmv_d_x(rv64_alloc_fp_regs[phys_fp], RV64_SCRATCH_REG));
        }
    } else if (def->op == XM_CONST_F64) {
        if (phys_fp >= 0) {
            rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, val);
            rv64_buf_emit(&ctx->buf, rv64_fmv_d_x(rv64_alloc_fp_regs[phys_fp], RV64_SCRATCH_REG));
        } else if (phys_gp >= 0)
            rv64_load_imm64(&ctx->buf, rv64_alloc_regs[phys_gp], val);
    }
}

/* Emit OSR entry stubs: alternate function entries for loop headers.
 *
 * OSR calling convention: same as normal entry — a0=coro, a1=int64_t *values.
 * Each stub does:
 *   1. Standard prologue (frame setup, callee-saved push, jit_ctx load)
 *   2. Save a1 (values pointer) into t6 (scratch) because the load loop may
 *      overwrite a1 when a vreg is allocated to it.
 *   3. Pass 0: spill-only vregs (no phys reg but have spill + bc_slot)
 *   4. Pass 1: load vregs with bc_slot from values[] into phys regs
 *   5. Pass 2: materialize compile-time constants for vregs without bc_slot
 *   6. JAL x0 (J-type) to the loop header block */
XR_FUNC void rv64_emit_osr_stubs(Rv64CodegenCtx *ctx, XmCodegenResult *result) {
    for (uint32_t i = 0; i < ctx->nosr_snap; i++) {
        uint32_t snap_block_id = ctx->osr_snaps[i].block_id;
        XmOsrEntry *entry = &result->osr_entries[result->nosr];

        entry->block_id = snap_block_id;
        if (snap_block_id < ctx->func->nblk)
            entry->bc_offset = ctx->func->blocks[snap_block_id]->bc_offset;
        else
            entry->bc_offset = 0;
        entry->entry_offset = rv64_buf_offset(&ctx->buf);

        /* === Standard prologue (mirrors rv64_emit_prologue) === */

        /* ADDI sp, sp, -frame_size (placeholder — patched later) */
        RV64_CODEGEN_CHECK(ctx, ctx->nsub_patches < 16, "too many frame sub patches for OSR stub");
        ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.count;
        rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, -(int32_t) RV64_JIT_FRAME_BASE));

        rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, RV64_FP));

        /* Set up frame pointer (placeholder — patched later) */
        RV64_CODEGEN_CHECK(ctx, ctx->nadd_patches < 8, "too many frame add patches for OSR stub");
        ctx->frame_patch_add[ctx->nadd_patches++] = ctx->buf.count;
        rv64_buf_emit(&ctx->buf, rv64_addi(RV64_FP, RV64_SP, (int32_t) RV64_JIT_FRAME_BASE));

        /* Save ra, callee-saved GPRs (same layout as normal prologue) */
        uint32_t off = RV64_CALLEE_SAVE_BASE_OFFSET;
        rv64_buf_emit(&ctx->buf, rv64_sd(RV64_RA, RV64_FP, -(int32_t) off));
        off += 8;
        rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_FP, -(int32_t) off));
        off += 8;
        rv64_buf_emit(&ctx->buf, rv64_sd(RV64_S1, RV64_FP, -(int32_t) off));
        off += 8;
        for (Rv64Reg r = RV64_S2; r <= RV64_S9; r++) {
            rv64_buf_emit(&ctx->buf, rv64_sd(r, RV64_FP, -(int32_t) off));
            off += 8;
        }
        rv64_buf_emit(&ctx->buf, rv64_sd(RV64_S10, RV64_FP, -(int32_t) off));
        off += 8;
        rv64_buf_emit(&ctx->buf, rv64_sd(RV64_S11, RV64_FP, -(int32_t) off));
        off += 8;
        /* FP callee-saved: fs0-fs7 */
        for (Rv64Freg f = RV64_FS0; f <= RV64_FS7; f++) {
            rv64_buf_emit(&ctx->buf, rv64_fsd(f, RV64_FP, -(int32_t) off));
            off += 8;
        }

        /* Copy coro pointer from a0 and load jit_ctx */
        rv64_buf_emit(&ctx->buf, rv64_mv(RV64_CORO_REG, RV64_A0));
        rv64_emit_load_jit_state(ctx, RV64_JIT_CTX_REG);
        rv64_buf_emit(&ctx->buf, rv64_ld(RV64_JIT_CTX_REG, RV64_JIT_CTX_REG,
                                         (int32_t) XM_JIT_STATE_SCRATCH_OFFSET));

        /* Save stack_map_ptr from jit_ctx into frame */
        rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                         (int32_t) XM_JIT_ACTIVE_SMAP_OFFSET));
        rv64_buf_emit(&ctx->buf,
                      rv64_sd(RV64_SCRATCH_REG, RV64_FP, -(int32_t) RV64_FRAME_SMAP_PTR_OFFSET));

        /* Initialize safepoint_id = UINT32_MAX */
        rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, 0xFFFFFFFF);
        rv64_buf_emit(&ctx->buf,
                      rv64_sw(RV64_SCRATCH_REG, RV64_FP, -(int32_t) RV64_FRAME_SMAP_ID_OFFSET));

        /* Save values pointer (a1) into t6 (scratch) — the vreg load loop
         * may overwrite the register that a1 maps to. */
        rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, RV64_A1));

        XmBlock *osr_blk =
            (snap_block_id < ctx->func->nblk) ? ctx->func->blocks[snap_block_id] : NULL;

        /* === Pass 0: spill-only live vreg initialization ===
         * Vregs with no phys reg but a spill slot AND a bc_slot must be
         * seeded from values[bc_slot] into their spill slot. Uses a0 as
         * transit (will be overwritten by Pass 1). */
        for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
            if (xra_vreg_reg_at(ctx->xra, snap_block_id, v) >= 0)
                continue;
            int16_t spill = xra_vreg_spill(ctx->xra, v);
            if (spill < 0)
                continue;
            int16_t bc_slot = ctx->func->vregs[v].bc_slot;
            if (bc_slot < 0)
                continue;
            if (rv64_osr_should_skip_vreg(ctx, osr_blk, v, -1))
                continue;
            int32_t bc_off = (int32_t) ((uint32_t) bc_slot * 8);
            int32_t spill_off = -(int32_t) (RV64_SPILL_BASE + (int32_t) spill * 8);
            XR_DCHECK(spill_off >= -2048, "OSR spill-only: spill offset OOB");
            rv64_buf_emit(&ctx->buf, rv64_ld(RV64_A0, RV64_SCRATCH_REG, bc_off));
            rv64_buf_emit(&ctx->buf, rv64_sd(RV64_A0, RV64_FP, spill_off));
        }

        /* === Pass 1: load vregs with bc_slot from values[] ===
         * t6 (SCRATCH_REG) holds values_ptr throughout this loop. */
        uint16_t nslots = 0;

        for (uint32_t v = 0; v < ctx->func->nvreg && nslots < XM_MAX_OSR_SLOTS; v++) {
            int8_t ri = xra_vreg_reg_at(ctx->xra, snap_block_id, v);
            if (ri < 0)
                continue;
            int16_t slot = ctx->func->vregs[v].bc_slot;
            if (slot < 0)
                continue;
            if (rv64_osr_should_skip_vreg(ctx, osr_blk, v, ri))
                continue;
            bool is_fp = (ctx->func->vregs[v].rep == XR_REP_F64);
            int32_t val_off = (int32_t) ((uint32_t) slot * 8);
            if (!is_fp) {
                Rv64Reg dst = rv64_alloc_regs[ri];
                rv64_buf_emit(&ctx->buf, rv64_ld(dst, RV64_SCRATCH_REG, val_off));
                entry->slots[nslots].bc_slot = slot;
                entry->slots[nslots].phys_reg = (uint8_t) dst;
                entry->slots[nslots].type = XR_REP_I64;
                nslots++;
            } else {
                Rv64Freg dst = rv64_alloc_fp_regs[ri];
                rv64_buf_emit(&ctx->buf, rv64_fld(dst, RV64_SCRATCH_REG, val_off));
                entry->slots[nslots].bc_slot = slot;
                entry->slots[nslots].phys_reg = (uint8_t) dst;
                entry->slots[nslots].type = XR_REP_F64;
                nslots++;
            }
        }

        /* === Pass 2: materialize compile-time constants for vregs
         * without bc_slot. t6 (values_ptr) no longer needed. */
        for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
            int8_t ri = xra_vreg_reg_at(ctx->xra, snap_block_id, v);
            if (ri < 0)
                continue;
            int16_t slot = ctx->func->vregs[v].bc_slot;
            if (slot >= 0)
                continue;
            bool is_fp = (ctx->func->vregs[v].rep == XR_REP_F64);
            rv64_osr_materialize_const(ctx, ctx->func->vregs[v].def, is_fp ? -1 : ri,
                                       is_fp ? ri : -1);
        }

        entry->nslots = nslots;

        /* J (JAL x0) to loop header block. block_offsets is populated by
         * rv64_emit_block earlier, so the displacement is known here.
         * byte_offset = (target_inst_idx - this_inst_idx) * 4. */
        uint32_t target_idx =
            (snap_block_id < ctx->nblock_offsets) ? ctx->block_offsets[snap_block_id] : 0;
        uint32_t jmp_idx = ctx->buf.count;
        int32_t byte_offset = (int32_t) (target_idx - jmp_idx) * 4;
        rv64_buf_emit(&ctx->buf, rv64_j(byte_offset));

        result->nosr++;
    }
}

/* ========== Resume Entry Stub (JIT Suspend/Resume) ========== */

/* Emit resume entry stub for suspended coroutines. When a coroutine is
 * JIT-suspended (XM_SUSPEND returned SUSPEND_MARKER), the worker calls
 * this entry point to re-enter JIT code. The stub:
 *   - Builds a new stack frame (identical to normal entry)
 *   - Reloads saved registers from xr_coro_jit_state(coro)->suspend
 *   - Restores spill slots
 *   - Dispatches to the correct continuation point by suspend_id */
XR_FUNC void rv64_emit_resume_entry(Rv64CodegenCtx *ctx, XmCodegenResult *result) {
    if (ctx->nsuspend == 0)
        return;
    ctx->resume_entry_offset = rv64_buf_offset(&ctx->buf);

    /* === Prologue (identical frame layout to normal entry) === */

    /* ADDI sp, sp, -frame_size (placeholder — patched later) */
    RV64_CODEGEN_CHECK(ctx, ctx->nsub_patches < 16, "too many frame sub patches for resume entry");
    ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_SP, RV64_SP, -(int32_t) RV64_JIT_FRAME_BASE));

    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_SCRATCH_REG, RV64_FP));

    /* Frame pointer setup (placeholder — patched later) */
    RV64_CODEGEN_CHECK(ctx, ctx->nadd_patches < 8, "too many frame add patches for resume entry");
    ctx->frame_patch_add[ctx->nadd_patches++] = ctx->buf.count;
    rv64_buf_emit(&ctx->buf, rv64_addi(RV64_FP, RV64_SP, (int32_t) RV64_JIT_FRAME_BASE));

    /* Save ra, callee-saved GPRs */
    uint32_t off = RV64_CALLEE_SAVE_BASE_OFFSET;
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_RA, RV64_FP, -(int32_t) off));
    off += 8;
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_SCRATCH_REG, RV64_FP, -(int32_t) off));
    off += 8;
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_S1, RV64_FP, -(int32_t) off));
    off += 8;
    for (Rv64Reg r = RV64_S2; r <= RV64_S9; r++) {
        rv64_buf_emit(&ctx->buf, rv64_sd(r, RV64_FP, -(int32_t) off));
        off += 8;
    }
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_S10, RV64_FP, -(int32_t) off));
    off += 8;
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_S11, RV64_FP, -(int32_t) off));
    off += 8;
    for (Rv64Freg f = RV64_FS0; f <= RV64_FS7; f++) {
        rv64_buf_emit(&ctx->buf, rv64_fsd(f, RV64_FP, -(int32_t) off));
        off += 8;
    }

    /* Setup CORO_REG and JIT_CTX_REG */
    rv64_buf_emit(&ctx->buf, rv64_mv(RV64_CORO_REG, RV64_A0));
    rv64_emit_load_jit_state(ctx, RV64_JIT_CTX_REG);
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_JIT_CTX_REG, RV64_JIT_CTX_REG,
                                     (int32_t) XM_JIT_STATE_SCRATCH_OFFSET));

    /* Save JIT frame SP for GC */
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_FP, RV64_JIT_CTX_REG, (int32_t) XM_JIT_FRAME_SP_OFFSET));

    /* Save stack_map_ptr from jit_ctx into frame */
    rv64_buf_emit(&ctx->buf,
                  rv64_ld(RV64_SCRATCH_REG, RV64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_OFFSET));
    rv64_buf_emit(&ctx->buf,
                  rv64_sd(RV64_SCRATCH_REG, RV64_FP, -(int32_t) RV64_FRAME_SMAP_PTR_OFFSET));

    /* Initialize smap_id to UINT32_MAX (invalid) */
    rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, 0xFFFFFFFF);
    rv64_buf_emit(&ctx->buf,
                  rv64_sw(RV64_SCRATCH_REG, RV64_FP, -(int32_t) RV64_FRAME_SMAP_ID_OFFSET));
    rv64_buf_emit(&ctx->buf, rv64_sw(RV64_SCRATCH_REG, RV64_JIT_CTX_REG,
                                     (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET));

    /* === Load suspend_id into t5 (SCRATCH_REG2) for later dispatch === */
    rv64_emit_load_jit_state(ctx, RV64_SCRATCH_REG);
    rv64_buf_emit(&ctx->buf, rv64_lw(RV64_SCRATCH_REG2, RV64_SCRATCH_REG,
                                     (int32_t) XM_JIT_STATE_SUSPEND_ID_OFFSET));

    /* === Load suspend_state pointer into t6 (SCRATCH_REG) === */
    rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_SCRATCH_REG,
                                     (int32_t) XM_JIT_STATE_SUSPEND_PTR_OFFSET));

    /* === Restore spill slots FIRST (a0 available as temp) === */
    {
        uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
        if (ns > XM_SUSPEND_SPILL_MAX)
            ns = XM_SUSPEND_SPILL_MAX;
        for (uint32_t s = 0; s < ns; s++) {
            int32_t regs_off = (int32_t) (XM_SUSPEND_SPILL_OFF + s * 8);
            int32_t frame_off = -(int32_t) (RV64_SPILL_BASE + s * 8);
            XR_DCHECK(frame_off >= -2048, "resume: spill frame offset OOB");
            rv64_buf_emit(&ctx->buf, rv64_ld(RV64_A0, RV64_SCRATCH_REG, regs_off));
            rv64_buf_emit(&ctx->buf, rv64_sd(RV64_A0, RV64_FP, frame_off));
        }
    }

    /* === Reload ALL allocatable registers ===
     * Caller-saved regs from caller_saved[] */
    for (int i = 0; i < RV64_NGPR_CALLER_SAVE && i < RV64_MAX_PHYS_REGS; i++)
        rv64_buf_emit(&ctx->buf, rv64_ld(rv64_alloc_regs[i], RV64_SCRATCH_REG,
                                         (int32_t) XM_SUSPEND_CALLER_SAVED_OFF + i * 8));

    /* Callee-saved allocatable regs from callee_saved[] */
    for (int i = 0; i < RV64_NGPR_CALLEE_SAVE_ALLOC; i++)
        rv64_buf_emit(&ctx->buf,
                      rv64_ld(rv64_alloc_regs[RV64_NGPR_CALLER_SAVE + i], RV64_SCRATCH_REG,
                              (int32_t) XM_SUSPEND_CALLEE_SAVED_OFF + i * 8));

    /* === Clear jit_resume_entry (one-shot: prevent double-resume) === */
    rv64_emit_load_jit_state(ctx, RV64_A0);
    rv64_buf_emit(&ctx->buf, rv64_sd(RV64_X0, RV64_A0, (int32_t) XM_JIT_STATE_RESUME_ENTRY_OFFSET));

    /* === Per-suspend-id dispatch: compare + branch chain ===
     * t5 (SCRATCH_REG2) holds suspend_id. */
    RV64_CODEGEN_CHECK(ctx, ctx->nsuspend <= XM_MAX_SUSPEND_ENTRIES, "too many suspend points");

    uint32_t trampoline_patches[XM_MAX_SUSPEND_ENTRIES];
    for (uint32_t i = 0; i < ctx->nsuspend; i++) {
        /* LI t6, i — load immediate for comparison */
        rv64_load_imm64(&ctx->buf, RV64_SCRATCH_REG, (uint64_t) i);
        /* BEQ t5, t6, trampoline_i (placeholder) */
        trampoline_patches[i] = ctx->buf.count;
        rv64_buf_emit(&ctx->buf, rv64_beq(RV64_SCRATCH_REG2, RV64_SCRATCH_REG, 0));
    }

    /* Fallback: should not reach here. Return DEOPT_MARKER. */
    rv64_load_imm64(&ctx->buf, RV64_A0, (uint64_t) XM_DEOPT_MARKER);
    rv64_emit_epilogue(ctx);

    /* Per-suspend trampolines: load result + J to continuation */
    for (uint32_t i = 0; i < ctx->nsuspend; i++) {
        /* Patch BEQ to target this trampoline */
        int32_t patch_off = (int32_t) (ctx->buf.count - trampoline_patches[i]) * 4;
        ctx->buf.code[trampoline_patches[i]] =
            rv64_beq(RV64_SCRATCH_REG2, RV64_SCRATCH_REG, patch_off);

        /* Reload suspend pointer for result access */
        rv64_emit_load_jit_state(ctx, RV64_SCRATCH_REG);
        rv64_buf_emit(&ctx->buf, rv64_ld(RV64_SCRATCH_REG, RV64_SCRATCH_REG,
                                         (int32_t) XM_JIT_STATE_SUSPEND_PTR_OFFSET));

        /* Load result into the correct register */
        Rv64Reg result_rd = (Rv64Reg) ctx->suspend_result_regs[i];
        if (result_rd != RV64_SCRATCH_REG) {
            rv64_buf_emit(&ctx->buf,
                          rv64_ld(result_rd, RV64_SCRATCH_REG, (int32_t) XM_SUSPEND_RESULT_OFF));
        }

        /* Load result_tag -> vreg_runtime_tags[vi] */
        int32_t tag_off = ctx->suspend_result_tag_offs[i];
        if (tag_off >= 0) {
            rv64_buf_emit(&ctx->buf,
                          rv64_lbu(RV64_A0, RV64_SCRATCH_REG, (int32_t) XM_SUSPEND_RESULT_TAG_OFF));
            rv64_buf_emit(&ctx->buf, rv64_sb(RV64_A0, RV64_JIT_CTX_REG, tag_off));
        }

        /* J (JAL x0) to continuation point in main function code.
         * suspend_cont_offsets[] stores byte offsets; convert to
         * instruction index for displacement computation. */
        uint32_t cont_byte = ctx->suspend_cont_offsets[i];
        uint32_t cont_idx = cont_byte / 4;
        uint32_t jmp_idx = ctx->buf.count;
        int32_t byte_disp = (int32_t) (cont_idx - jmp_idx) * 4;
        rv64_buf_emit(&ctx->buf, rv64_j(byte_disp));
    }

    result->resume_entry_offset = ctx->resume_entry_offset;
}

#endif /* __riscv */
