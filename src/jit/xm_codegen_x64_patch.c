/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_x64_patch.c - Edge copies (phi resolution) and branch patching
 *
 * Contains x64_emit_edge_copies() for parallel-move phi resolution
 * and x64_patch_branches() for deferred rel32 fixups.  Split from
 * xm_codegen_x64.c to keep each file under 3000 lines.
 */

#if defined(__x86_64__) || defined(_M_X64)

#include "xm_codegen_x64_internal.h"

/* ========== Edge Copies (Phi Resolution) ========== */

XR_FUNC void x64_emit_edge_copies(X64CodegenCtx *ctx, XmBlock *target, XmBlock *from) {
    if (!ctx->xra)
        return;
    typedef struct {
        X64Reg dst, src;
        bool done, is_fp, is_reload;
        int16_t spill_slot;
    } Copy;
    Copy copies[64];
    uint32_t n = 0;

    XraEdgeCopy ec[64];
    uint32_t nc = xra_edge_copies(ctx->xra, ctx->func, target, from, ec, 64);
    for (uint32_t i = 0; i < nc && n < 64; i++) {
        X64Reg d =
            ec[i].is_fp ? (X64Reg) x64_alloc_fp_regs[ec[i].dst_idx] : x64_alloc_regs[ec[i].dst_idx];
        if (ec[i].is_reload) {
            copies[n++] = (Copy) {d, X64_R11, false, ec[i].is_fp, true, ec[i].spill_slot};
            continue;
        }
        X64Reg s =
            ec[i].is_fp ? (X64Reg) x64_alloc_fp_regs[ec[i].src_idx] : x64_alloc_regs[ec[i].src_idx];
        if (d != s)
            copies[n++] = (Copy) {d, s, false, ec[i].is_fp, false, 0};
    }
    if (n == 0)
        return;

    /* Phase 1: emit non-conflicting copies (topological order) */
    bool progress = true;
    while (progress) {
        progress = false;
        for (uint32_t i = 0; i < n; i++) {
            if (copies[i].done)
                continue;
            bool blocked = false;
            for (uint32_t j = 0; j < n; j++) {
                if (j == i || copies[j].done)
                    continue;
                if (!copies[j].is_reload && copies[j].is_fp == copies[i].is_fp &&
                    copies[j].src == copies[i].dst) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) {
                if (copies[i].is_reload) {
                    int32_t off = -(X64_SPILL_BASE + copies[i].spill_slot * 8);
                    if (copies[i].is_fp)
                        x64_movsd_rm(&ctx->buf, (X64Xmm) copies[i].dst, X64_RBP, off);
                    else
                        x64_mov_rm(&ctx->buf, copies[i].dst, X64_RBP, off);
                } else if (copies[i].is_fp) {
                    x64_movsd_rr(&ctx->buf, (X64Xmm) copies[i].dst, (X64Xmm) copies[i].src);
                } else {
                    x64_mov_rr(&ctx->buf, copies[i].dst, copies[i].src);
                }
                copies[i].done = true;
                progress = true;
            }
        }
    }

    /* Phase 2: break remaining cycles with scratch register */
    for (uint32_t i = 0; i < n; i++) {
        if (copies[i].done)
            continue;
        if (copies[i].is_reload) {
            int32_t off = -(X64_SPILL_BASE + copies[i].spill_slot * 8);
            if (copies[i].is_fp)
                x64_movsd_rm(&ctx->buf, (X64Xmm) copies[i].dst, X64_RBP, off);
            else
                x64_mov_rm(&ctx->buf, copies[i].dst, X64_RBP, off);
            copies[i].done = true;
            continue;
        }

        /* Save cycle entry to scratch */
        X64Reg cycle_start = copies[i].dst;
        bool cycle_fp = copies[i].is_fp;
        if (cycle_fp)
            x64_movsd_rr(&ctx->buf, X64_SCRATCH_XMM, (X64Xmm) cycle_start);
        else
            x64_mov_rr(&ctx->buf, X64_SCRATCH_REG, cycle_start);

        /* Emit first copy in the cycle */
        if (cycle_fp)
            x64_movsd_rr(&ctx->buf, (X64Xmm) copies[i].dst, (X64Xmm) copies[i].src);
        else
            x64_mov_rr(&ctx->buf, copies[i].dst, copies[i].src);
        copies[i].done = true;
        X64Reg last_src = copies[i].src;

        /* Follow the cycle chain: find copy whose dst == last_src
         * (that register was just read, so it's safe to overwrite now) */
        for (;;) {
            bool found = false;
            for (uint32_t j = 0; j < n; j++) {
                if (copies[j].done || copies[j].is_fp != cycle_fp)
                    continue;
                if (copies[j].dst == last_src) {
                    if (copies[j].src == cycle_start) {
                        /* Close cycle: src needs saved value from scratch */
                        if (cycle_fp)
                            x64_movsd_rr(&ctx->buf, (X64Xmm) copies[j].dst, X64_SCRATCH_XMM);
                        else
                            x64_mov_rr(&ctx->buf, copies[j].dst, X64_SCRATCH_REG);
                    } else {
                        if (cycle_fp)
                            x64_movsd_rr(&ctx->buf, (X64Xmm) copies[j].dst, (X64Xmm) copies[j].src);
                        else
                            x64_mov_rr(&ctx->buf, copies[j].dst, copies[j].src);
                    }
                    last_src = copies[j].src;
                    copies[j].done = true;
                    found = true;
                    break;
                }
            }
            if (!found)
                break;
        }
    }
}

/* ========== Branch Patching ========== */

XR_FUNC void x64_patch_branches(X64CodegenCtx *ctx) {
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
                x64_patch_rel32(&ctx->buf, p->emit_pos, 0); /* entry at offset 0 */
                break;
            case X64_PATCH_CALL_SELF_FAST:
                x64_patch_rel32(&ctx->buf, p->emit_pos, ctx->fast_entry_offset);
                break;
            case X64_PATCH_BARRIER_FWD:
                x64_patch_rel32(&ctx->buf, p->emit_pos, ctx->barrier_fwd_stub);
                break;
            case X64_PATCH_BARRIER_BACK:
                x64_patch_rel32(&ctx->buf, p->emit_pos, ctx->barrier_back_stub);
                break;
            default:
                CODEGEN_CHECK(ctx, p->target_blk < ctx->nblock_offsets,
                              "x64_patch: target block OOB");
                x64_patch_rel32(&ctx->buf, p->emit_pos, ctx->block_offsets[p->target_blk]);
                break;
        }
    }
}

#endif /* __x86_64__ || _M_X64 */
