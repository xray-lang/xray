/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_stub.c - ARM64 codegen: stubs, OSR, patching, deopt, resume
 *
 * Contains barrier/deopt/call_c stubs, OSR entry stubs,
 * branch patching, runtime deopt table, and suspend/resume.
 * Split from xm_codegen.c to keep each file under 3000 lines.
 */

#if defined(__aarch64__)

#include "xm_codegen_internal.h"
#include "../coro/xcoroutine.h"

/* ========== Write Barrier Stubs ========== */

XR_FUNC void a64_emit_barrier_stubs(CodegenCtx *ctx) {
    if (!ctx->has_barriers)
        return;

    // Forward barrier stub: xr_jit_barrier_fwd(coro, parent, child)
    // Caller has set x16=parent, x17=child before BL here.
    ctx->barrier_fwd_stub = ctx->buf.count;

    a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, 128));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X1, A64_X2, A64_SP, 0));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X3, A64_X4, A64_SP, 16));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X5, A64_X6, A64_SP, 32));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X7, A64_X8, A64_SP, 48));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X9, A64_X10, A64_SP, 64));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X11, A64_X12, A64_SP, 80));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X13, A64_X14, A64_SP, 96));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X15, A64_LR, A64_SP, 112));

    // Set up C call: x0=coro, x1=parent(x16), x2=child(x17)
    a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
    a64_buf_emit(&ctx->buf, a64_mov(A64_X1, SCRATCH_REG));
    a64_buf_emit(&ctx->buf, a64_mov(A64_X2, SCRATCH_REG2));
    a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_barrier_fwd);
    a64_buf_emit(&ctx->buf, a64_blr(SCRATCH_REG));

    a64_buf_emit(&ctx->buf, a64_ldp(A64_X1, A64_X2, A64_SP, 0));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X3, A64_X4, A64_SP, 16));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X5, A64_X6, A64_SP, 32));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X7, A64_X8, A64_SP, 48));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X9, A64_X10, A64_SP, 64));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X11, A64_X12, A64_SP, 80));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X13, A64_X14, A64_SP, 96));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X15, A64_LR, A64_SP, 112));
    a64_buf_emit(&ctx->buf, a64_add_imm(A64_SP, A64_SP, 128));
    a64_buf_emit(&ctx->buf, a64_ret());

    // Back barrier stub: xr_jit_barrier_back(coro, container)
    // Caller has set x16=container before BL here.
    ctx->barrier_back_stub = ctx->buf.count;

    a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, 128));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X1, A64_X2, A64_SP, 0));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X3, A64_X4, A64_SP, 16));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X5, A64_X6, A64_SP, 32));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X7, A64_X8, A64_SP, 48));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X9, A64_X10, A64_SP, 64));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X11, A64_X12, A64_SP, 80));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X13, A64_X14, A64_SP, 96));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X15, A64_LR, A64_SP, 112));

    // Set up C call: x0=coro, x1=container(x16)
    a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
    a64_buf_emit(&ctx->buf, a64_mov(A64_X1, SCRATCH_REG));
    a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_barrier_back);
    a64_buf_emit(&ctx->buf, a64_blr(SCRATCH_REG));

    a64_buf_emit(&ctx->buf, a64_ldp(A64_X1, A64_X2, A64_SP, 0));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X3, A64_X4, A64_SP, 16));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X5, A64_X6, A64_SP, 32));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X7, A64_X8, A64_SP, 48));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X9, A64_X10, A64_SP, 64));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X11, A64_X12, A64_SP, 80));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X13, A64_X14, A64_SP, 96));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X15, A64_LR, A64_SP, 112));
    a64_buf_emit(&ctx->buf, a64_add_imm(A64_SP, A64_SP, 128));
    a64_buf_emit(&ctx->buf, a64_ret());
}

/* ========== Deopt Exit Stub ========== */

