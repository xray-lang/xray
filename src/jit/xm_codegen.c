/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen.c - Xm → ARM64 machine code generation
 *
 * KEY CONCEPT:
 *   Single-pass emit with deferred branch patching:
 *   1. Lower Phi nodes to MOV instructions in predecessors
 *   2. Emit prologue (save FP/LR, establish stack frame)
 *   3. Emit blocks sequentially, recording start offsets
 *   4. Branch instructions emit NOP placeholders
 *   5. After all blocks, patch branch offsets in one sweep
 *
 * WHY THIS DESIGN:
 *   - Single pass + patch avoids fragile instruction-count estimation
 *   - Prologue/Epilogue follow AAPCS64 calling convention
 *   - Linear register allocation (vreg → x1-x15, with spill support)
 *   - x0 = coro pointer, x1 = args array pointer (entry convention)
 *   - x16/x17 reserved as scratch registers
 *   - x19 = coro pointer (callee-saved, set in prologue)
 */

#ifdef __aarch64__

#include "../base/xlog.h"

#include "xm_codegen_internal.h"
#include "xm_coalesce.h"
#include "xm_peephole.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

/* ========== Register Allocator (live-range aware) ========== */

// Types, constants, and macros now in xm_codegen_internal.h

const A64Reg alloc_regs[MAX_PHYS_REGS] = {
    A64_X1,  A64_X2,  A64_X3,  A64_X4,  A64_X5,  A64_X6,  A64_X7,  A64_X8,
    A64_X9,  A64_X10, A64_X11, A64_X12, A64_X13, A64_X14, A64_X15, A64_X21,
    A64_X22, A64_X23, A64_X24, A64_X25, A64_X26, A64_X27,
};

const A64Reg alloc_fp_regs[MAX_FP_REGS] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

void add_cs_patch(CodegenCtx *ctx, uint8_t pair) {
    if (ctx->ncs_patches >= ctx->cs_patches_cap) {
        uint32_t new_cap = ctx->cs_patches_cap * 2;
        XR_REALLOC_OR_ABORT(ctx->cs_patches, new_cap * sizeof(CsPatch), "codegen cs_patches grow");
        ctx->cs_patches_cap = new_cap;
    }
    ctx->cs_patches[ctx->ncs_patches].idx = ctx->buf.count;
    ctx->cs_patches[ctx->ncs_patches].pair = pair;
    ctx->ncs_patches++;
}

void add_patch(CodegenCtx *ctx, PatchType type, uint32_t target_blk, A64Reg reg) {
    if (ctx->npatch >= ctx->patches_cap) {
        uint32_t new_cap = ctx->patches_cap * 2;
        XR_REALLOC_OR_ABORT(ctx->patches, new_cap * sizeof(BranchPatch),
                            "codegen branch patches grow");
        ctx->patches_cap = new_cap;
    }
    BranchPatch *p = &ctx->patches[ctx->npatch++];
    p->emit_idx = ctx->buf.count;
    p->target_blk = target_blk;
    p->type = type;
    p->reg = reg;
}

/* ========== Rematerialization ========== */

/*
 * Emit code to rematerialise a spilled vreg into |rd|.
 * Returns true if the pattern was handled; false if not (caller should
 * fall through to the error path).
 *
 * Supported patterns (see is_remat in xm_regalloc.c):
 *   - XM_CONST_I64 / XM_CONST_F64 / XM_CONST_PTR  →  load imm64
 *   - XM_LOAD_CORO_BYTE with const offset  →  LDRB from jit_ctx
 *   - XM_I2F with const int operand  →  load imm64 + SCVTF
 *   - XM_F2I with const float operand  →  FMOV + FCVTZS
 */
static bool emit_remat(CodegenCtx *ctx, uint32_t vreg, A64Reg rd) {
    if (vreg >= ctx->func->nvreg)
        return false;
    XmIns *def = ctx->func->vregs[vreg].def;
    if (!def)
        return false;

    switch (def->op) {
        case XM_CONST_I64:
        case XM_CONST_F64:
        case XM_CONST_PTR:
            if (xm_ref_is_const(def->args[0])) {
                uint32_t ci = XM_REF_INDEX(def->args[0]);
                if (ci < ctx->func->nconst) {
                    a64_load_imm64(&ctx->buf, rd, (uint64_t) ctx->func->consts[ci].val.raw);
                    return true;
                }
            }
            return false;

        case XM_LOAD_CORO_BYTE:
            if (xm_ref_is_const(def->args[0])) {
                uint32_t ci = XM_REF_INDEX(def->args[0]);
                if (ci < ctx->func->nconst) {
                    int32_t offset = (int32_t) ctx->func->consts[ci].val.i64;
                    a64_buf_emit(&ctx->buf, a64_ldrb(rd, JIT_CTX_REG, offset));
                    return true;
                }
            }
            return false;

        case XM_I2F: {
            /* Pre-compute (double)int64 and emit as FP constant load.
             * rd is an FP register; a64_load_f64 handles GP→FP transfer. */
            if (xm_ref_is_const(def->args[0])) {
                uint32_t ci = XM_REF_INDEX(def->args[0]);
                if (ci < ctx->func->nconst) {
                    double d = (double) ctx->func->consts[ci].val.i64;
                    a64_load_f64(&ctx->buf, rd, SCRATCH_REG, d);
                    return true;
                }
            }
            return false;
        }

        case XM_F2I: {
            /* Pre-compute (int64_t)f64 at compile time and emit as
             * integer constant load — avoids needing an FP scratch reg. */
            if (xm_ref_is_const(def->args[0])) {
                uint32_t ci = XM_REF_INDEX(def->args[0]);
                if (ci < ctx->func->nconst) {
                    union {
                        uint64_t raw;
                        double d;
                    } u;
                    u.raw = ctx->func->consts[ci].val.raw;
                    int64_t ival = (int64_t) u.d;
                    a64_load_imm64(&ctx->buf, rd, (uint64_t) ival);
                    return true;
                }
            }
            return false;
        }

        default:
            return false;
    }
}

/* ========== XraResult Lookup (global vreg→reg) ========== */

// Per-block lookup: vreg → physical register using per-block mapping.
// Checks vreg_override first for mid-block register transitions (gap moves).
A64Reg xra_get(CodegenCtx *ctx, XmRef ref) {
    if (xm_ref_is_none(ref))
        return A64_XZR;
    if (!xm_ref_is_vreg(ref))
        return A64_XZR;
    uint32_t idx = XM_REF_INDEX(ref);

    /*
     * Check gap-move override first, then segment lookup.
     * RA uses even positions (upos) for uses, odd (upos+1) for defs.
     * Try cur_ra_pos (use) first; if not found, try cur_ra_pos+1 (def).
     */
    int8_t ri;
    if (ctx->vreg_override && idx < ctx->xra->nvreg && ctx->vreg_override[idx] != -128)
        ri = ctx->vreg_override[idx];
    else {
        ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    }

    if (idx < ctx->func->nvreg && ctx->func->vregs[idx].rep == XR_REP_F64) {
        if (ri < 0) {
            fprintf(stderr, "[regalloc-fp] func=%s vreg=%u pos=%d blk=%u\n",
                    ctx->func->name ? ctx->func->name : "?", idx, ctx->cur_ra_pos, ctx->cur_blk_id);
            fflush(stderr);
            ctx->had_error = true;
            return alloc_fp_regs[0];
        }
        return alloc_fp_regs[ri];
    }
    if (ri >= MAX_PHYS_REGS) {
        // Regalloc returned an index beyond physical register table — treat as error
        fprintf(stderr, "[regalloc-max] func=%s vreg=%u ri=%d pos=%d blk=%u\n",
                ctx->func->name ? ctx->func->name : "?", idx, ri, ctx->cur_ra_pos, ctx->cur_blk_id);
        fflush(stderr);
        ctx->had_error = true;
        return SCRATCH_REG;
    }
    if (ri < 0) {
        /*
         * Vreg has no register at this position. Check if it's live but
         * spilled (vs truly dead). Spilled vregs need an auto-reload
         * from their spill slot — this happens when the allocator split
         * a range and the tail was spilled, but codegen reaches a use
         * via xra_get() instead of xra_arg().
         */
        if (xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos) ||
            xra_vreg_live_at(ctx->xra, idx, ctx->cur_ra_pos + 1)) {
            int16_t slot = xra_vreg_spill(ctx->xra, idx);
            if (slot >= 0) {
                int32_t offset = SPILL_BASE + slot * 8;
                a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, A64_SP, offset));
                return SCRATCH_REG;
            }
            if (slot == XRA_SPILL_REMAT) {
                if (emit_remat(ctx, idx, SCRATCH_REG))
                    return SCRATCH_REG;
            }
        }
        // Genuinely dead vreg or missing spill slot — codegen error
        fprintf(stderr,
                "[regalloc-seg] func=%s vreg=%u (type=%d) pos=%d not found (nvreg=%u) blk=%u\n",
                ctx->func->name ? ctx->func->name : "?", idx,
                (idx < ctx->func->nvreg) ? ctx->func->vregs[idx].rep : -1, ctx->cur_ra_pos,
                ctx->xra->nvreg, ctx->cur_blk_id);
        fflush(stderr);
        ctx->had_error = true;
        return SCRATCH_REG;
    }
    return alloc_regs[ri];
}

