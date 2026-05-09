/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_x64_osr.c - OSR entry stubs and resume entry (x86-64)
 *
 * Split from xm_codegen_x64.c for modularity.
 * Contains: OSR entry stub emission and JIT suspend/resume entry.
 */

#if defined(__x86_64__) || defined(_M_X64)

#include "xm_codegen_x64_internal.h"
#include "xm_offsets.h"
#include "xm_jit_runtime.h"
#include "../coro/xcoroutine.h" /* XM_SUSPEND_SPILL_MAX */

/* ========== OSR Entry Stubs ========== */

/* Skip vregs that the OSR stub should NOT pre-load:
 *   1. Vregs defined by an instruction inside the loop header itself
 *      (will be assigned by that instruction; loading would be redundant
 *      or wrong).
 *   2. PHI inputs that have been coalesced to the PHI dst's register
 *      (loading would clobber the PHI dst's interpreter-side value).
 *
 * Mirrors osr_should_skip_vreg() in xm_codegen.c. */
static bool x64_osr_should_skip_vreg(X64CodegenCtx *ctx, XmBlock *osr_blk, uint32_t v, int8_t ri) {
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
static void x64_osr_materialize_const(X64CodegenCtx *ctx, XmIns *def, int8_t phys_gp,
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
            x64_load_imm64(&ctx->buf, x64_alloc_regs[phys_gp], val);
        else if (phys_fp >= 0) {
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, val);
            x64_movq_xmm_gp(&ctx->buf, x64_alloc_fp_regs[phys_fp], X64_SCRATCH_REG);
        }
    } else if (def->op == XM_CONST_F64) {
        if (phys_fp >= 0) {
            x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, val);
            x64_movq_xmm_gp(&ctx->buf, x64_alloc_fp_regs[phys_fp], X64_SCRATCH_REG);
        } else if (phys_gp >= 0)
            x64_load_imm64(&ctx->buf, x64_alloc_regs[phys_gp], val);
    }
}

/* Emit OSR entry stubs: alternate function entries for loop headers.
 *
 * OSR calling convention: same as normal entry — RDI=coro, RSI=int64_t *values.
 * Each stub does:
 *   1. Standard prologue (frame setup, callee-saved push, jit_ctx load) —
 *      identical to fast prologue except param load is replaced with OSR
 *      value materialization.
 *   2. Save RSI (values pointer) into R11 because the load loop may
 *      overwrite RSI when a vreg is allocated to it.
 *   3. Pass 1: for every vreg with a phys reg AND a bc_slot, load
 *      values[bc_slot] into the phys reg using R11 as base.
 *   4. Pass 2: for vregs without bc_slot but with a compile-time const
 *      definition, materialize the constant into the phys reg. R11 may
 *      now be reused as scratch.
 *   5. JMP rel32 to the loop header block. block_offsets is already
 *      populated, so the displacement is computed directly (no patch). */