XR_FUNC void a64_emit_deopt_stub(CodegenCtx *ctx) {
    if (!ctx->has_deopt)
        return;

    ctx->deopt_stub = ctx->buf.count;

    // Write deopt_id (in w17/SCRATCH_REG2) to jit_ctx->deopt_id
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_DEOPT_ID_OFFSET));

    // Save frame pointer for spill slot recovery
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XM_JIT_DEOPT_SPILL_BASE_OFFSET));

    // Save all allocatable GP registers to jit_ctx->deopt_regs[reg_num]
    // MUST happen BEFORE epilogue restores callee-saved registers.
    // x19 = CORO_REG (preserved), x28 = JIT_CTX_REG (preserved)
    int32_t gp_base = XM_JIT_DEOPT_REGS_OFFSET;
    for (int i = 0; i < MAX_PHYS_REGS; i++) {
        A64Reg r = alloc_regs[i];
        a64_buf_emit(&ctx->buf, a64_str(r, JIT_CTX_REG, gp_base + (int32_t) r * 8));
    }

    // Save FP registers d0-d15 to jit_ctx->deopt_fp_regs[d_num]
    int32_t fp_base = XM_JIT_DEOPT_FP_REGS_OFFSET;
    for (int i = 0; i < MAX_FP_REGS; i++) {
        a64_buf_emit(&ctx->buf, a64_str_fp(i, JIT_CTX_REG, fp_base + i * 8));
    }

    // Copy spill slots from frame to jit_ctx->deopt_spill_save[] BEFORE epilogue.
    // After epilogue the frame is deallocated and may be overwritten by C calls.
    // Uses x16 (SCRATCH_REG) as scratch — safe since all GP regs are already saved.
    {
        uint32_t nspill = ctx->xra ? ctx->xra->nspill : 0;
        if (nspill > 32)
            nspill = 32;
        int32_t save_base = (int32_t) XM_JIT_DEOPT_SPILL_SAVE_OFFSET;
        for (uint32_t s = 0; s < nspill; s++) {
            int32_t frame_off = SPILL_BASE + (int32_t) s * 8;
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, A64_SP, frame_off));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, JIT_CTX_REG, save_base + (int32_t) s * 8));
        }
    }

    // Load deopt marker into x0
    a64_load_imm64(&ctx->buf, A64_X0, (uint64_t) XM_DEOPT_MARKER);

    // Epilogue + return
    emit_epilogue(ctx);
    a64_buf_emit(&ctx->buf, a64_ret());
}

/* ========== CALL_C Stub ========== */

// Generic C call stub: caller sets x16=func_ptr, x17=extra_arg
// Stub: save x1-x15+LR, call func(coro, extra_arg), restore, return result in x0
XR_FUNC void a64_emit_call_c_stub(CodegenCtx *ctx) {
    if (!ctx->has_call_c)
        return;

    ctx->call_c_stub = ctx->buf.count;

    // Save caller-saved GP (x1-x15, LR) + FP (d0-d7) registers
    // GP: 16 regs * 8 = 128 bytes, FP: 8 regs * 8 = 64 bytes → 192 bytes total
    a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, 192));
    // GP registers at SP+0..127
    a64_buf_emit(&ctx->buf, a64_stp(A64_X1, A64_X2, A64_SP, 0));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X3, A64_X4, A64_SP, 16));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X5, A64_X6, A64_SP, 32));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X7, A64_X8, A64_SP, 48));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X9, A64_X10, A64_SP, 64));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X11, A64_X12, A64_SP, 80));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X13, A64_X14, A64_SP, 96));
    a64_buf_emit(&ctx->buf, a64_stp(A64_X15, A64_LR, A64_SP, 112));
    // FP caller-saved d0-d7 at SP+128..191
    for (int i = 0; i < 8; i++)
        a64_buf_emit(&ctx->buf, a64_str_fp(i, A64_SP, 128 + i * 8));

    // Save SP to jit_ctx for GC stack map access to saved registers
    a64_buf_emit(&ctx->buf, a64_str(A64_SP, JIT_CTX_REG, XM_JIT_SAFEPOINT_SAVED_SP_OFFSET));

    // Set up C call: x0=coro, x1=extra_arg(x17), func_ptr in x16
    a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
    a64_buf_emit(&ctx->buf, a64_mov(A64_X1, SCRATCH_REG2));
    a64_buf_emit(&ctx->buf, a64_blr(SCRATCH_REG));

    // Save payload and tag before GP restore:
    //   x16 (SCRATCH_REG) = payload (XrJitResult.payload)
    //   jit_ctx->call_result_tag = tag (XrJitResult.tag from x1)
    // Storing the tag via memory (not x17) avoids clobbering alloc_regs[0]=x1
    // on stub return, which would corrupt any vreg live in x1 across this call.
    a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, A64_X0));
    a64_buf_emit(&ctx->buf, a64_str(A64_X1, JIT_CTX_REG, XM_JIT_CALL_RESULT_TAG_OFFSET));

    // Restore FP registers d0-d7
    for (int i = 0; i < 8; i++)
        a64_buf_emit(&ctx->buf, a64_ldr_fp(i, A64_SP, 128 + i * 8));
    // Restore GP registers (x1-x15 restored; x16/x17 stay intact)
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X1, A64_X2, A64_SP, 0));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X3, A64_X4, A64_SP, 16));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X5, A64_X6, A64_SP, 32));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X7, A64_X8, A64_SP, 48));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X9, A64_X10, A64_SP, 64));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X11, A64_X12, A64_SP, 80));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X13, A64_X14, A64_SP, 96));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X15, A64_LR, A64_SP, 112));
    a64_buf_emit(&ctx->buf, a64_add_imm(A64_SP, A64_SP, 192));

    // Return x0=payload; x1 keeps the stack-restored original value.
    // Tag is in jit_ctx->call_result_tag for XM_CALL_C codegen to read.
    a64_buf_emit(&ctx->buf, a64_mov(A64_X0, SCRATCH_REG));
    a64_buf_emit(&ctx->buf, a64_ret());
}