/*
 * Emit gap moves scheduled before instruction ins_idx in the current block.
 * Updates vreg_override so subsequent xra_get calls return the new register.
 */
static void emit_gap_moves_before(CodegenCtx *ctx, uint32_t ins_idx) {
    if (!ctx->xra || !ctx->xra->gap_moves)
        return;
    XraGapMove *gm = ctx->xra->gap_moves;
    uint32_t n = ctx->xra->ngap_move;
    uint32_t bid = ctx->cur_blk_id;

    while (ctx->gap_move_cursor < n && gm[ctx->gap_move_cursor].gap_blk == bid &&
           gm[ctx->gap_move_cursor].gap_ins_idx <= ins_idx) {
        XraGapMove *m = &gm[ctx->gap_move_cursor++];

        if (m->src_reg >= 0 && m->dst_reg >= 0) {
            // reg-to-reg move
            A64Reg src = m->is_fp ? alloc_fp_regs[m->src_reg] : alloc_regs[m->src_reg];
            A64Reg dst = m->is_fp ? alloc_fp_regs[m->dst_reg] : alloc_regs[m->dst_reg];
            if (src != dst) {
                if (m->is_fp)
                    a64_buf_emit(&ctx->buf, a64_fmov(dst, src));
                else
                    a64_buf_emit(&ctx->buf, a64_mov(dst, src));
            }
        } else if (m->src_reg >= 0 && m->dst_reg < 0) {
            // reg-to-spill (store)
            A64Reg src = m->is_fp ? alloc_fp_regs[m->src_reg] : alloc_regs[m->src_reg];
            int32_t offset = SPILL_BASE + m->spill_slot * 8;
            if (m->is_fp)
                a64_buf_emit(&ctx->buf, a64_str_fp(src, A64_SP, offset));
            else
                a64_buf_emit(&ctx->buf, a64_str(src, A64_SP, offset));
        } else if (m->src_reg < 0 && m->dst_reg >= 0) {
            // spill-to-reg (reload)
            A64Reg dst = m->is_fp ? alloc_fp_regs[m->dst_reg] : alloc_regs[m->dst_reg];
            int32_t offset = SPILL_BASE + m->spill_slot * 8;
            if (m->is_fp)
                a64_buf_emit(&ctx->buf, a64_ldr_fp(dst, A64_SP, offset));
            else
                a64_buf_emit(&ctx->buf, a64_ldr(dst, A64_SP, offset));
        }

        // Update override so subsequent xra_get returns the new register
        if (m->vreg < ctx->xra->nvreg && ctx->vreg_override)
            ctx->vreg_override[m->vreg] = m->dst_reg;
    }
}

// Collect live caller-saved GP regs in current block (bitmask-based).
int xra_live_gp(CodegenCtx *ctx, A64Reg *out, A64Reg exclude_reg) {
    uint32_t bid = ctx->cur_blk_id;
    uint32_t mask = (bid < ctx->xra->nblk) ? ctx->xra->blk_gp_live[bid] : 0;
    int n = 0;
    // Only caller-saved: alloc indices 0..14 (x1-x15)
    for (int r = 0; r < 15; r++) {
        if ((mask & (1u << r)) && alloc_regs[r] != exclude_reg)
            out[n++] = alloc_regs[r];
    }
    return n;
}

// Collect live caller-saved FP regs (d0-d7) in current block.
int xra_live_fp(CodegenCtx *ctx, A64Reg *out) {
    uint32_t bid = ctx->cur_blk_id;
    uint32_t mask = (bid < ctx->xra->nblk) ? ctx->xra->blk_fp_live[bid] : 0;
    int n = 0;
    for (int r = 0; r < 8; r++) {
        if (mask & (1u << r))
            out[n++] = alloc_fp_regs[r];
    }
    return n;
}

// Collect live GP regs holding PTR-typed vregs (for GC shadow stack).
// Uses per-instruction RA position for precise tracking — the per-block
// blk_ptr_live bitmap is too coarse when split ranges cause register reuse
// between PTR and non-PTR vregs within the same block.
int xra_live_ptr(CodegenCtx *ctx, A64Reg *out) {
    int32_t pos = ctx->cur_ra_pos;
    int n = 0;
    uint32_t seen = 0;
    for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
        if (ctx->func->vregs[v].rep != XR_REP_PTR)
            continue;
        int8_t ri = xra_reg_at_pos(ctx->xra, v, pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, v, pos + 1);
        if (ri < 0)
            continue;
        uint32_t bit = (1u << ri);
        if (seen & bit)
            continue;
        seen |= bit;
        out[n++] = alloc_regs[ri];
    }
    return n;
}

/* ========== GC Stack Map Recording ========== */

// Record a safepoint bitmap at the current codegen position.
// Returns the safepoint_id (index into smap_entries[]).
// Captures which alloc_regs[] hold PTR vregs (reg_bitmap) and
// which spill slots hold PTR vregs (spill_bitmap) at cur_ra_pos.
uint32_t record_safepoint(CodegenCtx *ctx) {
    if (ctx->nsmap >= XM_MAX_STACK_MAP_ENTRIES) {
        fprintf(stderr, "[SMAP] WARNING: stack map table full (%u entries)\n", ctx->nsmap);
        return ctx->nsmap > 0 ? ctx->nsmap - 1 : 0;
    }

    int32_t pos = ctx->cur_ra_pos;
    uint32_t reg_bitmap = 0;
    uint32_t spill_bitmap = 0;

    for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
        if (ctx->func->vregs[v].rep != XR_REP_PTR)
            continue;

        // Check if vreg is live in a register at this position
        int8_t ri = xra_reg_at_pos(ctx->xra, v, pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, v, pos + 1);
        if (ri >= 0 && ri < MAX_PHYS_REGS) {
            reg_bitmap |= (1u << ri);
            // Also set spill_bitmap if this reg-allocated PTR has a spill slot.
            // This allows GC to find PTR values via spill slots in outer frames
            // (where caller-saved regs are not directly accessible).
            if (ctx->xra && v < ctx->xra->nvreg && ctx->xra->valloc[v].spill >= 0) {
                int16_t slot = ctx->xra->valloc[v].spill;
                if (slot >= 0 && slot < XM_MAX_SPILL_SLOTS)
                    spill_bitmap |= (1u << slot);
            }
            continue;
        }

        // Check if vreg is spilled AND still live at this position.
        // Dead PTR vregs' spill slots may have been reused by non-PTR vregs;
        // marking them as PTR causes GC to treat integer values as pointers.
        if (ctx->xra && v < ctx->xra->nvreg && ctx->xra->valloc[v].spill >= 0 &&
            xra_vreg_live_at(ctx->xra, v, pos)) {
            int16_t slot = ctx->xra->valloc[v].spill;
            if (slot >= 0 && slot < XM_MAX_SPILL_SLOTS) {
                spill_bitmap |= (1u << slot);
            }
        }
    }

    uint32_t sid = ctx->nsmap;
    ctx->smap_entries[sid].pc_offset = a64_buf_offset(&ctx->buf);
    ctx->smap_entries[sid].reg_bitmap = reg_bitmap;
    ctx->smap_entries[sid].spill_bitmap = spill_bitmap;
    ctx->nsmap++;
    return sid;
}

// Write back all live PTR register values to their spill slots.
// Called before cross-function calls so GC can find PTR values in outer frames
// by scanning spill slots (without needing access to caller-saved registers).
// For PTR vregs without spill slots, dynamically allocates one.
void emit_ptr_spill_writeback(CodegenCtx *ctx) {
    XR_DCHECK(ctx != NULL, "emit_ptr_spill_writeback: NULL ctx");
    int32_t pos = ctx->cur_ra_pos;
    for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
        if (ctx->func->vregs[v].rep != XR_REP_PTR)
            continue;
        int8_t ri = xra_reg_at_pos(ctx->xra, v, pos);
        if (ri < 0)
            ri = xra_reg_at_pos(ctx->xra, v, pos + 1);
        if (ri < 0)
            continue;  // not in a register

        int16_t slot = ctx->xra->valloc[v].spill;
        if (slot < 0) {
            // No spill slot assigned — allocate one dynamically
            slot = (int16_t) ctx->xra->nspill++;
            ctx->xra->valloc[v].spill = slot;
        }
        if (slot >= 0 && slot < 32 && ri >= 0 && ri < MAX_PHYS_REGS) {
            A64Reg reg = alloc_regs[ri];
            int32_t offset = SPILL_BASE + slot * 8;
            a64_buf_emit(&ctx->buf, a64_str(reg, A64_SP, offset));
        }
    }
}