XR_FUNC void x64_emit_osr_stubs(X64CodegenCtx *ctx, XmCodegenResult *result) {
    for (uint32_t i = 0; i < ctx->nosr_snap; i++) {
        uint32_t snap_block_id = ctx->osr_snaps[i].block_id;
        XmOsrEntry *entry = &result->osr_entries[result->nosr];

        entry->block_id = snap_block_id;
        if (snap_block_id < ctx->func->nblk)
            entry->bc_offset = ctx->func->blocks[snap_block_id]->bc_offset;
        else
            entry->bc_offset = 0;
        entry->entry_offset = ctx->buf.pos;

        /* === Standard prologue (mirrors x64_emit_fast_prologue) === */
        x64_push_r(&ctx->buf, X64_RBP);
        x64_mov_rr(&ctx->buf, X64_RBP, X64_RSP);

        CODEGEN_CHECK(ctx, ctx->nsub_patches < 16, "too many sub patches for OSR stub");
        x64_emit8(&ctx->buf, 0x48);
        x64_emit8(&ctx->buf, 0x81);
        x64_emit8(&ctx->buf, 0xEC);
        ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.pos;
        x64_emit32(&ctx->buf, X64_JIT_FRAME_BASE);

#ifdef _WIN32
        for (int xi = 0; xi < X64_CALLEE_XMM_COUNT; xi++) {
            int32_t xoff = -(int32_t) (X64_XMM_SAVE_OFFSET + xi * 8);
            x64_movsd_mr(&ctx->buf, X64_RBP, xoff, (X64Xmm) (6 + xi));
        }
#endif

        x64_push_r(&ctx->buf, X64_RBX);
#ifdef _WIN32
        x64_push_r(&ctx->buf, X64_RDI);
        x64_push_r(&ctx->buf, X64_RSI);
#endif
        x64_push_r(&ctx->buf, X64_R12);
        x64_push_r(&ctx->buf, X64_R13);
        x64_push_r(&ctx->buf, X64_R14);
        x64_push_r(&ctx->buf, X64_R15);

        /* Save coro and load jit_ctx — use platform ABI arg registers */
        x64_mov_rr(&ctx->buf, X64_CORO_REG, X64_ABI_ARG1);
        x64_mov_rm(&ctx->buf, X64_JIT_CTX_REG, X64_CORO_REG, (int32_t) XM_CORO_JIT_CTX_OFFSET);

        /* Save stack_map_ptr from jit_ctx into frame (RCX as temp) */
        x64_mov_rm(&ctx->buf, X64_RCX, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_OFFSET);
        x64_mov_mr(&ctx->buf, X64_RBP, -(int32_t) X64_FRAME_SMAP_PTR_OFFSET, X64_RCX);
        /* Initialize safepoint_id = UINT32_MAX */
        x64_load_imm64(&ctx->buf, X64_RCX, 0xFFFFFFFF);
        x64_mov_mr32(&ctx->buf, X64_RBP, -(int32_t) X64_FRAME_SMAP_ID_OFFSET, X64_RCX);

        /* Save values pointer (ABI_ARG2) into R11 — the vreg load loop may
         * overwrite the register that ABI_ARG2 maps to. */
        x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_ABI_ARG2);

        XmBlock *osr_blk =
            (snap_block_id < ctx->func->nblk) ? ctx->func->blocks[snap_block_id] : NULL;

        /* === Pass 0: spill-only live vreg initialization ===
         * vregs that have NO phys reg but DO have a spill slot AND a
         * bc_slot must be seeded from values[bc_slot] into their spill
         * slot, otherwise the first reload after OSR entry reads
         * uninitialized stack memory. Has to run before Pass 1 because
         * Pass 1 saturates the allocatable GP set; here alloc_regs[0]
         * (RAX) is still free as transit, and Pass 1 will simply
         * overwrite RAX with whatever vreg it actually owns — which is
         * safe because the spill slot is already in memory by then.
         * 8-byte mov works for both i64 and f64 bit patterns. */
        for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
            if (xra_vreg_reg_at(ctx->xra, snap_block_id, v) >= 0)
                continue;
            int16_t spill = xra_vreg_spill(ctx->xra, v);
            if (spill < 0)
                continue;
            int16_t bc_slot = ctx->func->vregs[v].bc_slot;
            if (bc_slot < 0)
                continue;
            /* ri=-1 disables phi-coalesce check; only "defined in osr_blk". */
            if (x64_osr_should_skip_vreg(ctx, osr_blk, v, -1))
                continue;
            int32_t bc_off = (int32_t) ((uint32_t) bc_slot * 8);
            int32_t spill_off = -(X64_SPILL_BASE + (int32_t) spill * 8);
            X64Reg transit = x64_alloc_regs[0]; /* RAX */
            x64_mov_rm(&ctx->buf, transit, X64_SCRATCH_REG, bc_off);
            x64_mov_mr(&ctx->buf, X64_RBP, spill_off, transit);
        }

        /* === Pass 1: load vregs with bc_slot from values[] ===
         * R11 (SCRATCH_REG) holds values_ptr throughout this loop. */
        uint16_t nslots = 0;

        for (uint32_t v = 0; v < ctx->func->nvreg && nslots < XM_MAX_OSR_SLOTS; v++) {
            int8_t ri = xra_vreg_reg_at(ctx->xra, snap_block_id, v);
            if (ri < 0)
                continue;
            int16_t slot = ctx->func->vregs[v].bc_slot;
            if (slot < 0)
                continue;
            if (x64_osr_should_skip_vreg(ctx, osr_blk, v, ri))
                continue;
            bool is_fp = (ctx->func->vregs[v].rep == XR_REP_F64);
            int32_t off = (int32_t) ((uint32_t) slot * 8);
            if (!is_fp) {
                X64Reg dst = x64_alloc_regs[ri];
                x64_mov_rm(&ctx->buf, dst, X64_SCRATCH_REG, off);
                entry->slots[nslots].bc_slot = slot;
                entry->slots[nslots].phys_reg = (uint8_t) dst;
                entry->slots[nslots].type = XR_REP_I64;
                nslots++;
            } else {
                X64Xmm dst = x64_alloc_fp_regs[ri];
                x64_movsd_rm(&ctx->buf, dst, X64_SCRATCH_REG, off);
                entry->slots[nslots].bc_slot = slot;
                entry->slots[nslots].phys_reg = (uint8_t) dst;
                entry->slots[nslots].type = XR_REP_F64;
                nslots++;
            }
        }

        /* === Pass 2: materialize compile-time constants for vregs with
         * no bc_slot. R11 (values_ptr) is no longer needed and may be
         * reused as scratch by the const materializer. */
        for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
            int8_t ri = xra_vreg_reg_at(ctx->xra, snap_block_id, v);
            if (ri < 0)
                continue;
            int16_t slot = ctx->func->vregs[v].bc_slot;
            if (slot >= 0)
                continue;
            bool is_fp = (ctx->func->vregs[v].rep == XR_REP_F64);
            x64_osr_materialize_const(ctx, ctx->func->vregs[v].def, is_fp ? -1 : ri,
                                      is_fp ? ri : -1);
        }

        entry->nslots = nslots;

        /* JMP rel32 to loop header block. block_offsets is populated by
         * x64_emit_block earlier, so the displacement is known here. */
        uint32_t target =
            (snap_block_id < ctx->nblock_offsets) ? ctx->block_offsets[snap_block_id] : 0;
        x64_emit8(&ctx->buf, 0xE9);
        uint32_t rel_origin = ctx->buf.pos + 4;
        int32_t rel = (int32_t) target - (int32_t) rel_origin;
        x64_emit32(&ctx->buf, (uint32_t) rel);

        result->nosr++;
    }
}