/* ========== OSR Helpers ========== */

// Check if a vreg should be skipped during OSR entry loading.
// Skip vregs defined inside the loop header block or coalesced PHI inputs.
static bool osr_should_skip_vreg(CodegenCtx *ctx, XmBlock *osr_blk, uint32_t v, int8_t ri) {
    if (!osr_blk)
        return false;
    XmRef vref = XM_REF(XM_REF_VREG, v);
    // Skip vregs defined by instructions in this block
    for (uint32_t ii = 0; ii < osr_blk->nins; ii++) {
        if (osr_blk->ins[ii].dst == vref)
            return true;
    }
    // Skip PHI inputs only when coalesced with the PHI dst
    // (same physical register). Unconditionally skipping all PHI
    // inputs is too aggressive: a vreg may be a PHI input for one
    // slot but also a live through-value for another slot.
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

// Materialize a compile-time constant into a physical register for OSR.
static void osr_materialize_const(CodegenCtx *ctx, XmIns *def, int8_t phys, int8_t fp) {
    if (!def)
        return;
    if (def->op == XM_CONST_I64 && xm_ref_is_const(def->args[0])) {
        uint32_t ci = XM_REF_INDEX(def->args[0]);
        uint64_t val = ctx->func->consts[ci].val.raw;
        if (phys >= 0)
            a64_load_imm64(&ctx->buf, alloc_regs[phys], val);
        else if (fp >= 0) {
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, val);
            a64_buf_emit(&ctx->buf, a64_fmov_from_gpr(alloc_fp_regs[fp], SCRATCH_REG2));
        }
    } else if (def->op == XM_CONST_F64 && xm_ref_is_const(def->args[0])) {
        uint32_t ci = XM_REF_INDEX(def->args[0]);
        uint64_t val = ctx->func->consts[ci].val.raw;
        if (fp >= 0) {
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, val);
            a64_buf_emit(&ctx->buf, a64_fmov_from_gpr(alloc_fp_regs[fp], SCRATCH_REG2));
        } else if (phys >= 0)
            a64_load_imm64(&ctx->buf, alloc_regs[phys], val);
    } else if (def->op == XM_CONST_PTR && xm_ref_is_const(def->args[0])) {
        uint32_t ci = XM_REF_INDEX(def->args[0]);
        uint64_t val = ctx->func->consts[ci].val.raw;
        if (phys >= 0)
            a64_load_imm64(&ctx->buf, alloc_regs[phys], val);
    }
}

/* ========== OSR Entry Stubs ========== */