// Push current FP onto jit_frame_stack before a cross-function call.
// GC walks this stack to scan caller frames' spill slots.
// Uses x16/x17 as scratch (caller must not hold live values there).
void emit_jit_frame_push(CodegenCtx *ctx) {
    XR_DCHECK(ctx != NULL, "emit_jit_frame_push: NULL ctx");
    // LDR W16, [X28, #depth_offset]
    a64_buf_emit(&ctx->buf, a64_ldr_w(SCRATCH_REG, JIT_CTX_REG, XM_JIT_FRAME_DEPTH_OFFSET));
    // X17 = X28 + depth * 8  (byte offset into frame_stack)
    a64_buf_emit(&ctx->buf, a64_add_lsl(SCRATCH_REG2, JIT_CTX_REG, SCRATCH_REG, 3));
    // STR FP, [X17, #stack_offset]
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, SCRATCH_REG2, XM_JIT_FRAME_STACK_OFFSET));
    // depth++
    a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG, SCRATCH_REG, 1));
    // STR W16, [X28, #depth_offset]
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XM_JIT_FRAME_DEPTH_OFFSET));
}

// Pop from jit_frame_stack after a cross-function call returns.
void emit_jit_frame_pop(CodegenCtx *ctx) {
    XR_DCHECK(ctx != NULL, "emit_jit_frame_pop: NULL ctx");
    // LDR W16, [X28, #depth_offset]
    a64_buf_emit(&ctx->buf, a64_ldr_w(SCRATCH_REG, JIT_CTX_REG, XM_JIT_FRAME_DEPTH_OFFSET));
    // depth--
    a64_buf_emit(&ctx->buf, a64_sub_imm(SCRATCH_REG, SCRATCH_REG, 1));
    // STR W16, [X28, #depth_offset]
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XM_JIT_FRAME_DEPTH_OFFSET));
}

// Build heap-allocated XrStackMapTable from collected entries.
// Caller takes ownership (must free with xr_free).
static XrStackMapTable *build_stack_map_table(CodegenCtx *ctx, uint32_t frame_size) {
    uint32_t n = ctx->nsmap;
    if (n == 0)
        return NULL;

    size_t sz = sizeof(XrStackMapTable) + n * sizeof(XrStackMapEntry);
    XrStackMapTable *table = (XrStackMapTable *) xr_malloc(sz);
    table->magic = XR_STACK_MAP_MAGIC;
    table->count = n;
    table->frame_size = frame_size;
    memcpy(table->entries, ctx->smap_entries, n * sizeof(XrStackMapEntry));
    return table;
}

/* ========== Phi Resolution (parallel copy) ========== */

// Emit parallel copies for edge from→target using XraResult (pure lookup).
// No allocation calls — all mappings come from the independent regalloc pass.
static void emit_edge_copies(CodegenCtx *ctx, XmBlock *target, XmBlock *from) {
    XR_DCHECK(ctx != NULL, "emit_edge_copies: NULL ctx");
    XR_DCHECK(target != NULL, "emit_edge_copies: NULL target");
    typedef struct {
        A64Reg dst, src;
        bool done, is_fp, is_reload;
        int16_t spill_slot;
    } Copy;
    Copy copies[64];
    uint32_t n = 0;

    if (ctx->xra) {
        XraEdgeCopy ec[64];
        uint32_t nc = xra_edge_copies(ctx->xra, ctx->func, target, from, ec, 64);
        for (uint32_t i = 0; i < nc && n < 64; i++) {
            A64Reg d = ec[i].is_fp ? alloc_fp_regs[ec[i].dst_idx] : alloc_regs[ec[i].dst_idx];
            if (ec[i].is_reload) {
                /* Spill-to-reg reload: add to copies[] for ordering, not emitted immediately.
                 * A reload that writes dst must come AFTER any pending reg-to-reg copy
                 * that uses dst as its source, to avoid clobbering the source value. */
                copies[n++] = (Copy) {d, A64_XZR, false, ec[i].is_fp, true, ec[i].spill_slot};
                continue;
            }
            A64Reg s = ec[i].is_fp ? alloc_fp_regs[ec[i].src_idx] : alloc_regs[ec[i].src_idx];
            if (d != s)
                copies[n++] = (Copy) {d, s, false, ec[i].is_fp, false, 0};
        }
    }

    if (n == 0)
        return;

// --- Emit copies+reloads: topological sort + cycle breaking ---
// A copy/reload i is "blocked" if its dst is the src of a pending reg-to-reg copy j.
// Reloads have no register source, so they can block others but are never cycle members.
#define EMIT_COPY(c)                                                                               \
    do {                                                                                           \
        if ((c).is_reload) {                                                                       \
            int32_t _off = SPILL_BASE + (c).spill_slot * 8;                                        \
            if ((c).is_fp)                                                                         \
                a64_buf_emit(&ctx->buf, a64_ldr_fp((c).dst, A64_SP, _off));                        \
            else                                                                                   \
                a64_buf_emit(&ctx->buf, a64_ldr((c).dst, A64_SP, _off));                           \
        } else {                                                                                   \
            if ((c).is_fp)                                                                         \
                a64_buf_emit(&ctx->buf, a64_fmov((c).dst, (c).src));                               \
            else                                                                                   \
                a64_buf_emit(&ctx->buf, a64_mov((c).dst, (c).src));                                \
        }                                                                                          \
    } while (0)

    // Phase 1: emit non-conflicting copies (topological order)
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
                /* copies[i].dst must not be the src of any pending reg-to-reg copy.
                 * Reloads have no register source (src=XZR), so skip them as blockers. */
                if (!copies[j].is_reload && copies[j].is_fp == copies[i].is_fp &&
                    copies[j].src == copies[i].dst) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) {
                EMIT_COPY(copies[i]);
                copies[i].done = true;
                progress = true;
            }
        }
    }

    // Phase 2: break remaining cycles with scratch register.
    //
    // For a cycle A←B, B←C, C←A (meaning A:=B, B:=C, C:=A):
    //   1. scratch = A  (save one value in the cycle)
    //   2. A = B        (safe: A's old value is in scratch)
    //   3. B = C        (safe: B's old value already consumed by step 2)
    //   4. C = scratch  (close cycle: C gets A's old value)
    //
    // Key insight: after saving A to scratch and executing A=B, we follow
    // the chain by finding the copy whose src equals the dst we just wrote.
    // Each step is safe because the source was already read by a prior copy.
    for (uint32_t i = 0; i < n; i++) {
        if (copies[i].done)
            continue;

        /* Reloads have no register source — they cannot be cycle members.
         * If a reload reaches phase 2 undone (shouldn't happen in practice),
         * emit it directly rather than treating it as a cycle entry. */
        if (copies[i].is_reload) {
            EMIT_COPY(copies[i]);
            copies[i].done = true;
            continue;
        }

        // Save copies[i].dst to scratch — it's about to be overwritten
        A64Reg cycle_start = copies[i].dst;
        bool cycle_fp = copies[i].is_fp;
        if (cycle_fp)
            a64_buf_emit(&ctx->buf, a64_fmov_to_gpr(SCRATCH_REG, cycle_start));
        else
            a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, cycle_start));

        // Emit copies along the cycle chain
        EMIT_COPY(copies[i]);
        copies[i].done = true;
        A64Reg last_src = copies[i].src;

        bool found = true;
        while (found) {
            found = false;
            for (uint32_t j = 0; j < n; j++) {
                if (copies[j].done)
                    continue;
                if (copies[j].is_fp != cycle_fp)
                    continue;
                // Find the copy whose dst == last_src
                // (that copy's dst was just read as a source, so writing it is safe)
                if (copies[j].dst == last_src) {
                    if (copies[j].src == cycle_start) {
                        // Cycle closes: src needs the saved old value
                        if (cycle_fp)
                            a64_buf_emit(&ctx->buf, a64_fmov_from_gpr(copies[j].dst, SCRATCH_REG));
                        else
                            a64_buf_emit(&ctx->buf, a64_mov(copies[j].dst, SCRATCH_REG));
                    } else {
                        EMIT_COPY(copies[j]);
                    }
                    last_src = copies[j].src;
                    copies[j].done = true;
                    found = true;
                    break;
                }
            }
        }
    }
#undef EMIT_COPY
}

/* ========== CMP+BR Fusion (SSA Combine) ========== */