/* ========== Resume Entry Stub (JIT Suspend/Resume) ========== */

/* Emit resume entry stub for suspended coroutines. When a coroutine is
 * JIT-suspended (XM_SUSPEND returned SUSPEND_MARKER), the worker calls
 * this entry point to re-enter JIT code. The stub:
 *   - Builds a new stack frame (identical to normal entry)
 *   - Reloads saved registers from coro->jit_suspend
 *   - Restores spill slots
 *   - Dispatches to the correct continuation point by suspend_id */
XR_FUNC void x64_emit_resume_entry(X64CodegenCtx *ctx, XmCodegenResult *result) {
    if (ctx->nsuspend == 0)
        return;
    ctx->resume_entry_offset = ctx->buf.pos;

    /* === Prologue (identical frame layout to normal entry) === */
    /* PUSH RBP; MOV RBP, RSP */
    x64_push_r(&ctx->buf, X64_RBP);
    x64_mov_rr(&ctx->buf, X64_RBP, X64_RSP);
    /* SUB RSP, frame_size (placeholder, patched later) */
    CODEGEN_CHECK(ctx, ctx->nsub_patches < 16, "too many sub patches for resume entry");
    x64_emit8(&ctx->buf, 0x48);
    x64_emit8(&ctx->buf, 0x81);
    x64_emit8(&ctx->buf, 0xEC);
    ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.pos;
    x64_emit32(&ctx->buf, X64_JIT_FRAME_BASE);

#ifdef _WIN32
    for (int xi = 0; xi < X64_CALLEE_XMM_COUNT; xi++) {
        int32_t xoff = -(int32_t) (X64_XMM_SAVE_OFFSET + xi * 8);
        x64_movsd_mr(&ctx->buf, X64_RBP, xoff, (X64Xmm) (6 + xi));
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

    /* Setup CORO_REG and JIT_CTX_REG — use platform ABI arg register */
    x64_mov_rr(&ctx->buf, X64_CORO_REG, X64_ABI_ARG1);
    x64_mov_rm(&ctx->buf, X64_JIT_CTX_REG, X64_CORO_REG, (int32_t) XM_CORO_JIT_CTX_OFFSET);

    /* Save JIT frame SP for GC */
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_FRAME_SP_OFFSET, X64_RBP);
    /* Save stack_map_ptr from jit_ctx into frame */
    x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_OFFSET);
    x64_mov_mr(&ctx->buf, X64_RBP, -(int32_t) X64_FRAME_SMAP_PTR_OFFSET, X64_SCRATCH_REG);
    /* Initialize smap_id to UINT32_MAX (invalid) */
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, 0xFFFFFFFF);
    x64_mov_mr32(&ctx->buf, X64_RBP, -(int32_t) X64_FRAME_SMAP_ID_OFFSET, X64_SCRATCH_REG);
    x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_ACTIVE_SMAP_ID_OFFSET,
                 X64_SCRATCH_REG);

    /* === Save suspend_id to stack (before we clobber RCX with reg restore) === */
    x64_mov_rm32(&ctx->buf, X64_RCX, X64_CORO_REG, (int32_t) XM_CORO_SUSPEND_ID_OFFSET);
    x64_push_r(&ctx->buf, X64_RCX);
    CODEGEN_CHECK(ctx, ctx->nsuspend <= 16, "too many suspend points");

    /* === Load suspend_state pointer into R11 === */
    x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_CORO_REG, (int32_t) XM_CORO_SUSPEND_PTR_OFFSET);

    /* === Restore spill slots FIRST (RCX available as temp) === */
    {
        uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
        if (ns > XM_SUSPEND_SPILL_MAX)
            ns = XM_SUSPEND_SPILL_MAX;
        for (uint32_t s = 0; s < ns; s++) {
            int32_t regs_off = (int32_t) (XM_SUSPEND_SPILL_OFF + s * 8);
            int32_t frame_off = -(int32_t) (X64_SPILL_BASE + s * 8);
            x64_mov_rm(&ctx->buf, X64_RCX, X64_SCRATCH_REG, regs_off);
            x64_mov_mr(&ctx->buf, X64_RBP, frame_off, X64_RCX);
        }
    }

    /* === Reload ALL allocatable registers === */
    /* Caller-saved regs from caller_saved[] */
    for (int i = 0; i < X64_NGPR_CALLER_SAVE && i < X64_MAX_PHYS_REGS; i++)
        x64_mov_rm(&ctx->buf, x64_alloc_regs[i], X64_SCRATCH_REG, i * 8);

    /* Callee-saved allocatable regs from callee_saved[] */
    for (int i = 0; i < X64_NGPR_CALLEE_SAVE_ALLOC; i++)
        x64_mov_rm(&ctx->buf, x64_alloc_regs[X64_NGPR_CALLER_SAVE + i], X64_SCRATCH_REG,
                   (int32_t) XM_SUSPEND_CALLEE_SAVED_OFF + i * 8);

    /* === Clear jit_resume_entry (one-shot: prevent double-resume) === */
    x64_xor_rr(&ctx->buf, X64_SCRATCH_REG, X64_SCRATCH_REG);
    x64_mov_mr(&ctx->buf, X64_CORO_REG, (int32_t) XM_CORO_RESUME_ENTRY_OFFSET, X64_SCRATCH_REG);

    /* === Pop suspend_id into R11 (non-allocatable scratch) for dispatch === */
    x64_pop_r(&ctx->buf, X64_SCRATCH_REG);

    /* === Per-suspend-id dispatch: CMP + JE chain === */
    uint32_t trampoline_patches[16];
    for (uint32_t i = 0; i < ctx->nsuspend; i++) {
        /* CMP R11d, i */
        x64_cmp_ri(&ctx->buf, X64_SCRATCH_REG, (int32_t) i);
        /* JE trampoline_i (placeholder) */
        x64_emit8(&ctx->buf, 0x0F);
        x64_emit8(&ctx->buf, (uint8_t) (0x80 | X64_CC_E));
        trampoline_patches[i] = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);
    }

    /* Fallback: should not reach here. Return DEOPT_MARKER. */
    x64_load_imm64(&ctx->buf, X64_RAX, (uint64_t) XM_DEOPT_MARKER);
    x64_emit_epilogue(ctx);

    /* Per-suspend trampolines: load result + JMP to continuation */
    for (uint32_t i = 0; i < ctx->nsuspend; i++) {
        x64_patch_rel32(&ctx->buf, trampoline_patches[i], ctx->buf.pos);

        /* Reload suspend pointer for result access */
        x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_CORO_REG, (int32_t) XM_CORO_SUSPEND_PTR_OFFSET);

        /* Load result into the correct register */
        X64Reg result_rd = (X64Reg) ctx->suspend_result_regs[i];
        if (result_rd != X64_SCRATCH_REG) {
            x64_mov_rm(&ctx->buf, result_rd, X64_SCRATCH_REG, (int32_t) XM_SUSPEND_RESULT_OFF);
        }

        /* Load result_tag → vreg_runtime_tags[vi] */
        int32_t tag_off = ctx->suspend_result_tag_offs[i];
        if (tag_off >= 0) {
            x64_movzx_rm8(&ctx->buf, X64_RCX, X64_SCRATCH_REG, (int32_t) XM_SUSPEND_RESULT_TAG_OFF);
            x64_mov_mr8(&ctx->buf, X64_JIT_CTX_REG, tag_off, X64_RCX);
        }

        /* JMP to continuation point in main function code */
        uint32_t cont = ctx->suspend_cont_offsets[i];
        x64_emit8(&ctx->buf, 0xE9);
        uint32_t jmp_pos = ctx->buf.pos;
        x64_emit32(&ctx->buf, 0);
        x64_patch_rel32(&ctx->buf, jmp_pos, cont);
    }

    result->resume_entry_offset = ctx->resume_entry_offset;
}

#endif  // __x86_64__ || _M_X64