// Emit OSR entry stubs: alternate function entries for loop headers.
// OSR calling convention: x0=coro, x1=pointer to int64_t values array
// The stub does a standard prologue, loads live regs from the array,
// then jumps to the loop header block.
// Uses register allocator liveness to determine which slots to load.
XR_FUNC void a64_emit_osr_stubs(CodegenCtx *ctx, XmCodegenResult *result) {
    for (uint32_t i = 0; i < ctx->nosr_snap; i++) {
        OsrSnapshot *snap = &ctx->osr_snaps[i];
        XmOsrEntry *entry = &result->osr_entries[result->nosr];

        entry->block_id = snap->block_id;
        // Copy bytecode PC from the Xm block for VM-side OSR matching
        if (snap->block_id < ctx->func->nblk) {
            entry->bc_offset = ctx->func->blocks[snap->block_id]->bc_offset;
        }
        // Record byte offset of this stub
        entry->entry_offset = ctx->buf.count * 4;

        // Emit standard prologue (SUB SP patched later with final frame size)
        XR_DCHECK(ctx->nsub_patches < 8, "assertion failed");
        ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.count;
        a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, JIT_FRAME_BASE));
        a64_buf_emit(&ctx->buf, a64_stp(A64_FP, A64_LR, A64_SP, 0));
        a64_buf_emit(&ctx->buf, a64_stp(A64_X19, A64_X20, A64_SP, 16));
        a64_buf_emit(&ctx->buf, a64_stp(A64_X21, A64_X22, A64_SP, 32));
        a64_buf_emit(&ctx->buf, a64_stp(A64_X23, A64_X24, A64_SP, 48));
        a64_buf_emit(&ctx->buf, a64_stp(A64_X25, A64_X26, A64_SP, 64));
        a64_buf_emit(&ctx->buf, a64_stp(A64_X27, A64_X28, A64_SP, 80));
        a64_buf_emit(&ctx->buf, a64_stp_fp(8, 9, A64_SP, 96));
        a64_buf_emit(&ctx->buf, a64_stp_fp(10, 11, A64_SP, 112));
        a64_buf_emit(&ctx->buf, a64_stp_fp(12, 13, A64_SP, 128));
        a64_buf_emit(&ctx->buf, a64_stp_fp(14, 15, A64_SP, 144));
        a64_buf_emit(&ctx->buf, a64_add_imm(A64_FP, A64_SP, 0));
        a64_buf_emit(&ctx->buf, a64_mov(CORO_REG, A64_X0));
        // LDR x28, [x19, #jit_ctx_offset]  (load per-Worker JIT scratch pointer)
        a64_buf_emit(&ctx->buf, a64_ldr(JIT_CTX_REG, CORO_REG, XM_CORO_JIT_CTX_OFFSET));

        // Store stack_map_ptr into frame from jit_ctx->active_stack_map
        a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_OFFSET));
        a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, A64_FP, FRAME_SMAP_PTR_OFFSET));
        // Initialize safepoint_id = UINT32_MAX (both frame + jit_ctx)
        a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, 0xFFFF, 0));
        a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG2, 0xFFFF, 16));
        a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
        a64_buf_emit(&ctx->buf,
                     a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));
        // Save JIT frame SP for GC access
        a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XM_JIT_FRAME_SP_OFFSET));
        // Load guard page pointer into x20 from jit_ctx->safepoint_page for OSR entry
        a64_buf_emit(&ctx->buf, a64_ldr(SAFEPT_PAGE_REG, JIT_CTX_REG,
                                        (int32_t) XM_JIT_SAFEPOINT_PAGE_OFFSET));

        // Save values pointer to SCRATCH_REG (x16) before loading,
        // because x1 (alloc_regs[0]) may be overwritten by vreg loads
        a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, A64_X1));

        // Load live values from interpreter's register array into physical regs.
        // Directly queries the register allocator for vregs live at the OSR
        // block that map back to a bytecode slot.
        XmBlock *osr_blk =
            (snap->block_id < ctx->func->nblk) ? ctx->func->blocks[snap->block_id] : NULL;
        uint16_t nslots = 0;

        // Walk all vregs: if live at the OSR block (has phys reg) and has a
        // valid bc_slot, load it from the interpreter's values array.
        for (uint32_t v = 0; v < ctx->func->nvreg && nslots < XM_MAX_OSR_SLOTS; v++) {
            int16_t slot = ctx->func->vregs[v].bc_slot;
            if (slot < 0)
                continue;
            int8_t ri = xra_vreg_reg_at(ctx->xra, snap->block_id, v);
            if (ri < 0)
                continue;
            if (osr_should_skip_vreg(ctx, osr_blk, v, ri))
                continue;

            bool is_fp = (ctx->func->vregs[v].rep == XR_REP_F64);
            if (!is_fp) {
                A64Reg dst = alloc_regs[ri];
                a64_buf_emit(&ctx->buf, a64_ldr(dst, SCRATCH_REG, (int32_t) (slot * 8)));
                entry->slots[nslots].bc_slot = slot;
                entry->slots[nslots].phys_reg = (uint8_t) dst;
                entry->slots[nslots].type = XR_REP_I64;
                nslots++;
            } else {
                A64Reg dst = alloc_fp_regs[ri];
                a64_buf_emit(&ctx->buf, a64_ldr_fp(dst, SCRATCH_REG, (int32_t) (slot * 8)));
                entry->slots[nslots].bc_slot = slot;
                entry->slots[nslots].phys_reg = (uint8_t) dst;
                entry->slots[nslots].type = XR_REP_F64;
                nslots++;
            }
        }

        // Materialize constants (vregs without bc_slot) into physical regs.
        for (uint32_t v = 0; v < ctx->func->nvreg && nslots < XM_MAX_OSR_SLOTS; v++) {
            int8_t ri = xra_vreg_reg_at(ctx->xra, snap->block_id, v);
            if (ri < 0)
                continue;
            if (ctx->func->vregs[v].bc_slot >= 0)
                continue;  // handled above
            if (!ctx->func->vregs[v].def)
                continue;
            bool is_fp = (ctx->func->vregs[v].rep == XR_REP_F64);
            osr_materialize_const(ctx, ctx->func->vregs[v].def, is_fp ? -1 : ri, is_fp ? ri : -1);
        }

        // Pass 3: spill-only live vregs.
        // A vreg with no phys reg at the OSR header but a spill slot AND a
        // bc_slot is "live but spilled": the first reload after entering
        // the loop will fault into uninitialized stack memory unless we
        // seed the spill slot from values[bc_slot] here. SCRATCH_REG (X16)
        // still holds values_ptr; SCRATCH_REG2 (X17) is free as transit.
        // 8-byte LDR/STR works for both i64 and f64 bit patterns.
        for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
            if (xra_vreg_reg_at(ctx->xra, snap->block_id, v) >= 0)
                continue;
            int16_t spill = xra_vreg_spill(ctx->xra, v);
            if (spill < 0)
                continue;
            int16_t bc_slot = ctx->func->vregs[v].bc_slot;
            if (bc_slot < 0)
                continue;
            // ri=-1 disables phi-coalesce check; we only want "defined in osr_blk".
            if (osr_should_skip_vreg(ctx, osr_blk, v, -1))
                continue;
            int32_t bc_off = (int32_t) bc_slot * 8;
            int32_t spill_off = SPILL_BASE + (int32_t) spill * 8;
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, SCRATCH_REG, bc_off));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, A64_SP, spill_off));
        }

        entry->nslots = nslots;

        // Branch to loop header block
        int32_t target_offset = (int32_t) snap->block_offset - (int32_t) ctx->buf.count;
        a64_buf_emit(&ctx->buf, a64_b(target_offset));

        result->nosr++;
    }
}