/*
 * When a block ends with BR and the condition vreg is produced by a
 * comparison instruction (XM_LT..XM_FLE) with no other uses, we can
 * fuse the CMP and branch: skip the CSET in the comparison, and emit
 * B.cc (conditional branch) instead of CBZ in the terminator.
 *
 * Saves 1 instruction per fused comparison+branch pair:
 *   Before: CMP a,b; CSET dst,cc; CBZ dst,false  (3 instructions)
 *   After:  CMP a,b; B.inverted_cc false           (2 instructions)
 */

/* Map Xm comparison op to inverted A64Cond for the false branch.
 * Returns A64_CC_AL if the op is not a fusible comparison. */
static A64Cond xm_cmp_to_false_cc(uint16_t op, bool *is_float) {
    *is_float = false;
    switch (op) {
        case XM_LT:
            return A64_CC_GE;
        case XM_LE:
            return A64_CC_GT;
        case XM_GT:
            return A64_CC_LE;
        case XM_GE:
            return A64_CC_LT;
        case XM_EQ:
            return A64_CC_NE;
        case XM_NE:
            return A64_CC_EQ;
        case XM_FEQ:
            *is_float = true;
            return A64_CC_NE;
        case XM_FNE:
            *is_float = true;
            return A64_CC_EQ;
        case XM_FLT:
            *is_float = true;
            return A64_CC_PL;
        case XM_FLE:
            *is_float = true;
            return A64_CC_HI;
        default:
            return A64_CC_AL;
    }
}

// Check if an Xm op may clobber ARM64 condition flags when emitted.
static bool xm_op_clobbers_flags(uint16_t op) {
    // Comparisons emit CMP/FCMP + CSET
    if (op >= XM_EQ && op <= XM_FLE)
        return true;
    // SELECT_COND emits CMP_IMM
    if (op == XM_SELECT_COND)
        return true;
    // Guards emit CMP internally
    if (op == XM_GUARD_TAG || op == XM_GUARD_CLASS || op == XM_GUARD_KLASS ||
        op == XM_GUARD_NONNULL || op == XM_GUARD_SHAPE || op == XM_GUARD_BOUNDS)
        return true;
    // Safepoint emits CMP for reductions check
    if (op == XM_SAFEPOINT)
        return true;
    // Calls: callee may clobber NZCV
    if (op == XM_CALL || op == XM_CALL_C || op == XM_CALL_C_LEAF || op == XM_CALL_KNOWN ||
        op == XM_CALL_KNOWN_REG || op == XM_CALL_SELF_DIRECT || op == XM_CALL_DIRECT)
        return true;
    // ALLOC: inline bump-pointer has CMP for limit check
    if (op == XM_ALLOC)
        return true;
    // RT_* helpers call into C runtime
    if (op >= XM_RT_ADD && op <= XM_RT_ISNULL)
        return true;
    // BOX/UNBOX may involve runtime calls
    if (op == XM_BOX_I64 || op == XM_BOX_F64 || op == XM_UNBOX_I64 || op == XM_UNBOX_F64)
        return true;
    return false;
}

/*
 * Pre-scan a block for CMP+BR fusion opportunity.
 * Sets ctx->fused_cmp_ref if fusion is possible.
 */
static void prescan_fuse_cmp_br(CodegenCtx *ctx, XmBlock *blk) {
    XR_DCHECK(ctx != NULL, "prescan_fuse_cmp_br: NULL ctx");
    XR_DCHECK(blk != NULL, "prescan_fuse_cmp_br: NULL blk");
    ctx->fused_cmp_ref = XM_NONE;

    if (blk->jmp.type != XM_JMP_BR)
        return;
    XmRef cond = blk->jmp.arg;
    if (!xm_ref_is_vreg(cond))
        return;

    // Find the defining instruction of cond in this block
    int def_idx = -1;
    for (int i = (int) blk->nins - 1; i >= 0; i--) {
        if (blk->ins[i].dst == cond) {
            def_idx = i;
            break;
        }
    }
    if (def_idx < 0)
        return;

    // Check if it's a fusible comparison
    bool is_float;
    A64Cond fcc = xm_cmp_to_false_cc(blk->ins[def_idx].op, &is_float);
    if (fcc == A64_CC_AL)
        return;

    // Check no flag-clobbering instruction after the comparison
    for (uint32_t i = (uint32_t) def_idx + 1; i < blk->nins; i++) {
        if (xm_op_clobbers_flags(blk->ins[i].op))
            return;
    }

    // Check cond vreg has no other uses in this block's instructions
    for (uint32_t i = 0; i < blk->nins; i++) {
        if ((int) i == def_idx)
            continue;
        XmIns *ins = &blk->ins[i];
        if ((xm_ref_is_vreg(ins->args[0]) && ins->args[0] == cond) ||
            (xm_ref_is_vreg(ins->args[1]) && ins->args[1] == cond))
            return;
    }

    // Check no use in successor phis
    XmBlock *succs[2] = {blk->s1, blk->s2};
    for (int s = 0; s < 2; s++) {
        if (!succs[s])
            continue;
        for (XmPhi *phi = succs[s]->phis; phi; phi = phi->next) {
            for (uint32_t p = 0; p < phi->narg; p++) {
                if (xm_ref_is_vreg(phi->args[p]) && phi->args[p] == cond)
                    return;
            }
        }
    }

    // Fusion is safe
    ctx->fused_cmp_ref = cond;
    ctx->fused_false_cc = fcc;
    ctx->fused_is_float = is_float;
}

/* ========== Constant Materialization ========== */

void emit_load_const(CodegenCtx *ctx, A64Reg rd, XmRef ref) {
    XR_DCHECK(ctx != NULL, "emit_load_const: NULL ctx");
    if (xm_ref_is_const(ref)) {
        uint32_t idx = XM_REF_INDEX(ref);
        XR_DCHECK(idx < ctx->func->nconst, "assertion failed");
        XmConst *c = &ctx->func->consts[idx];
        a64_load_imm64(&ctx->buf, rd, (uint64_t) c->val.raw);
    } else {
        A64Reg src = xra_get(ctx, ref);
        if (src != rd) {
            a64_buf_emit(&ctx->buf, a64_mov(rd, src));
        }
    }
}

/* ========== Spill/Reload Support ========== */

/*
 * Resolve an Xm arg ref to a physical register, handling spilled vregs.
 * If the vreg was spilled (vreg_spill >= 0), emits LDR from spill slot
 * into scratch_reg and returns scratch_reg. Otherwise returns vreg_reg[].
 * For rematerializable vregs (constants), re-emits the constant load.
 */
A64Reg xra_arg(CodegenCtx *ctx, XmRef ref, A64Reg scratch_reg) {
    XR_DCHECK(ctx != NULL, "xra_arg: NULL ctx");
    if (xm_ref_is_none(ref))
        return A64_XZR;
    if (!xm_ref_is_vreg(ref))
        return A64_XZR;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= ctx->xra->nvreg)
        return A64_XZR;

    /* Check segment register at current position: if vreg has a register here,
     * no reload needed even if it has a spill slot (due to splitting) */
    int8_t blk_reg = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos);
    if (blk_reg < 0)
        blk_reg = xra_reg_at_pos(ctx->xra, idx, ctx->cur_ra_pos + 1);
    if (blk_reg >= 0)
        return xra_get(ctx, ref);

    int16_t slot = xra_vreg_spill(ctx->xra, idx);

    // Real spill: reload from stack
    if (slot >= 0) {
        int32_t offset = SPILL_BASE + slot * 8;
        if (idx < ctx->func->nvreg && ctx->func->vregs[idx].rep == XR_REP_F64) {
            // FP spill reload: LDR Dt, [SP, #offset]
            A64Reg fp_reg = xra_get(ctx, ref);
            a64_buf_emit(&ctx->buf, a64_ldr_fp(fp_reg, A64_SP, offset));
            return fp_reg;
        }
        a64_buf_emit(&ctx->buf, a64_ldr(scratch_reg, A64_SP, offset));
        return scratch_reg;
    }

    // Rematerializable: re-emit instruction (constant / LOAD_CORO_BYTE / I2F / F2I)
    if (slot == XRA_SPILL_REMAT) {
        if (emit_remat(ctx, idx, scratch_reg))
            return scratch_reg;
    }

    // Not spilled — use per-block vreg_reg mapping
    return xra_get(ctx, ref);
}

/*
 * After defining a spilled vreg, emit STR to spill slot.
 * Called after each instruction that writes to a spilled dst.
 */
void xra_maybe_spill(CodegenCtx *ctx, XmRef dst_ref) {
    XR_DCHECK(ctx != NULL, "xra_maybe_spill: NULL ctx");
    if (!xm_ref_is_vreg(dst_ref))
        return;
    uint32_t v = XM_REF_INDEX(dst_ref);
    if (v >= ctx->xra->nvreg)
        return;
    int16_t slot = xra_vreg_spill(ctx->xra, v);
    if (slot < 0)
        return;  // not spilled or remat — nothing to store

    // Get the register that holds the just-defined value
    A64Reg reg = xra_get(ctx, dst_ref);

    int32_t offset = SPILL_BASE + slot * 8;
    if (v < ctx->func->nvreg && ctx->func->vregs[v].rep == XR_REP_F64) {
        // FP spill store: STR Dt, [SP, #offset]
        a64_buf_emit(&ctx->buf, a64_str_fp(reg, A64_SP, offset));
        return;
    }
    a64_buf_emit(&ctx->buf, a64_str(reg, A64_SP, offset));
}

