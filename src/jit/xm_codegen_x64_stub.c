/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_x64_stub.c - Shared stubs and safepoint helpers (x86-64)
 *
 * Split from xm_codegen_x64.c for modularity.
 * Contains: call_c_stub, barrier stubs, deopt stub,
 *           safepoint recording, spill writeback, live reg collection.
 */

#if defined(__x86_64__) || defined(_M_X64)

#include "xm_codegen_x64_internal.h"
#include "xm_offsets.h"
#include "xm_jit_runtime.h"

/* ========== Call-C Stub ========== */

/* Emit call_c_stub: shared trampoline that saves/restores all registers
 * around a C function call.
 *
 * Protocol:
 *   R11 = C function pointer
 *   [R14 + X64_EXTRA_ARG_OFFSET] = extra argument (pre-stored by codegen)
 *
 * Stub saves all 11 allocatable GP + 15 FP regs, calls the C function
 * using platform ABI, saves result payload/tag, restores all regs,
 * returns payload in RAX.
 *
 * ABI difference:
 *   SysV:  call(RDI=coro, RSI=extra), return RAX=payload, RDX=tag
 *   Win64: call(RCX=ret_buf_ptr, RDX=coro, R8=extra), struct via hidden ptr */
XR_FUNC void x64_emit_call_c_stub(X64CodegenCtx *ctx) {
    if (!ctx->has_call_c)
        return;
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
        x64_movsd_mr(&ctx->buf, X64_RSP, i * 8, (X64Xmm) i);

    /* Save SP to jit_ctx for GC stack map access */
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_SAFEPOINT_SAVED_SP_OFFSET, X64_RSP);

#ifdef _WIN32
    /* Win64: XrJitResult (16B) returned via hidden pointer.
     * Allocate 16 (return buf) + 32 (shadow) = 48 bytes. */
    x64_sub_ri(&ctx->buf, X64_RSP, 48);
    /* RCX = hidden return pointer (into stack-allocated XrJitResult) */
    x64_lea(&ctx->buf, X64_RCX, X64_RSP, 32);
    /* RDX = coro, R8 = extra_arg */
    x64_mov_rr(&ctx->buf, X64_RDX, X64_CORO_REG);
    x64_mov_rm(&ctx->buf, X64_R8, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET);
    x64_call_r(&ctx->buf, X64_SCRATCH_REG);
    /* Read payload/tag from return buffer */
    x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_RSP, 32); /* payload */
    x64_mov_rm(&ctx->buf, X64_RDX, X64_RSP, 40);         /* tag */
    x64_add_ri(&ctx->buf, X64_RSP, 48);
#else
    /* System V: RDI=coro, RSI=extra_arg, func_ptr in R11 */
    x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
    x64_mov_rm(&ctx->buf, X64_RSI, X64_JIT_CTX_REG, (int32_t) X64_EXTRA_ARG_OFFSET);
    x64_call_r(&ctx->buf, X64_SCRATCH_REG);
    /* XrJitResult returned in RAX(payload), RDX(tag) */
    x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, X64_RAX);
#endif

    /* Store tag to jit_ctx */
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_CALL_RESULT_TAG_OFFSET, X64_RDX);

    /* Restore 15 FP regs */
    for (int i = 0; i < 15; i++)
        x64_movsd_rm(&ctx->buf, (X64Xmm) i, X64_RSP, i * 8);
    x64_add_ri(&ctx->buf, X64_RSP, 128);

    /* Restore 11 GP regs (reverse order) */
    for (int i = X64_MAX_PHYS_REGS - 1; i >= 0; i--)
        x64_pop_r(&ctx->buf, x64_alloc_regs[i]);

    /* Return payload in RAX */
    x64_mov_rr(&ctx->buf, X64_RAX, X64_SCRATCH_REG);
    x64_ret(&ctx->buf);
}

/* ========== Write Barrier Stubs ========== */

/* Barrier stubs save/restore caller-saved GP regs, then call the
 * runtime barrier function via platform ABI.
 * On entry:  R11 = parent/container, RCX = child (fwd only).
 * Convention: barriers are leaf-like (no GC re-entry, no safepoint needed). */