/* ========== Branch Patching ========== */

XR_FUNC void a64_patch_branches(CodegenCtx *ctx) {
    for (uint32_t i = 0; i < ctx->npatch; i++) {
        BranchPatch *p = &ctx->patches[i];
        if (p->target_blk >= ctx->nblock_offsets)
            continue;
        int32_t offset = (int32_t) ctx->block_offsets[p->target_blk] - (int32_t) p->emit_idx;

        switch (p->type) {
            case PATCH_B:
                ctx->buf.code[p->emit_idx] = a64_b(offset);
                break;
            case PATCH_CBNZ:
                ctx->buf.code[p->emit_idx] = a64_cbnz(p->reg, offset);
                break;
            case PATCH_CBZ:
                ctx->buf.code[p->emit_idx] = a64_cbz(p->reg, offset);
                break;
            case PATCH_B_COND:
                ctx->buf.code[p->emit_idx] = a64_b_cond((A64Cond) p->reg, offset);
                break;
            case PATCH_SAFEPOINT:
                // Guard page safepoint: no per-function stub to patch
                break;
            case PATCH_BARRIER_FWD: {
                int32_t off = (int32_t) ctx->barrier_fwd_stub - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_bl(off);
                break;
            }
            case PATCH_BARRIER_BACK: {
                int32_t off = (int32_t) ctx->barrier_back_stub - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_bl(off);
                break;
            }
            case PATCH_DEOPT: {
                int32_t off = (int32_t) ctx->deopt_stub - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_b(off);
                break;
            }
            case PATCH_DEOPT_NE: {
                int32_t off = (int32_t) ctx->deopt_stub - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_b_cond(A64_CC_NE, off);
                break;
            }
            case PATCH_DEOPT_EQ: {
                int32_t off = (int32_t) ctx->deopt_stub - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_b_cond(A64_CC_EQ, off);
                break;
            }
            case PATCH_DEOPT_CBZ: {
                int32_t off = (int32_t) ctx->deopt_stub - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_cbz(p->reg, off);
                break;
            }
            case PATCH_DEOPT_CBNZ: {
                int32_t off = (int32_t) ctx->deopt_stub - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_cbnz(p->reg, off);
                break;
            }
            case PATCH_DEOPT_CS: {
                int32_t off = (int32_t) ctx->deopt_stub - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_b_cond(A64_CC_CS, off);
                break;
            }
            case PATCH_CALL_C: {
                int32_t off = (int32_t) ctx->call_c_stub - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_bl(off);
                break;
            }
            case PATCH_CALL_SELF: {
                // BL to function entry (instruction index 0)
                int32_t off = 0 - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_bl(off);
                break;
            }
            case PATCH_CALL_SELF_FAST: {
                // BL to fast entry (after param loading in prologue)
                int32_t off = (int32_t) ctx->fast_entry_offset - (int32_t) p->emit_idx;
                ctx->buf.code[p->emit_idx] = a64_bl(off);
                break;
            }
        }
    }
}

/* ========== Runtime Deopt Table Construction ========== */