/* ========== Operand Resolution ========== */

// Resolve an Xm operand to a physical register. If the operand is a const
// pool ref (not a vreg), materialize it into scratch_reg and return that.
// For vreg refs, handles spill reload via xra_arg().
A64Reg xra_operand(CodegenCtx *ctx, XmRef ref, A64Reg scratch_reg) {
    if (xm_ref_is_const(ref)) {
        uint32_t idx = XM_REF_INDEX(ref);
        if (idx < ctx->func->nconst) {
            a64_load_imm64(&ctx->buf, scratch_reg, (uint64_t) ctx->func->consts[idx].val.raw);
            return scratch_reg;
        }
        return A64_XZR;
    }
    return xra_arg(ctx, ref, scratch_reg);
}

/* Check if a CONST_I64 vreg is fully folded into add_imm/sub_imm by all
 * its users in the given block. If so, the CONST_I64 emit can be skipped. */
static bool const_i64_fully_folded(CodegenCtx *ctx, XmBlock *blk, XmRef dst) {
    if (!xm_ref_is_vreg(dst))
        return false;
    uint32_t vi = XM_REF_INDEX(dst);
    if (vi >= ctx->func->nvreg)
        return false;
    XmIns *def = ctx->func->vregs[vi].def;
    if (!def || def->op != XM_CONST_I64)
        return false;
    if (!xm_ref_is_const(def->args[0]))
        return false;
    uint32_t ci = XM_REF_INDEX(def->args[0]);
    if (ci >= ctx->func->nconst)
        return false;
    int64_t val = ctx->func->consts[ci].val.i64;
    if (val < 0 || val > 4095)
        return false;

    /* Scan block: every use of vi must be in an ADD/SUB arg position
     * where the other arg is NOT also this const (to avoid double-fold). */
    uint32_t total = 0, folded = 0;
    for (uint32_t i = 0; i < blk->nins; i++) {
        XmIns *ins = &blk->ins[i];
        for (int a = 0; a < 2; a++) {
            if (!xm_ref_is_vreg(ins->args[a]))
                continue;
            if (XM_REF_INDEX(ins->args[a]) != vi)
                continue;
            total++;
            if (ins->op == XM_ADD || ins->op == XM_SUB)
                folded++;
        }
    }
    // Also check terminator arg
    if (xm_ref_is_vreg(blk->jmp.arg) && XM_REF_INDEX(blk->jmp.arg) == vi)
        total++;
    return total > 0 && total == folded;
}

/* Instruction emission moved to xm_codegen_ins.c (table-driven dispatch).
 * The following block-level emit logic calls a64_emit_xm_ins() for each ins. */

/* ========== Prologue / Epilogue ========== */

static void emit_prologue(CodegenCtx *ctx) {
    // SUB SP, SP, #frame_size (placeholder, patched later)
    XR_DCHECK(ctx->nsub_patches < 8, "assertion failed");
    ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, JIT_FRAME_BASE));
    // STP x29, x30, [SP, #0]
    a64_buf_emit(&ctx->buf, a64_stp(A64_FP, A64_LR, A64_SP, 0));
    // Save callee-saved registers x19-x28
    a64_buf_emit(&ctx->buf, a64_stp(A64_X19, A64_X20, A64_SP, 16));
    add_cs_patch(ctx, 0);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X21, A64_X22, A64_SP, 32));
    add_cs_patch(ctx, 1);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X23, A64_X24, A64_SP, 48));
    add_cs_patch(ctx, 2);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X25, A64_X26, A64_SP, 64));
    add_cs_patch(ctx, 3);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X27, A64_X28, A64_SP, 80));
    // Save FP callee-saved registers d8-d15
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
    // MOV x19, x0  (save coro pointer)
    a64_buf_emit(&ctx->buf, a64_mov(CORO_REG, A64_X0));
    // LDR x28, [x19, #jit_ctx_offset]  (load per-Worker JIT scratch pointer)
    a64_buf_emit(&ctx->buf, a64_ldr(JIT_CTX_REG, CORO_REG, XM_CORO_JIT_CTX_OFFSET));

    // Store stack_map_ptr into frame from jit_ctx->active_stack_map (set by xm_jit_call)
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, A64_FP, FRAME_SMAP_PTR_OFFSET));
    // Initialize safepoint_id = UINT32_MAX (no active safepoint)
    // Write to both frame (for future FP chain walking) and jit_ctx (for current GC)
    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, 0xFFFF, 0));
    a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG, 0xFFFF, 16));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, A64_FP, FRAME_SMAP_ID_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));
    // Save JIT frame SP for GC access: jit_ctx->jit_frame_sp = FP
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XM_JIT_FRAME_SP_OFFSET));
    // Load guard page pointer into x20 from jit_ctx->safepoint_page.
    // JIT back-edges emit LDR WZR,[x20] which faults when page is armed.
    a64_buf_emit(&ctx->buf,
                 a64_ldr(SAFEPT_PAGE_REG, JIT_CTX_REG, (int32_t) XM_JIT_SAFEPOINT_PAGE_OFFSET));

    // Load params from args array (x1 = args pointer)
    // Save args ptr to x17 first since x1 may be overwritten by param load
    uint32_t nparams = ctx->func->num_params;
    if (nparams > 0) {
        a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG2, A64_X1));
        for (uint32_t i = 0; i < nparams; i++) {
            bool is_fp = (i < ctx->func->nvreg && ctx->func->vregs[i].rep == XR_REP_F64);
            // Drive loading from the position the param actually occupies
            // at function entry (pos 0). xra_vreg_first_reg returns the
            // first reg of any segment, so when the leading segment is
            // spill it would point at a later post-reload reg the RA has
            // not yet committed; the spill slot would be left
            // uninitialized.
            int8_t ri = xra_reg_at_pos(ctx->xra, i, 0);
            int16_t slot = xra_vreg_spill(ctx->xra, i);
            bool live0 = xra_vreg_live_at(ctx->xra, i, 0);
            if (!live0)
                continue;  // dead at entry

            int32_t arg_off = (int32_t) (i * 8);
            if (ri >= 0) {
                // Leading segment is a reg: load there, mirror to spill
                // slot (if any) so later spill segments reload correctly.
                if (is_fp) {
                    A64Reg dst = alloc_fp_regs[ri];
                    a64_buf_emit(&ctx->buf, a64_ldr_fp(dst, SCRATCH_REG2, arg_off));
                    if (slot >= 0)
                        a64_buf_emit(&ctx->buf, a64_str_fp(dst, A64_SP, SPILL_BASE + slot * 8));
                } else {
                    A64Reg dst = alloc_regs[ri];
                    a64_buf_emit(&ctx->buf, a64_ldr(dst, SCRATCH_REG2, arg_off));
                    if (slot >= 0)
                        a64_buf_emit(&ctx->buf, a64_str(dst, A64_SP, SPILL_BASE + slot * 8));
                }
            } else if (slot >= 0) {
                // Leading segment is spill: seed the slot from args[i] via
                // SCRATCH_REG. SCRATCH_REG2 holds args_ptr. 8-byte LDR/STR
                // works for both i64 and f64 bit patterns.
                a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG2, arg_off));
                a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, A64_SP, SPILL_BASE + slot * 8));
            }
        }
        // Init vreg_runtime_tags for TAGGED params from param_tags[].
        // Params are vregs 0..n-1 by construction, so vreg index == param index.
        // param_tags[] is set by xm_jit_call (interp→JIT) or
        // xr_jit_call_func / CALL_KNOWN codegen (JIT→JIT).
        for (uint32_t i = 0; i < nparams && i < 8; i++) {
            if (i >= ctx->func->nvreg || i >= XR_JIT_MAX_VREG_TAGS)
                continue;
            if (ctx->func->vregs[i].rep != XR_REP_TAGGED)
                continue;
            int32_t pt_off = (int32_t) (XM_JIT_PARAM_TAGS_OFFSET + i * 8);
            int32_t rt_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) i;
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, pt_off));
            a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG, JIT_CTX_REG, rt_off));
        }
    }
}