XR_FUNC void x64_emit_barrier_stubs(X64CodegenCtx *ctx) {
    if (!ctx->has_barriers)
        return;

    /* Forward barrier stub: xr_jit_barrier_fwd(coro, parent, child)
     * On entry: R11 = parent, RCX = child. */
    ctx->barrier_fwd_stub = ctx->buf.pos;

    int ncaller = X64_NGPR_CALLER_SAVE;
    /* Save all caller-saved GP alloc regs + RCX_child (avoid clobber). */
    x64_push_r(&ctx->buf, X64_RCX); /* save child */
    for (int i = 0; i < ncaller && i < X64_MAX_PHYS_REGS; i++)
        x64_push_r(&ctx->buf, x64_alloc_regs[i]);
    /* Alignment: (ncaller+1 pushes + return addr) * 8 must be 16-aligned.
     * If not, push a dummy. */
    int total_pushes_fwd = ncaller + 1;                         /* +1 for RCX save */
    bool need_pad_fwd = ((total_pushes_fwd + 1) * 8) % 16 != 0; /* +1 for ret addr */
    if (need_pad_fwd)
        x64_push_r(&ctx->buf, X64_SCRATCH_REG);

    int child_stack_off = (total_pushes_fwd + (need_pad_fwd ? 1 : 0)) * 8;

#ifdef _WIN32
    /* Win64: shadow space + args in RCX,RDX,R8 */
    x64_sub_ri(&ctx->buf, X64_RSP, X64_ABI_SHADOW_BYTES);
    x64_mov_rr(&ctx->buf, X64_RCX, X64_CORO_REG);
    x64_mov_rr(&ctx->buf, X64_RDX, X64_SCRATCH_REG); /* parent was in R11 */
    x64_mov_rm(&ctx->buf, X64_R8, X64_RSP, X64_ABI_SHADOW_BYTES + child_stack_off);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_barrier_fwd);
    x64_call_r(&ctx->buf, X64_SCRATCH_REG);
    x64_add_ri(&ctx->buf, X64_RSP, X64_ABI_SHADOW_BYTES);
#else
    /* System V: RDI=coro, RSI=parent(R11), RDX=child(from stack) */
    x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
    x64_mov_rr(&ctx->buf, X64_RSI, X64_SCRATCH_REG);
    x64_mov_rm(&ctx->buf, X64_RDX, X64_RSP, child_stack_off);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_barrier_fwd);
    x64_call_r(&ctx->buf, X64_SCRATCH_REG);
#endif

    /* Restore regs */
    if (need_pad_fwd)
        x64_pop_r(&ctx->buf, X64_SCRATCH_REG);
    for (int i = ncaller - 1; i >= 0 && i < X64_MAX_PHYS_REGS; i--)
        x64_pop_r(&ctx->buf, x64_alloc_regs[i]);
    x64_pop_r(&ctx->buf, X64_RCX);
    x64_ret(&ctx->buf);

    /* Back barrier stub: xr_jit_barrier_back(coro, container)
     * On entry: R11 = container. */
    ctx->barrier_back_stub = ctx->buf.pos;

    for (int i = 0; i < ncaller && i < X64_MAX_PHYS_REGS; i++)
        x64_push_r(&ctx->buf, x64_alloc_regs[i]);
    bool need_pad_back = ((ncaller + 1) * 8) % 16 != 0; /* +1 for ret addr */
    if (need_pad_back)
        x64_push_r(&ctx->buf, X64_SCRATCH_REG);

#ifdef _WIN32
    x64_sub_ri(&ctx->buf, X64_RSP, X64_ABI_SHADOW_BYTES);
    x64_mov_rr(&ctx->buf, X64_RCX, X64_CORO_REG);
    x64_mov_rr(&ctx->buf, X64_RDX, X64_SCRATCH_REG);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_barrier_back);
    x64_call_r(&ctx->buf, X64_SCRATCH_REG);
    x64_add_ri(&ctx->buf, X64_RSP, X64_ABI_SHADOW_BYTES);