// Convert Xm deopt infos to runtime deopt entries using final regalloc state.
// Must be called after codegen (regalloc is finalized) but before ctx is freed.
XR_FUNC void a64_build_runtime_deopt_table(CodegenCtx *ctx, XmCodegenResult *result) {
    XmFunc *func = ctx->func;
    result->ndeopt = 0;
    if (!func->deopt_infos || func->ndeopt == 0)
        return;

    for (uint32_t d = 0; d < func->ndeopt && result->ndeopt < XM_MAX_RT_DEOPT_ENTRIES; d++) {
        XmDeoptInfo *src = &func->deopt_infos[d];
        XmRtDeoptEntry *dst = &result->deopt_entries[result->ndeopt];
        dst->bc_pc = src->bc_pc;
        dst->deopt_id = src->deopt_id;
        dst->nslots = 0;

        for (uint16_t s = 0; s < src->nslots && dst->nslots < XM_MAX_DEOPT_SLOTS; s++) {
            XmDeoptSlot *slot = &src->slots[s];
            XmRtDeoptSlot *rs = &dst->slots[dst->nslots];
            rs->bc_slot = slot->bc_slot;
            rs->type = slot->rep;
            rs->xr_tag = slot->xr_tag;
            rs->_pad = 0;
            rs->vreg_idx = xm_ref_is_vreg(slot->value) ? (uint16_t) XM_REF_INDEX(slot->value) : 0xFFFF;

            XmRef ref = slot->value;
            if (xm_ref_is_none(ref))
                continue;

            if (xm_ref_is_const(ref)) {
                // Constant: embed value directly
                uint32_t ci = XM_REF_INDEX(ref);
                XmConst *c = &func->consts[ci];
                if (slot->rep == XR_REP_F64) {
                    rs->loc_kind = DEOPT_LOC_CONST_F64;
                    rs->loc.const_f64 = c->val.f64;
                } else if (slot->rep == XR_REP_PTR) {
                    rs->loc_kind = DEOPT_LOC_CONST_PTR;
                    rs->loc.const_ptr = c->val.ptr;
                } else {
                    rs->loc_kind = DEOPT_LOC_CONST_I64;
                    rs->loc.const_i64 = c->val.i64;
                }
            } else {
                // Vreg: look up physical register from XraResult
                uint32_t vi = XM_REF_INDEX(ref);
                if (vi >= func->nvreg)
                    continue;

                // Search blocks' beg[] then full[] for this vreg's register.
                // beg[] has live-in vregs (cross-block); full[] also has
                // block-local vregs that are defined and used within one block.
                bool found = false;
                // Segment lookup: find first assigned register for this vreg
                int8_t dreg = xra_vreg_first_reg(ctx->xra, vi);
                if (dreg >= 0) {
                    if (slot->rep == XR_REP_F64) {
                        rs->loc_kind = DEOPT_LOC_FP_REG;
                        rs->loc.phys_reg = (uint8_t) alloc_fp_regs[dreg];
                    } else {
                        rs->loc_kind = DEOPT_LOC_REG;
                        rs->loc.phys_reg = (uint8_t) alloc_regs[dreg];
                    }
                    found = true;
                }
                if (!found) {
                    int16_t sp = xra_vreg_spill(ctx->xra, vi);
                    if (sp >= 0) {
                        rs->loc_kind = DEOPT_LOC_SPILL;
                        rs->loc.spill_offset = (int16_t) (sp * 8);
                        found = true;
                    }
                }
                // Fallback: follow MOV def chain
                if (!found) {
                    uint32_t cur = vi;
                    for (int depth = 0; depth < 4 && !found; depth++) {
                        XmIns *def = (cur < func->nvreg) ? func->vregs[cur].def : NULL;
                        if (!def)
                            break;
                        if (!xm_op_is_copy(def->op))
                            break;
                        XmRef src = def->args[0];
                        if (!xm_ref_is_vreg(src))
                            break;
                        uint32_t sv = XM_REF_INDEX(src);
                        if (sv >= func->nvreg)
                            break;
                        int8_t sr = xra_vreg_first_reg(ctx->xra, sv);
                        if (sr >= 0) {
                            bool is_fp = (slot->rep == XR_REP_F64);
                            rs->loc_kind = is_fp ? DEOPT_LOC_FP_REG : DEOPT_LOC_REG;
                            rs->loc.phys_reg =
                                is_fp ? (uint8_t) alloc_fp_regs[sr] : (uint8_t) alloc_regs[sr];
                            found = true;
                        }
                        if (!found) {
                            int16_t sp2 = xra_vreg_spill(ctx->xra, sv);
                            if (sp2 >= 0) {
                                rs->loc_kind = DEOPT_LOC_SPILL;
                                rs->loc.spill_offset = (int16_t) (sp2 * 8);
                                found = true;
                            }
                        }
                        cur = sv;
                    }
                }
                if (!found)
                    continue;
            }
            dst->nslots++;
        }
        result->ndeopt++;
    }
}

/* ========== Resume Entry Stub (JIT Suspend/Resume) ========== */