// Fast prologue: same frame setup as normal prologue, but NO param loading.
// Used as BL target for register-passing self-calls where args are already
// in the correct param registers.
static void emit_fast_prologue(CodegenCtx *ctx) {
    // SUB SP, SP, #frame_size (placeholder, patched later — same as normal)
    XR_DCHECK(ctx->nsub_patches < 8, "assertion failed");
    ctx->frame_patch_sub[ctx->nsub_patches++] = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, JIT_FRAME_BASE));
    // STP x29, x30, [SP, #0]
    a64_buf_emit(&ctx->buf, a64_stp(A64_FP, A64_LR, A64_SP, 0));
    // Save callee-saved registers x19-x28
    a64_buf_emit(&ctx->buf, a64_stp(A64_X19, A64_X20, A64_SP, 16));
    add_cs_patch(ctx, 0);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X21, A64_X22, A64_SP, 32));
    add_cs_patch(ctx, 1);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X23, A64_X24, A64_SP, 48));
    add_cs_patch(ctx, 2);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X25, A64_X26, A64_SP, 64));
    add_cs_patch(ctx, 3);
    a64_buf_emit(&ctx->buf, a64_stp(A64_X27, A64_X28, A64_SP, 80));
    // Save FP callee-saved registers d8-d15
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
    // MOV x19, x0  (save coro pointer)
    a64_buf_emit(&ctx->buf, a64_mov(CORO_REG, A64_X0));
    // LDR x28, [x19, #jit_ctx_offset]  (load per-Worker JIT scratch pointer)
    a64_buf_emit(&ctx->buf, a64_ldr(JIT_CTX_REG, CORO_REG, XM_CORO_JIT_CTX_OFFSET));

    // Store stack_map_ptr into frame from jit_ctx->active_stack_map
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, A64_FP, FRAME_SMAP_PTR_OFFSET));
    // Initialize safepoint_id = UINT32_MAX (both frame + jit_ctx)
    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, 0xFFFF, 0));
    a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG, 0xFFFF, 16));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, A64_FP, FRAME_SMAP_ID_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));
    // Save JIT frame SP for GC access
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XM_JIT_FRAME_SP_OFFSET));
    // Load guard page pointer into x20 from jit_ctx->safepoint_page
    a64_buf_emit(&ctx->buf,
                 a64_ldr(SAFEPT_PAGE_REG, JIT_CTX_REG, (int32_t) XM_JIT_SAFEPOINT_PAGE_OFFSET));

    // Move params from fixed ABI registers (alloc_regs[0..N]) to
    // regalloc-assigned registers. Both self-calls and cross-function
    // calls place args in alloc_regs[0..N] before BL fast_entry.
    uint32_t nparams = ctx->func->num_params;
    if (nparams > 0) {
        for (uint32_t i = 0; i < nparams && i < 8; i++) {
            bool is_fp = (i < ctx->func->nvreg && ctx->func->vregs[i].rep == XR_REP_F64);
            int8_t ri = xra_vreg_first_reg(ctx->xra, i);
            if (ri < 0)
                continue;
            if (is_fp) {
                A64Reg dst = alloc_fp_regs[ri];
                A64Reg src = alloc_fp_regs[i];
                if (dst != src)
                    a64_buf_emit(&ctx->buf, a64_fmov(dst, src));
            } else {
                A64Reg dst = alloc_regs[ri];
                A64Reg src = alloc_regs[i];
                if (dst != src)
                    a64_buf_emit(&ctx->buf, a64_mov(dst, src));
            }
        }
    }
}

void emit_epilogue(CodegenCtx *ctx) {
    // Restore callee-saved registers x19-x28
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X19, A64_X20, A64_SP, 16));
    add_cs_patch(ctx, 0);
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X21, A64_X22, A64_SP, 32));
    add_cs_patch(ctx, 1);
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X23, A64_X24, A64_SP, 48));
    add_cs_patch(ctx, 2);
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X25, A64_X26, A64_SP, 64));
    add_cs_patch(ctx, 3);
    a64_buf_emit(&ctx->buf, a64_ldp(A64_X27, A64_X28, A64_SP, 80));
    // Restore FP callee-saved registers d8-d15
    add_cs_patch(ctx, 4);
    a64_buf_emit(&ctx->buf, a64_ldp_fp(8, 9, A64_SP, 96));
    add_cs_patch(ctx, 5);
    a64_buf_emit(&ctx->buf, a64_ldp_fp(10, 11, A64_SP, 112));
    add_cs_patch(ctx, 6);
    a64_buf_emit(&ctx->buf, a64_ldp_fp(12, 13, A64_SP, 128));
    add_cs_patch(ctx, 7);
    a64_buf_emit(&ctx->buf, a64_ldp_fp(14, 15, A64_SP, 144));
    // Restore FP and LR from fixed offset 0 (no post-index needed)
    a64_buf_emit(&ctx->buf, a64_ldp(A64_FP, A64_LR, A64_SP, 0));
    // ADD SP, SP, #frame_size (placeholder, patched later)
    // Using add_imm instead of ldp_post to support frames >504 bytes
    XR_DCHECK(ctx->nadd_patches < 32, "assertion failed");
    ctx->frame_patch_add[ctx->nadd_patches++] = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_add_imm(A64_SP, A64_SP, JIT_FRAME_BASE));
}

/* ========== Block Emission ========== */