#else
    x64_mov_rr(&ctx->buf, X64_RDI, X64_CORO_REG);
    x64_mov_rr(&ctx->buf, X64_RSI, X64_SCRATCH_REG);
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_barrier_back);
    x64_call_r(&ctx->buf, X64_SCRATCH_REG);
#endif

    if (need_pad_back)
        x64_pop_r(&ctx->buf, X64_SCRATCH_REG);
    for (int i = ncaller - 1; i >= 0 && i < X64_MAX_PHYS_REGS; i--)
        x64_pop_r(&ctx->buf, x64_alloc_regs[i]);
    x64_ret(&ctx->buf);
}

/* ========== Deopt Stub ========== */

/* Emit deopt exit stub: saves all register state to jit_ctx->deopt_regs,
 * loads DEOPT_MARKER into RAX, then returns via epilogue.
 *
 * On entry to stub, [R14 + deopt_id_offset] already stores the deopt_id
 * (written by codegen before the Jcc/JMP to this stub). */
XR_FUNC void x64_emit_deopt_stub(X64CodegenCtx *ctx) {
    if (!ctx->has_deopt)
        return;
    ctx->deopt_stub = ctx->buf.pos;

    /* Save frame pointer for spill slot recovery */
    x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_SPILL_BASE_OFFSET, X64_RBP);

    /* Save all allocatable GP registers to jit_ctx->deopt_regs[reg_num].
     * Index by hardware register number so the deopt reconstructor
     * can map phys_reg → deopt_regs[phys_reg] directly. */
    int32_t gp_base = (int32_t) XM_JIT_DEOPT_REGS_OFFSET;
    for (int i = 0; i < X64_MAX_PHYS_REGS; i++) {
        X64Reg r = x64_alloc_regs[i];
        x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, gp_base + (int32_t) r * 8, r);
    }

    /* Save FP registers xmm0-xmm14 to jit_ctx->deopt_fp_regs[d_num] */
    int32_t fp_base = (int32_t) XM_JIT_DEOPT_FP_REGS_OFFSET;
    for (int i = 0; i < 15; i++) {
        x64_movsd_mr(&ctx->buf, X64_JIT_CTX_REG, fp_base + i * 8, (X64Xmm) i);
    }

    /* Copy spill slots from frame to jit_ctx->deopt_spill_save[] BEFORE
     * epilogue destroys the frame.  The deopt reconstructor reads these
     * via DEOPT_LOC_SPILL.  Uses R11 (SCRATCH_REG) — already saved above. */
    {
        uint32_t nspill = ctx->xra ? ctx->xra->nspill : 0;
        if (nspill > 32)
            nspill = 32;
        int32_t save_base = (int32_t) XM_JIT_DEOPT_SPILL_SAVE_OFFSET;
        for (uint32_t s = 0; s < nspill; s++) {
            int32_t frame_off = -(int32_t) (X64_SPILL_BASE + s * 8);
            x64_mov_rm(&ctx->buf, X64_SCRATCH_REG, X64_RBP, frame_off);
            x64_mov_mr(&ctx->buf, X64_JIT_CTX_REG, save_base + (int32_t) s * 8, X64_SCRATCH_REG);
        }
    }

    /* Load DEOPT_MARKER into RAX as return value */
    x64_load_imm64(&ctx->buf, X64_RAX, (uint64_t) XM_DEOPT_MARKER);

    /* Epilogue + RET */
    x64_emit_epilogue(ctx);
    x64_ret(&ctx->buf);
}

/* Helper: emit a deopt branch patch (Jcc rel32 → deopt_stub).
 * Before calling this, codegen has already stored deopt_id into
 * [R14 + XM_JIT_DEOPT_ID_OFFSET]. */
void x64_emit_deopt_jcc(X64CodegenCtx *ctx, X64Cond cc) {
    CODEGEN_CHECK(ctx, ctx->npatch < ctx->patches_cap, "too many patches");
    X64BranchPatch *p = &ctx->patches[ctx->npatch];
    x64_emit8(&ctx->buf, 0x0F);
    x64_emit8(&ctx->buf, (uint8_t) (0x80 | cc)); /* Jcc rel32 */
    p->emit_pos = ctx->buf.pos;
    p->target_blk = 0;
    p->type = X64_PATCH_DEOPT_JCC;
    p->cc = cc;
    ctx->npatch++;
    x64_emit32(&ctx->buf, 0); /* placeholder rel32 */
    ctx->has_deopt = true;
}