/*
 * Emit resume entry stub for JIT suspend/resume.
 *
 * When a coroutine is JIT-suspended (XM_SUSPEND returned SUSPEND_MARKER),
 * the worker calls this entry point to re-enter JIT code. The stub:
 * 1. Sets up the same frame as the normal prologue
 * 2. Reloads ALL live registers from coro->jit_suspend (pointer)
 * 3. Loads the await result into the correct physical register
 * 4. Dispatches to the continuation point via suspend_id jump table
 *
 * Calling convention: same as XmJitFn — x0=coro, x1=unused
 * Returns: XrJitResult in x0 (payload) + x1 (tag)
 */
XR_FUNC void a64_emit_resume_entry(CodegenCtx *ctx, XmCodegenResult *result) {
    if (ctx->nsuspend == 0)
        return;

    ctx->resume_entry_offset = ctx->buf.count;

    // === Prologue (identical frame layout to normal entry) ===

    // SUB SP, SP, #frame_size (placeholder, patched by global frame_size patch)
    XR_DCHECK(ctx->nsub_patches < 8, "too many sub patches for resume entry");
    ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, JIT_FRAME_BASE));
    // STP x29, x30, [SP, #0]
    a64_buf_emit(&ctx->buf, a64_stp(A64_FP, A64_LR, A64_SP, 0));
    // Save callee-saved registers (uses same cs_patch as normal prologue)
    a64_buf_emit(&ctx->buf, a64_stp(A64_X19, A64_X20, A64_SP, 16));
    add_cs_patch(ctx, 0);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X21, A64_X22, A64_SP, 32));
    add_cs_patch(ctx, 1);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X23, A64_X24, A64_SP, 48));
    add_cs_patch(ctx, 2);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X25, A64_X26, A64_SP, 64));
    add_cs_patch(ctx, 3);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X27, A64_X28, A64_SP, 80));
    add_cs_patch(ctx, 4);
    a64_buf_emit(&ctx->buf, a64_stp_fp(8, 9, A64_SP, 96));
    add_cs_patch(ctx, 5);
    a64_buf_emit(&ctx->buf, a64_stp_fp(10, 11, A64_SP, 112));
    add_cs_patch(ctx, 6);
    a64_buf_emit(&ctx->buf, a64_stp_fp(12, 13, A64_SP, 128));
    add_cs_patch(ctx, 7);
    a64_buf_emit(&ctx->buf, a64_stp_fp(14, 15, A64_SP, 144));
    // MOV x29, SP
    a64_buf_emit(&ctx->buf, a64_add_imm(A64_FP, A64_SP, 0));
    // MOV x19, x0 (CORO_REG = coro)
    a64_buf_emit(&ctx->buf, a64_mov(CORO_REG, A64_X0));
    // LDR x28, [x19, #jit_ctx_offset]
    a64_buf_emit(&ctx->buf, a64_ldr(JIT_CTX_REG, CORO_REG, XM_CORO_JIT_CTX_OFFSET));

    // Store stack_map_ptr into frame
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, A64_FP, FRAME_SMAP_PTR_OFFSET));
    // Initialize safepoint_id = UINT32_MAX
    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, 0xFFFF, 0));
    a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG, 0xFFFF, 16));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, A64_FP, FRAME_SMAP_ID_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));
    // Save JIT frame SP for GC
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XM_JIT_FRAME_SP_OFFSET));

    // === Load suspend_id ===
    // LDR w16, [x19, #suspend_id_offset]
    a64_buf_emit(&ctx->buf, a64_ldr_w(SCRATCH_REG, CORO_REG, XM_CORO_SUSPEND_ID_OFFSET));

    // === Reload ALL registers from coro->jit_suspend (pointer deref) ===
    // LDR x17, [x19, #jit_suspend_ptr_offset]
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, CORO_REG, XM_CORO_SUSPEND_PTR_OFFSET));

    // x1-x15 from suspend_regs[0..14]
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X1, A64_X2, SCRATCH_REG2, 0));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X3, A64_X4, SCRATCH_REG2, 16));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X5, A64_X6, SCRATCH_REG2, 32));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X7, A64_X8, SCRATCH_REG2, 48));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X9, A64_X10, SCRATCH_REG2, 64));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X11, A64_X12, SCRATCH_REG2, 80));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X13, A64_X14, SCRATCH_REG2, 96));
    a64_buf_emit(&ctx->buf, a64_ldr(A64_X15, SCRATCH_REG2, 112));

    // x20-x27 from suspend_regs[15..22]
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X20, A64_X21, SCRATCH_REG2, 120));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X22, A64_X23, SCRATCH_REG2, 136));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X24, A64_X25, SCRATCH_REG2, 152));
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X26, A64_X27, SCRATCH_REG2, 168));

    // Override x20 (SAFEPT_PAGE_REG) with the CURRENT worker's guard page.
    // The suspend_regs[15] value is from the old worker; after gopark the
    // coro may resume on a different worker whose safepoint_page differs.
    a64_buf_emit(&ctx->buf,
                 a64_ldr(SAFEPT_PAGE_REG, JIT_CTX_REG, (int32_t) XM_JIT_SAFEPOINT_PAGE_OFFSET));

    // === Clear jit_resume_entry (one-shot: prevent double-resume) ===
    a64_buf_emit(&ctx->buf, a64_str(A64_XZR, CORO_REG, XM_CORO_RESUME_ENTRY_OFFSET));

    // === Restore spill slots from suspend_state.spill[] into new frame ===
    // The XM_SUSPEND codegen saved the old frame's spill area into
    // suspend_state.spill[0..nspill-1].  Copy them to the new frame.
    {
        uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
        if (ns > XM_SUSPEND_SPILL_MAX)
            ns = XM_SUSPEND_SPILL_MAX;
        if (ns > 0) {
            // Load jit_suspend pointer into SCRATCH_REG for spill access
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, CORO_REG, XM_CORO_SUSPEND_PTR_OFFSET));
            for (uint32_t s = 0; s < ns; s++) {
                int32_t regs_off = XM_SUSPEND_SPILL_OFF + (int32_t) s * 8;
                int32_t frame_off = SPILL_BASE + (int32_t) s * 8;
                // LDR x17, [x16, #spill_off]
                a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, SCRATCH_REG, regs_off));
                // STR x17, [SP, #frame_off]
                a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, A64_SP, frame_off));
            }
        }
    }

    // === Per-suspend-id dispatch: load result + branch to continuation ===
    // x16 = suspend_id (loaded above), x17 = suspend_regs base
    // For each suspend point: CMP + B.EQ to trampoline
    // Each trampoline: LDR result_reg, [x17, #result_off]; B continuation

    // First emit CMP/B.EQ chain (suspend_id dispatch)
    uint32_t trampoline_patches[16];
    for (uint32_t i = 0; i < ctx->nsuspend; i++) {
        // CMP w16, #i  (SUBS WZR, W16, #i)
        a64_buf_emit(&ctx->buf, a64_subs_imm_w(A64_XZR, SCRATCH_REG, i));
        // B.EQ trampoline_i (placeholder, patched below)
        trampoline_patches[i] = ctx->buf.count;
        a64_buf_emit(&ctx->buf, a64_nop());
    }

    // Fallback: should not reach here. Return DEOPT_MARKER.
    a64_load_imm64(&ctx->buf, A64_X0, (uint64_t) XM_DEOPT_MARKER);
    a64_buf_emit(&ctx->buf, a64_movz(A64_X1, 0, 0));
    emit_epilogue(ctx);
    a64_buf_emit(&ctx->buf, a64_ret());

    // Emit per-suspend trampolines
    for (uint32_t i = 0; i < ctx->nsuspend; i++) {
        // Patch B.EQ to here
        uint32_t here = ctx->buf.count;
        int32_t off = (int32_t) here - (int32_t) trampoline_patches[i];
        ctx->buf.code[trampoline_patches[i]] = a64_bcond(A64_CC_EQ, off);

        // Reload x17 = coro->jit_suspend (pointer deref for result + result_tag)
        a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, CORO_REG, XM_CORO_SUSPEND_PTR_OFFSET));

        // Load result from suspend_state.result into the correct register
        A64Reg rd = (A64Reg) ctx->suspend_result_regs[i];
        if (rd != A64_XZR) {
            a64_buf_emit(&ctx->buf, a64_ldr(rd, SCRATCH_REG2, XM_SUSPEND_RESULT_OFF));
        }

        // Load result_tag and write to vreg_runtime_tags[vi]
        int32_t tag_off = ctx->suspend_result_tag_offs[i];
        if (tag_off >= 0) {
            a64_buf_emit(&ctx->buf,
                         a64_ldrb(SCRATCH_REG, SCRATCH_REG2, (int32_t) XM_SUSPEND_RESULT_TAG_OFF));
            a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG, JIT_CTX_REG, tag_off));
        }

        // Branch to continuation point in main function code
        uint32_t cont_offset = ctx->suspend_cont_offsets[i];
        int32_t branch_off = (int32_t) cont_offset - (int32_t) ctx->buf.count;
        a64_buf_emit(&ctx->buf, a64_b(branch_off));
    }

    // Convert instruction index to byte offset (ARM64: 4 bytes/insn)
    result->resume_entry_offset = ctx->resume_entry_offset * 4;
}


#endif  /* __aarch64__ */