static void emit_block(CodegenCtx *ctx, uint32_t block_idx) {
    XmBlock *blk = ctx->func->blocks[block_idx];

    // Set current block and RA position for segment lookups
    ctx->cur_blk_id = blk->id;
    ctx->cur_ra_pos = (ctx->xra && blk->id < ctx->xra->nblk) ? ctx->xra->blk_start[blk->id] : 0;

    // Reset gap-move overrides at block entry
    if (ctx->vreg_override && ctx->xra)
        memset(ctx->vreg_override, -128, ctx->xra->nvreg);

    // Advance gap_move_cursor to the first gap move in this block
    if (ctx->xra && ctx->xra->gap_moves) {
        while (ctx->gap_move_cursor < ctx->xra->ngap_move &&
               ctx->xra->gap_moves[ctx->gap_move_cursor].gap_blk < blk->id)
            ctx->gap_move_cursor++;
    }

    // Record block start offset
    ctx->block_offsets[blk->id] = ctx->buf.count;

    // Snapshot regalloc state at loop headers for OSR entry.
    // Skip for functions with coroutine deopt (AWAIT/SCOPE_EXIT): OSR + deopt
    // can't correctly recover full interpreter state, causing double execution
    // of side-effecting code (spawns, array pushes).
    if (blk->is_loop_header && ctx->nosr_snap < XM_MAX_OSR_ENTRIES && !ctx->func->has_coro_deopt) {
        OsrSnapshot *snap = &ctx->osr_snaps[ctx->nosr_snap++];
        snap->block_id = blk->id;
        snap->block_offset = ctx->buf.count;
    }

    // Pre-scan for CMP+BR fusion opportunity (SSA Combine)
    prescan_fuse_cmp_br(ctx, blk);

    // Emit all instructions with gap moves at split points
    for (uint32_t i = 0; i < blk->nins; i++) {
        // Advance RA position: blk_start + 2 (phi/label) + i*2 (even = inst)
        ctx->cur_ra_pos = (ctx->xra && blk->id < ctx->xra->nblk)
                              ? ctx->xra->blk_start[blk->id] + 2 + (int32_t) i * 2
                              : 0;
        emit_gap_moves_before(ctx, i);
        ctx->cur_ins_idx = i;
        // Skip CONST_I64 whose value is fully folded into add_imm/sub_imm
        if (blk->ins[i].op == XM_CONST_I64 && const_i64_fully_folded(ctx, blk, blk->ins[i].dst)) {
            continue;
        }
        a64_emit_xm_ins(ctx, &blk->ins[i]);

        // Spill store: if dst vreg has a spill slot, write to stack
        xra_maybe_spill(ctx, blk->ins[i].dst);

        // Exception check after call-sites
        {
            uint16_t op = blk->ins[i].op;
            if (op == XM_CALL_C || op == XM_CALL_DIRECT || op == XM_CALL_SELF_DIRECT ||
                op == XM_CALL_KNOWN_REG) {
                if (blk->exception_handler) {
                    // In try block: branch to catch handler
                    a64_buf_emit(&ctx->buf,
                                 a64_ldr(SCRATCH_REG, JIT_CTX_REG, XM_JIT_EXCEPTION_OFFSET));
                    add_patch(ctx, PATCH_CBNZ, blk->exception_handler->id, SCRATCH_REG);
                    a64_buf_emit(&ctx->buf, a64_nop());
                } else if (blk->ins[i].flags & XM_FLAG_MAY_THROW) {
                    // No catch handler but may throw: deopt to interpreter
                    // so exception propagates through VM try-catch frames
                    a64_buf_emit(&ctx->buf,
                                 a64_ldr(SCRATCH_REG, JIT_CTX_REG, XM_JIT_EXCEPTION_OFFSET));
                    add_patch(ctx, PATCH_DEOPT_CBNZ, 0, SCRATCH_REG);
                    a64_buf_emit(&ctx->buf, a64_nop());
                    ctx->has_deopt = true;
                }
            }
        }
    }

    // Emit terminator
    switch (blk->jmp.type) {
        case XM_JMP_JMP: {
            XR_DCHECK(blk->s1 != NULL, "assertion failed");
            emit_edge_copies(ctx, blk->s1, blk);
            // Fall-through: skip branch if target is the next block
            bool jmp_is_next =
                (block_idx + 1 < ctx->func->nblk) && (ctx->func->blocks[block_idx + 1] == blk->s1);
            if (!jmp_is_next) {
                add_patch(ctx, PATCH_B, blk->s1->id, A64_XZR);
                a64_buf_emit(&ctx->buf, a64_nop());  // placeholder
            }
            break;
        }
        case XM_JMP_BR: {
            XR_DCHECK(blk->s1 != NULL && blk->s2 != NULL, "assertion failed");

            if (!xm_ref_is_none(ctx->fused_cmp_ref)) {
                // Fused CMP+BR: use B.cc instead of CBZ.
                // CMP/FCMP already emitted (CSET was skipped).

                if (blk->is_loop_header) {
                    /* Loop header layout optimization:
                     * Branch cold path (exit) directly to exit block,
                     * keep hot path (body) inline for potential fallthrough.
                     * Saves 1-2 branches vs the default layout. */
                    // B.cc → exit block (cold, false path = s2)
                    emit_edge_copies(ctx, blk->s2, blk);
                    add_patch(ctx, PATCH_B_COND, blk->s2->id, (A64Reg) ctx->fused_false_cc);
                    a64_buf_emit(&ctx->buf, a64_nop());  // placeholder

                    // Hot path (true = s1 = body): edge copies, B/fallthrough
                    emit_edge_copies(ctx, blk->s1, blk);
                    bool s1_is_next = (block_idx + 1 < ctx->func->nblk) &&
                                      (ctx->func->blocks[block_idx + 1] == blk->s1);
                    if (!s1_is_next) {
                        add_patch(ctx, PATCH_B, blk->s1->id, A64_XZR);
                        a64_buf_emit(&ctx->buf, a64_nop());
                    }
                } else {
                    // Non-loop: default layout (true inline, false after B.cc)
                    uint32_t bcond_pos = ctx->buf.count;
                    a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for B.cc

                    // True path: edge copies for s1, then B s1
                    emit_edge_copies(ctx, blk->s1, blk);
                    add_patch(ctx, PATCH_B, blk->s1->id, A64_XZR);
                    a64_buf_emit(&ctx->buf, a64_nop());  // placeholder

                    // False path label (B.cc lands here)
                    uint32_t false_pos = ctx->buf.count;
                    int32_t bcond_offset = (int32_t) false_pos - (int32_t) bcond_pos;
                    ctx->buf.code[bcond_pos] = a64_b_cond(ctx->fused_false_cc, bcond_offset);

                    // False path: edge copies for s2, then B s2 (or fallthrough)
                    emit_edge_copies(ctx, blk->s2, blk);
                    bool s2_is_next = (block_idx + 1 < ctx->func->nblk) &&
                                      (ctx->func->blocks[block_idx + 1] == blk->s2);
                    if (!s2_is_next) {
                        add_patch(ctx, PATCH_B, blk->s2->id, A64_XZR);
                        a64_buf_emit(&ctx->buf, a64_nop());
                    }
                }
            } else {
                // Original path: CBZ on boolean condition
                A64Reg cond_reg = xra_get(ctx, blk->jmp.arg);

                // CBZ cond, false_path; [true copies]; B s1; false_path: [false copies]; B s2
                uint32_t cbz_pos = ctx->buf.count;
                a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for CBZ

                // True path: edge copies for s1, then B s1
                emit_edge_copies(ctx, blk->s1, blk);
                add_patch(ctx, PATCH_B, blk->s1->id, A64_XZR);
                a64_buf_emit(&ctx->buf, a64_nop());  // placeholder

                // False path label (CBZ lands here)
                uint32_t false_pos = ctx->buf.count;
                int32_t cbz_offset = (int32_t) false_pos - (int32_t) cbz_pos;
                ctx->buf.code[cbz_pos] = a64_cbz(cond_reg, cbz_offset);

                // False path: edge copies for s2, then B s2 (or fallthrough)
                emit_edge_copies(ctx, blk->s2, blk);
                bool s2_is_next = (block_idx + 1 < ctx->func->nblk) &&
                                  (ctx->func->blocks[block_idx + 1] == blk->s2);
                if (!s2_is_next) {
                    add_patch(ctx, PATCH_B, blk->s2->id, A64_XZR);
                    a64_buf_emit(&ctx->buf, a64_nop());
                }
            }
            break;
        }
        case XM_JMP_RET: {
            if (!xm_ref_is_none(blk->jmp.arg)) {
                A64Reg ret_reg = xra_get(ctx, blk->jmp.arg);
                uint32_t ret_idx = XM_REF_INDEX(blk->jmp.arg);
                bool is_fp =
                    (ret_idx < ctx->func->nvreg && ctx->func->vregs[ret_idx].rep == XR_REP_F64);
                // Return XrJitResult: x0=payload, x1=tag (ARM64 two-register struct).
                // call_c_stub captures both before restoring live regs, so callers
                // receive {payload, tag} without any memory side-channel.
                // Determine return value_tag (x1) from ctype
                uint8_t ret_vtag = VTAG_TAGGED;
                uint8_t ret_xr_tag = XR_TAG_I64;  // default
                if (ret_idx < ctx->func->nvreg) {
                    XmType rct = xm_ref_ctype(ctx->func, blk->jmp.arg);
                    ret_vtag = type_kind_to_vtag(rct.kind);
                    uint8_t vt = vtag_to_value_tag(ret_vtag);
                    if (vt != 0xFF) {
                        ret_xr_tag = vt;
                    } else if (ret_vtag == VTAG_TAGGED) {
                        // VTAG_TAGGED: type is only known at runtime.
                        // Use rep as hint, but fall back to 0xFF so the
                        // caller uses heuristic rather than assuming I64.
                        uint8_t mt = ctx->func->vregs[ret_idx].rep;
                        if (mt == XR_REP_F64)
                            ret_xr_tag = XR_TAG_F64;
                        else if (mt == XR_REP_PTR)
                            ret_xr_tag = XR_TAG_PTR;
                        else
                            ret_xr_tag = 0xFF;  // unknown: use heuristic
                    }
                }
                // Set x0 = payload FIRST: ret_reg may be x1, and setting x1=tag
                // below would clobber it before we copy to x0.
                // Also must precede emit_epilogue: ret_reg may be callee-saved.
                if (is_fp) {
                    a64_buf_emit(&ctx->buf, a64_fmov_to_gpr(A64_X0, ret_reg));
                } else if (ret_reg != A64_X0) {
                    a64_buf_emit(&ctx->buf, a64_mov(A64_X0, ret_reg));
                }
                // Set x1 = tag AFTER x0 is safely set.
                // All types including BOOL use a constant tag now.
                a64_buf_emit(&ctx->buf, a64_movz(A64_X1, (uint16_t) ret_xr_tag, 0));
            }
            emit_epilogue(ctx);
            a64_buf_emit(&ctx->buf, a64_ret());
            break;
        }
        case XM_JMP_UNREACHABLE:
            a64_buf_emit(&ctx->buf, 0xD4200020);  // BRK #1
            break;
        default:
            break;
    }
}

/* Safepoint stub removed: guard page safepoint uses global trampoline
 * via SIGSEGV signal handler instead of per-function BL stub. */

/* ========== Main Codegen Entry ========== */