/* Helper: store deopt_id (from ins->dst const) to jit_ctx->deopt_id */
void x64_emit_deopt_id(X64CodegenCtx *ctx, XmIns *ins) {
    uint32_t did = 0xFFFF;
    if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
        uint32_t dci = XM_REF_INDEX(ins->dst);
        did = (uint32_t) ctx->func->consts[dci].val.raw;
    }
    /* Store deopt_id as 32-bit value to jit_ctx */
    x64_load_imm64(&ctx->buf, X64_SCRATCH_REG, (uint64_t) did);
    x64_mov_mr32(&ctx->buf, X64_JIT_CTX_REG, (int32_t) XM_JIT_DEOPT_ID_OFFSET, X64_SCRATCH_REG);
}

/* ========== GC Stack Map + Spill Writeback ========== */

/* Record safepoint bitmap: which alloc_regs[] hold PTR vregs (reg_bitmap),
 * and which spill slots hold PTR vregs (spill_bitmap) at cur_ra_pos. */
uint32_t x64_record_safepoint(X64CodegenCtx *ctx) {
    if (ctx->nsmap >= XM_MAX_STACK_MAP_ENTRIES) {
        xr_log_warning("x64-cg", "stack map table full (%u entries)", ctx->nsmap);
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
        if (ri >= 0 && ri < X64_MAX_PHYS_REGS) {
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
    ctx->smap_entries[sid].pc_offset = ctx->buf.pos;
    ctx->smap_entries[sid].reg_bitmap = reg_bitmap;
    ctx->smap_entries[sid].spill_bitmap = spill_bitmap;
    ctx->nsmap++;
    return sid;
}

/* Write back all live PTR register values to their spill slots.
 * Called before cross-function calls so GC can find PTR values in outer
 * frames by scanning spill slots. */
void x64_emit_ptr_spill_writeback(X64CodegenCtx *ctx) {
    CODEGEN_CHECK(ctx, ctx != NULL, "ptr_spill_writeback: NULL ctx");
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
        if (slot >= 0 && slot < 32 && ri >= 0 && ri < X64_MAX_PHYS_REGS) {
            X64Reg reg = x64_alloc_regs[ri];
            int32_t offset = -(X64_SPILL_BASE + slot * 8);
            x64_mov_mr(&ctx->buf, X64_RBP, offset, reg);
        }
    }
}

/* Collect live caller-saved GP regs (alloc indices 0..7) in current block. */
int x64_live_gp(X64CodegenCtx *ctx, X64Reg *out, X64Reg exclude) {
    uint32_t bid = ctx->cur_blk_id;
    uint32_t mask = (ctx->xra && bid < ctx->xra->nblk) ? ctx->xra->blk_gp_live[bid] : 0;
    int n = 0;
    /* Caller-saved: alloc indices 0..7 (RAX,RCX,RDX,RSI,RDI,R8,R9,R10) */
    for (int r = 0; r < 8; r++) {
        if ((mask & (1u << r)) && x64_alloc_regs[r] != exclude)
            out[n++] = x64_alloc_regs[r];
    }
    return n;
}

/* Collect live caller-saved FP regs in current block. */
int x64_live_fp(X64CodegenCtx *ctx, X64Xmm *out) {
    uint32_t bid = ctx->cur_blk_id;
    uint32_t mask = (ctx->xra && bid < ctx->xra->nblk) ? ctx->xra->blk_fp_live[bid] : 0;
    int n = 0;
    /* All xmm0-xmm13 are caller-saved in System V */
    for (int r = 0; r < X64_MAX_FP_REGS; r++) {
        if (mask & (1u << r))
            out[n++] = x64_alloc_fp_regs[r];
    }
    return n;
}

#endif  // __x86_64__ || _M_X64