XmCodegenResult xm_codegen_arm64(XmFunc *func, XmCodeAlloc *alloc) {
    XR_DCHECK(func != NULL, "xm_codegen_arm64: func is NULL");
    XR_DCHECK(alloc != NULL, "xm_codegen_arm64: alloc is NULL");
    XmCodegenResult result = {
        .code = NULL, .code_size = 0, .success = false, .error = NULL, .nosr = 0};

    // Validate consistency between hardcoded constants and XmTarget
    XR_DCHECK(xm_current_target != NULL, "assertion failed");
    XR_DCHECK(xm_current_target->ngpr == MAX_PHYS_REGS, "assertion failed");
    XR_DCHECK(xm_current_target->nfpr == MAX_FP_REGS, "assertion failed");
    XR_DCHECK(xm_current_target->max_vregs == MAX_VREGS, "assertion failed");
    XR_DCHECK(xm_current_target->scratch_gpr[0] == SCRATCH_REG, "assertion failed");
    XR_DCHECK(xm_current_target->scratch_gpr[1] == SCRATCH_REG2, "assertion failed");
    XR_DCHECK(xm_current_target->coro_reg == CORO_REG, "assertion failed");
    XR_DCHECK(xm_current_target->spill_base == SPILL_BASE, "assertion failed");
    XR_DCHECK(xm_current_target->frame_base == JIT_FRAME_BASE, "assertion failed");

    if (!func || !alloc || func->nblk == 0) {
        result.error = "invalid function or allocator";
        return result;
    }

    if (func->nvreg > MAX_VREGS) {
        result.error = "too many virtual registers";
        return result;
    }

    // Rebuild vreg def pointers (may be stale after optimization passes)
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

    CodegenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = func;
    ctx.alloc = alloc;
    ctx.npatch = 0;
    ctx.patches_cap = INIT_PATCHES;
    ctx.patches = (BranchPatch *) xr_malloc(INIT_PATCHES * sizeof(BranchPatch));
    ctx.cs_patches_cap = INIT_CS_PATCHES;
    ctx.cs_patches = (CsPatch *) xr_malloc(INIT_CS_PATCHES * sizeof(CsPatch));

    ctx.nsub_patches = 0;
    ctx.nadd_patches = 0;

    // Pre-RA: aggressive MOV coalescing (reduces vreg count)
    xm_coalesce(func);

    // Run register allocation
    ctx.xra = xra_run(func);

    /* LSRA may refuse compilation (e.g. when spill slot count exceeds the
     * GC stack-map bitmap width or the XrCoroutine suspend-bridge capacity).
     * Abort codegen before emitting any instructions so the interpreter
     * keeps executing this function. */
    if (ctx.xra && ctx.xra->had_error) {
        result.error = "regalloc refused: spill slot limit exceeded";
        xr_free(ctx.patches);
        xr_free(ctx.cs_patches);
        xra_result_free(ctx.xra);
        return result;
    }

    // Allocate gap-move override array (initialized per-block)
    if (ctx.xra && ctx.xra->nvreg > 0) {
        ctx.vreg_override = (int8_t *) xr_malloc(ctx.xra->nvreg * sizeof(int8_t));
        memset(ctx.vreg_override, -128, ctx.xra->nvreg);
    }

    // Block ids may be non-contiguous after optimization passes —
    // allocate block_offsets[] large enough to cover the maximum block id.
    uint32_t max_blk_id = 0;
    for (uint32_t i = 0; i < func->nblk; i++) {
        if (func->blocks[i]->id > max_blk_id)
            max_blk_id = func->blocks[i]->id;
    }
    ctx.block_offsets = (uint32_t *) xr_calloc(max_blk_id + 1, sizeof(uint32_t));
    ctx.nblock_offsets = max_blk_id + 1;

    // Memory estimate: 32 ARM64 insts per Xm ins + overhead.
    // CALL_DIRECT/CALL_KNOWN emit 50+ instructions (reg save/restore, frame push/pop,
    // safepoint bitmap store), so 8 was too conservative and caused buffer overflows.
    uint32_t total_xm_ins = 0;
    for (uint32_t i = 0; i < func->nblk; i++) {
        total_xm_ins += func->blocks[i]->nins + 4;
    }
    uint32_t alloc_size = (total_xm_ins * 32 + 256) * 4;
    alloc_size = (alloc_size + 4095) & ~(uint32_t) 4095;
    if (alloc_size < 8192)
        alloc_size = 8192;

    void *code_mem = xm_code_alloc(alloc, alloc_size, 16);
    if (!code_mem) {
        result.error = "failed to allocate executable memory";
        xr_free(ctx.block_offsets);
        xr_free(ctx.patches);
        xr_free(ctx.cs_patches);
        xra_result_free(ctx.xra);
        return result;
    }

#ifdef XR_OS_MACOS
    xm_code_make_writable(code_mem, alloc_size);
#endif

    a64_buf_init(&ctx.buf, (uint32_t *) code_mem, alloc_size / 4);

    // Emit prologue
    emit_prologue(&ctx);

    // Emit B to skip fast prologue (placeholder, patched after fast prologue)
    uint32_t skip_fast_idx = ctx.buf.count;
    a64_buf_emit(&ctx.buf, a64_nop());  // placeholder for B

    // Emit fast prologue (frame setup only, no param loading)
    ctx.fast_entry_offset = ctx.buf.count;
    emit_fast_prologue(&ctx);

    // Patch the skip branch: B over fast prologue to body start
    {
        int32_t off = (int32_t) ctx.buf.count - (int32_t) skip_fast_idx;
        ctx.buf.code[skip_fast_idx] = a64_b(off);
    }

    /* Reorder blocks: place loop body right after loop header.
     * This enables fallthrough from header to body, eliminating
     * one unconditional branch per loop iteration. */
    for (uint32_t i = 0; i + 1 < func->nblk; i++) {
        XmBlock *blk = func->blocks[i];
        if (blk->is_loop_header && blk->jmp.type == XM_JMP_BR && blk->s1) {
            XmBlock *body = blk->s1;
            if (func->blocks[i + 1] != body) {
                for (uint32_t j = i + 2; j < func->nblk; j++) {
                    if (func->blocks[j] == body) {
                        XmBlock *tmp = func->blocks[j];
                        for (uint32_t k = j; k > i + 1; k--)
                            func->blocks[k] = func->blocks[k - 1];
                        func->blocks[i + 1] = tmp;
                        break;
                    }
                }
            }
        }
    }

    // Emit all blocks
    for (uint32_t i = 0; i < func->nblk; i++) {
        emit_block(&ctx, i);
    }

    // Emit stubs (after all blocks, before patching)
    a64_emit_barrier_stubs(&ctx);
    a64_emit_deopt_stub(&ctx);
    a64_emit_call_c_stub(&ctx);
    a64_emit_osr_stubs(&ctx, &result);
    a64_emit_resume_entry(&ctx, &result);

    // Patch all branches
    a64_patch_branches(&ctx);

    // Build runtime deopt table from Xm deopt infos + regalloc state
    a64_build_runtime_deopt_table(&ctx, &result);

    // Patch frame size: JIT_FRAME_BASE + spill area, 16-byte aligned
    uint32_t frame_size = (JIT_FRAME_BASE + ctx.xra->nspill * 8 + 15) & ~(uint32_t) 15;
    for (uint32_t i = 0; i < ctx.nsub_patches; i++) {
        ctx.buf.code[ctx.frame_patch_sub[i]] = a64_sub_imm(A64_SP, A64_SP, frame_size);
    }
    for (uint32_t i = 0; i < ctx.nadd_patches; i++) {
        ctx.buf.code[ctx.frame_patch_add[i]] = a64_add_imm(A64_SP, A64_SP, frame_size);
    }

    // NOP-out callee-saved STP/LDP for unused register pairs
    // GP pairs 0-3: x21/x22..x27/x28 → csaved bits 1-8
    // FP pairs 4-7: d8/d9..d14/d15   → csaved bits 16-23
    for (uint32_t i = 0; i < ctx.ncs_patches; i++) {
        uint8_t pair = ctx.cs_patches[i].pair;
        bool used;
        if (pair < 4) {
            // GP pair: bits (pair*2+1) and (pair*2+2) in csaved
            uint32_t mask = (0x3u << (pair * 2 + 1));
            used = (ctx.xra->callee_saved & mask) != 0;
        } else {
            // FP pair: d(8+2*(pair-4)) and d(9+2*(pair-4)) → bits 16+2*(pair-4)..
            uint32_t fp_pair = pair - 4;
            uint32_t mask = (0x3u << (16 + fp_pair * 2));
            used = (ctx.xra->callee_saved & mask) != 0;
        }
        if (!used) {
            ctx.buf.code[ctx.cs_patches[i].idx] = a64_nop();
        }
    }

    // Post-emit peephole: NOP-out redundant STR+LDR, MOV self, CMP+B→CBZ
    xm_peephole(&ctx.buf);

    uint32_t code_size = a64_buf_offset(&ctx.buf);

    xm_code_make_executable(code_mem, code_size);
    xm_code_flush_icache(code_mem, code_size);

    if (ctx.had_error) {
        result.error = "regalloc: vreg not assigned at use point";
        xr_free(ctx.block_offsets);
        xr_free(ctx.patches);
        xr_free(ctx.cs_patches);
        xr_free(ctx.vreg_override);
        xra_result_free(ctx.xra);
        return result;
    }

    result.code = code_mem;
    result.code_size = code_size;
    // Convert instruction index to byte offset (ARM64: 4 bytes/insn)
    result.fast_entry_offset = ctx.fast_entry_offset * 4;
    result.stack_map = build_stack_map_table(&ctx, frame_size);
    result.success = true;

    xr_free(ctx.block_offsets);
    xr_free(ctx.patches);
    xr_free(ctx.cs_patches);
    xr_free(ctx.vreg_override);
    xra_result_free(ctx.xra);
    return result;
}

// xm_insert_write_barriers moved to xm_pass.c

#endif  // __aarch64__
