/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_codegen.c - XIR → ARM64 machine code generation
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

#include "xir_codegen_internal.h"
#include "xir_coalesce.h"
#include "xir_peephole.h"
#include "xir_blueprint.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

/* ========== Register Allocator (live-range aware) ========== */

// Types, constants, and macros now in xir_codegen_internal.h

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
 * Supported patterns (see is_remat in xir_regalloc.c):
 *   - XIR_CONST_I64 / XIR_CONST_F64 / XIR_CONST_PTR  →  load imm64
 *   - XIR_LOAD_CORO_BYTE with const offset  →  LDRB from jit_ctx
 *   - XIR_I2F with const int operand  →  load imm64 + SCVTF
 *   - XIR_F2I with const float operand  →  FMOV + FCVTZS
 */
static bool emit_remat(CodegenCtx *ctx, uint32_t vreg, A64Reg rd) {
    if (vreg >= ctx->func->nvreg)
        return false;
    XirIns *def = ctx->func->vregs[vreg].def;
    if (!def)
        return false;

    switch (def->op) {
        case XIR_CONST_I64:
        case XIR_CONST_F64:
        case XIR_CONST_PTR:
            if (xir_ref_is_const(def->args[0])) {
                uint32_t ci = XIR_REF_INDEX(def->args[0]);
                if (ci < ctx->func->nconst) {
                    a64_load_imm64(&ctx->buf, rd, (uint64_t) ctx->func->consts[ci].val.raw);
                    return true;
                }
            }
            return false;

        case XIR_LOAD_CORO_BYTE:
            if (xir_ref_is_const(def->args[0])) {
                uint32_t ci = XIR_REF_INDEX(def->args[0]);
                if (ci < ctx->func->nconst) {
                    int32_t offset = (int32_t) ctx->func->consts[ci].val.i64;
                    a64_buf_emit(&ctx->buf, a64_ldrb(rd, JIT_CTX_REG, offset));
                    return true;
                }
            }
            return false;

        case XIR_I2F: {
            /* Pre-compute (double)int64 and emit as FP constant load.
             * rd is an FP register; a64_load_f64 handles GP→FP transfer. */
            if (xir_ref_is_const(def->args[0])) {
                uint32_t ci = XIR_REF_INDEX(def->args[0]);
                if (ci < ctx->func->nconst) {
                    double d = (double) ctx->func->consts[ci].val.i64;
                    a64_load_f64(&ctx->buf, rd, SCRATCH_REG, d);
                    return true;
                }
            }
            return false;
        }

        case XIR_F2I: {
            /* Pre-compute (int64_t)f64 at compile time and emit as
             * integer constant load — avoids needing an FP scratch reg. */
            if (xir_ref_is_const(def->args[0])) {
                uint32_t ci = XIR_REF_INDEX(def->args[0]);
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
A64Reg xra_get(CodegenCtx *ctx, XirRef ref) {
    if (xir_ref_is_none(ref))
        return A64_XZR;
    if (!xir_ref_is_vreg(ref))
        return A64_XZR;
    uint32_t idx = XIR_REF_INDEX(ref);

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
    if (ctx->nsmap >= XIR_MAX_STACK_MAP_ENTRIES) {
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
                if (slot >= 0 && slot < XIR_MAX_SPILL_SLOTS)
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
            if (slot >= 0 && slot < XIR_MAX_SPILL_SLOTS) {
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
    a64_buf_emit(&ctx->buf, a64_ldr_w(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_FRAME_DEPTH_OFFSET));
    // X17 = X28 + depth * 8  (byte offset into frame_stack)
    a64_buf_emit(&ctx->buf, a64_add_lsl(SCRATCH_REG2, JIT_CTX_REG, SCRATCH_REG, 3));
    // STR FP, [X17, #stack_offset]
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, SCRATCH_REG2, XIR_JIT_FRAME_STACK_OFFSET));
    // depth++
    a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG, SCRATCH_REG, 1));
    // STR W16, [X28, #depth_offset]
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_FRAME_DEPTH_OFFSET));
}

// Pop from jit_frame_stack after a cross-function call returns.
void emit_jit_frame_pop(CodegenCtx *ctx) {
    XR_DCHECK(ctx != NULL, "emit_jit_frame_pop: NULL ctx");
    // LDR W16, [X28, #depth_offset]
    a64_buf_emit(&ctx->buf, a64_ldr_w(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_FRAME_DEPTH_OFFSET));
    // depth--
    a64_buf_emit(&ctx->buf, a64_sub_imm(SCRATCH_REG, SCRATCH_REG, 1));
    // STR W16, [X28, #depth_offset]
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_FRAME_DEPTH_OFFSET));
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
static void emit_edge_copies(CodegenCtx *ctx, XirBlock *target, XirBlock *from) {
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
                copies[n++] = (Copy){d, A64_XZR, false, ec[i].is_fp, true, ec[i].spill_slot};
                continue;
            }
            A64Reg s = ec[i].is_fp ? alloc_fp_regs[ec[i].src_idx] : alloc_regs[ec[i].src_idx];
            if (d != s)
                copies[n++] = (Copy){d, s, false, ec[i].is_fp, false, 0};
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
 * comparison instruction (XIR_LT..XIR_FLE) with no other uses, we can
 * fuse the CMP and branch: skip the CSET in the comparison, and emit
 * B.cc (conditional branch) instead of CBZ in the terminator.
 *
 * Saves 1 instruction per fused comparison+branch pair:
 *   Before: CMP a,b; CSET dst,cc; CBZ dst,false  (3 instructions)
 *   After:  CMP a,b; B.inverted_cc false           (2 instructions)
 */

/* Map XIR comparison op to inverted A64Cond for the false branch.
 * Returns A64_CC_AL if the op is not a fusible comparison. */
static A64Cond xir_cmp_to_false_cc(uint16_t op, bool *is_float) {
    *is_float = false;
    switch (op) {
        case XIR_LT:
            return A64_CC_GE;
        case XIR_LE:
            return A64_CC_GT;
        case XIR_GT:
            return A64_CC_LE;
        case XIR_GE:
            return A64_CC_LT;
        case XIR_EQ:
            return A64_CC_NE;
        case XIR_NE:
            return A64_CC_EQ;
        case XIR_FEQ:
            *is_float = true;
            return A64_CC_NE;
        case XIR_FNE:
            *is_float = true;
            return A64_CC_EQ;
        case XIR_FLT:
            *is_float = true;
            return A64_CC_PL;
        case XIR_FLE:
            *is_float = true;
            return A64_CC_HI;
        default:
            return A64_CC_AL;
    }
}

// Check if an XIR op may clobber ARM64 condition flags when emitted.
static bool xir_op_clobbers_flags(uint16_t op) {
    // Comparisons emit CMP/FCMP + CSET
    if (op >= XIR_EQ && op <= XIR_FLE)
        return true;
    // SELECT_COND emits CMP_IMM
    if (op == XIR_SELECT_COND)
        return true;
    // Guards emit CMP internally
    if (op == XIR_GUARD_TAG || op == XIR_GUARD_CLASS || op == XIR_GUARD_KLASS ||
        op == XIR_GUARD_NONNULL || op == XIR_GUARD_SHAPE || op == XIR_GUARD_BOUNDS)
        return true;
    // Safepoint emits CMP for reductions check
    if (op == XIR_SAFEPOINT)
        return true;
    // Calls: callee may clobber NZCV
    if (op == XIR_CALL || op == XIR_CALL_C || op == XIR_CALL_C_LEAF || op == XIR_CALL_KNOWN ||
        op == XIR_CALL_KNOWN_REG || op == XIR_CALL_SELF_DIRECT || op == XIR_CALL_DIRECT)
        return true;
    // ALLOC: inline bump-pointer has CMP for limit check
    if (op == XIR_ALLOC)
        return true;
    // RT_* helpers call into C runtime
    if (op >= XIR_RT_ADD && op <= XIR_RT_ISNULL)
        return true;
    // BOX/UNBOX may involve runtime calls
    if (op == XIR_BOX_I64 || op == XIR_BOX_F64 || op == XIR_UNBOX_I64 || op == XIR_UNBOX_F64)
        return true;
    return false;
}

/*
 * Pre-scan a block for CMP+BR fusion opportunity.
 * Sets ctx->fused_cmp_ref if fusion is possible.
 */
static void prescan_fuse_cmp_br(CodegenCtx *ctx, XirBlock *blk) {
    XR_DCHECK(ctx != NULL, "prescan_fuse_cmp_br: NULL ctx");
    XR_DCHECK(blk != NULL, "prescan_fuse_cmp_br: NULL blk");
    ctx->fused_cmp_ref = XIR_NONE;

    if (blk->jmp.type != XIR_JMP_BR)
        return;
    XirRef cond = blk->jmp.arg;
    if (!xir_ref_is_vreg(cond))
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
    A64Cond fcc = xir_cmp_to_false_cc(blk->ins[def_idx].op, &is_float);
    if (fcc == A64_CC_AL)
        return;

    // Check no flag-clobbering instruction after the comparison
    for (uint32_t i = (uint32_t) def_idx + 1; i < blk->nins; i++) {
        if (xir_op_clobbers_flags(blk->ins[i].op))
            return;
    }

    // Check cond vreg has no other uses in this block's instructions
    for (uint32_t i = 0; i < blk->nins; i++) {
        if ((int) i == def_idx)
            continue;
        XirIns *ins = &blk->ins[i];
        if ((xir_ref_is_vreg(ins->args[0]) && ins->args[0] == cond) ||
            (xir_ref_is_vreg(ins->args[1]) && ins->args[1] == cond))
            return;
    }

    // Check no use in successor phis
    XirBlock *succs[2] = {blk->s1, blk->s2};
    for (int s = 0; s < 2; s++) {
        if (!succs[s])
            continue;
        for (XirPhi *phi = succs[s]->phis; phi; phi = phi->next) {
            for (uint32_t p = 0; p < phi->narg; p++) {
                if (xir_ref_is_vreg(phi->args[p]) && phi->args[p] == cond)
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

void emit_load_const(CodegenCtx *ctx, A64Reg rd, XirRef ref) {
    XR_DCHECK(ctx != NULL, "emit_load_const: NULL ctx");
    if (xir_ref_is_const(ref)) {
        uint32_t idx = XIR_REF_INDEX(ref);
        XR_DCHECK(idx < ctx->func->nconst, "assertion failed");
        XirConst *c = &ctx->func->consts[idx];
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
 * Resolve an XIR arg ref to a physical register, handling spilled vregs.
 * If the vreg was spilled (vreg_spill >= 0), emits LDR from spill slot
 * into scratch_reg and returns scratch_reg. Otherwise returns vreg_reg[].
 * For rematerializable vregs (constants), re-emits the constant load.
 */
A64Reg xra_arg(CodegenCtx *ctx, XirRef ref, A64Reg scratch_reg) {
    XR_DCHECK(ctx != NULL, "xra_arg: NULL ctx");
    if (xir_ref_is_none(ref))
        return A64_XZR;
    if (!xir_ref_is_vreg(ref))
        return A64_XZR;
    uint32_t idx = XIR_REF_INDEX(ref);
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
void xra_maybe_spill(CodegenCtx *ctx, XirRef dst_ref) {
    XR_DCHECK(ctx != NULL, "xra_maybe_spill: NULL ctx");
    if (!xir_ref_is_vreg(dst_ref))
        return;
    uint32_t v = XIR_REF_INDEX(dst_ref);
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

// Resolve an XIR operand to a physical register. If the operand is a const
// pool ref (not a vreg), materialize it into scratch_reg and return that.
// For vreg refs, handles spill reload via xra_arg().
A64Reg xra_operand(CodegenCtx *ctx, XirRef ref, A64Reg scratch_reg) {
    if (xir_ref_is_const(ref)) {
        uint32_t idx = XIR_REF_INDEX(ref);
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
static bool const_i64_fully_folded(CodegenCtx *ctx, XirBlock *blk, XirRef dst) {
    if (!xir_ref_is_vreg(dst))
        return false;
    uint32_t vi = XIR_REF_INDEX(dst);
    if (vi >= ctx->func->nvreg)
        return false;
    XirIns *def = ctx->func->vregs[vi].def;
    if (!def || def->op != XIR_CONST_I64)
        return false;
    if (!xir_ref_is_const(def->args[0]))
        return false;
    uint32_t ci = XIR_REF_INDEX(def->args[0]);
    if (ci >= ctx->func->nconst)
        return false;
    int64_t val = ctx->func->consts[ci].val.i64;
    if (val < 0 || val > 4095)
        return false;

    /* Scan block: every use of vi must be in an ADD/SUB arg position
     * where the other arg is NOT also this const (to avoid double-fold). */
    uint32_t total = 0, folded = 0;
    for (uint32_t i = 0; i < blk->nins; i++) {
        XirIns *ins = &blk->ins[i];
        for (int a = 0; a < 2; a++) {
            if (!xir_ref_is_vreg(ins->args[a]))
                continue;
            if (XIR_REF_INDEX(ins->args[a]) != vi)
                continue;
            total++;
            if (ins->op == XIR_ADD || ins->op == XIR_SUB)
                folded++;
        }
    }
    // Also check terminator arg
    if (xir_ref_is_vreg(blk->jmp.arg) && XIR_REF_INDEX(blk->jmp.arg) == vi)
        total++;
    return total > 0 && total == folded;
}

/* Try to extract a small immediate (0..4095) from an XIR operand.
 * Returns true if the operand is a vreg defined by XIR_CONST_I64
 * with a value that fits in ARM64 add_imm/sub_imm (12-bit unsigned). */
static bool try_get_imm12(CodegenCtx *ctx, XirRef ref, uint32_t *out_imm) {
    if (!xir_ref_is_vreg(ref))
        return false;
    uint32_t vi = XIR_REF_INDEX(ref);
    if (vi >= ctx->func->nvreg)
        return false;
    XirIns *def = ctx->func->vregs[vi].def;
    if (!def || def->op != XIR_CONST_I64)
        return false;
    if (!xir_ref_is_const(def->args[0]))
        return false;
    uint32_t ci = XIR_REF_INDEX(def->args[0]);
    if (ci >= ctx->func->nconst)
        return false;
    int64_t val = ctx->func->consts[ci].val.i64;
    if (val >= 0 && val <= 4095) {
        *out_imm = (uint32_t) val;
        return true;
    }
    return false;
}

/* ========== Instruction Emission ========== */

static void emit_xir_ins(CodegenCtx *ctx, XirIns *ins) {
    XR_DCHECK(ctx != NULL, "emit_xir_ins: NULL ctx");
    XR_DCHECK(ins != NULL, "emit_xir_ins: NULL ins");
    A64Reg rd = xra_get(ctx, ins->dst);

    switch (ins->op) {
        case XIR_ADD: {
            uint32_t imm;
            if (try_get_imm12(ctx, ins->args[1], &imm)) {
                A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_add_imm(rd, rn, imm));
            } else if (try_get_imm12(ctx, ins->args[0], &imm)) {
                A64Reg rn = xra_operand(ctx, ins->args[1], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_add_imm(rd, rn, imm));
            } else {
                A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
                A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
                a64_buf_emit(&ctx->buf, a64_add(rd, rn, rm));
            }
            break;
        }
        case XIR_SUB: {
            uint32_t imm;
            if (try_get_imm12(ctx, ins->args[1], &imm)) {
                A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_sub_imm(rd, rn, imm));
            } else {
                A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
                A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
                a64_buf_emit(&ctx->buf, a64_sub(rd, rn, rm));
            }
            break;
        }
        case XIR_MUL: {
            A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_mul(rd, rn, rm));
            break;
        }
        case XIR_DIV: {
            A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
            // Division by zero: ARM64 SDIV returns 0 silently.
            // Deopt to interpreter which throws the proper exception.
            add_patch(ctx, PATCH_DEOPT_CBZ, 0, rm);
            a64_buf_emit(&ctx->buf, a64_nop());  // patched to CBZ rm, deopt
            ctx->has_deopt = true;
            a64_buf_emit(&ctx->buf, a64_sdiv(rd, rn, rm));
            break;
        }
        case XIR_MOD: {
            // ARM64 has no MOD: dst = a - (a / b) * b
            A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
            // Division by zero guard (same as XIR_DIV)
            add_patch(ctx, PATCH_DEOPT_CBZ, 0, rm);
            a64_buf_emit(&ctx->buf, a64_nop());  // patched to CBZ rm, deopt
            ctx->has_deopt = true;
            a64_buf_emit(&ctx->buf, a64_sdiv(SCRATCH_REG, rn, rm));
            a64_buf_emit(&ctx->buf, a64_msub(rd, SCRATCH_REG, rm, rn));
            break;
        }
        case XIR_NEG: {
            A64Reg rm = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_neg(rd, rm));
            break;
        }
        case XIR_AND: {
            A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_and(rd, rn, rm));
            break;
        }
        case XIR_OR: {
            A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_orr(rd, rn, rm));
            break;
        }
        case XIR_XOR: {
            A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_eor(rd, rn, rm));
            break;
        }
        case XIR_NOT: {
            A64Reg rm = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_mvn(rd, rm));
            break;
        }
        case XIR_SHL: {
            A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_lsl(rd, rn, rm));
            break;
        }
        case XIR_SHR: {
            A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_asr(rd, rn, rm));
            break;
        }

        // Float arithmetic
        case XIR_FADD: {
            A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_fadd(rd, dn, dm));
            break;
        }
        case XIR_FSUB: {
            A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_fsub(rd, dn, dm));
            break;
        }
        case XIR_FMUL: {
            A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_fmul(rd, dn, dm));
            break;
        }
        case XIR_FDIV: {
            A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_fdiv(rd, dn, dm));
            break;
        }
        case XIR_FNEG: {
            A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_fneg(rd, dn));
            break;
        }

        // Type conversion: int64 → float64
        case XIR_I2F: {
            A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_scvtf(rd, rn));
            break;
        }

        // Type conversion: float64 → int64 (truncate toward zero)
        case XIR_F2I: {
            A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_fcvtzs(rd, dn));
            break;
        }

        // Float comparison: FCMP + CSET (result is i64 0/1 in GP reg)
        case XIR_FEQ:
        case XIR_FNE:
        case XIR_FLT:
        case XIR_FLE: {
            A64Reg dn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg dm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_fcmp(dn, dm));
            // CMP+BR fusion: skip CSET when fused with BR terminator
            if (!xir_ref_is_none(ctx->fused_cmp_ref) && xir_ref_is_vreg(ins->dst) &&
                ins->dst == ctx->fused_cmp_ref)
                break;
            A64Cond cc;
            switch (ins->op) {
                case XIR_FEQ:
                    cc = A64_CC_EQ;
                    break;
                case XIR_FNE:
                    cc = A64_CC_NE;
                    break;
                case XIR_FLT:
                    cc = A64_CC_MI;
                    break;  // LT for unordered-aware
                case XIR_FLE:
                    cc = A64_CC_LS;
                    break;  // LE for unordered-aware
                default:
                    cc = A64_CC_AL;
                    break;
            }
            a64_buf_emit(&ctx->buf, a64_cset(rd, cc));
            break;
        }

        // Comparison: CMP + CSET
        case XIR_LT:
        case XIR_LE:
        case XIR_GT:
        case XIR_GE:
        case XIR_EQ:
        case XIR_NE: {
            A64Reg rn = xra_operand(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_operand(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_cmp(rn, rm));
            // CMP+BR fusion: skip CSET when fused with BR terminator
            if (!xir_ref_is_none(ctx->fused_cmp_ref) && xir_ref_is_vreg(ins->dst) &&
                ins->dst == ctx->fused_cmp_ref)
                break;
            A64Cond cc;
            switch (ins->op) {
                case XIR_LT:
                    cc = A64_CC_LT;
                    break;
                case XIR_LE:
                    cc = A64_CC_LE;
                    break;
                case XIR_GT:
                    cc = A64_CC_GT;
                    break;
                case XIR_GE:
                    cc = A64_CC_GE;
                    break;
                case XIR_EQ:
                    cc = A64_CC_EQ;
                    break;
                case XIR_NE:
                    cc = A64_CC_NE;
                    break;
                default:
                    cc = A64_CC_AL;
                    break;
            }
            a64_buf_emit(&ctx->buf, a64_cset(rd, cc));
            break;
        }

        // Constant load (GP)
        case XIR_CONST_I64:
        case XIR_CONST_PTR: {
            emit_load_const(ctx, rd, ins->args[0]);
            break;
        }

        // Constant load (FP): load bits into scratch GPR, FMOV to Dd
        case XIR_CONST_F64: {
            if (xir_ref_is_const(ins->args[0])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[0]);
                XR_DCHECK(ci < ctx->func->nconst, "assertion failed");
                double val;
                uint64_t raw = (uint64_t) ctx->func->consts[ci].val.raw;
                memcpy(&val, &raw, 8);
                a64_load_f64(&ctx->buf, rd, SCRATCH_REG, val);
            } else {
                // vreg → FP move
                A64Reg src = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                if (src != rd) {
                    a64_buf_emit(&ctx->buf, a64_fmov(rd, src));
                }
            }
            break;
        }

        // Conditional select (from if-conversion)
        case XIR_SELECT_COND: {
            // CMP cond, #0 — set flags for subsequent CSEL
            A64Reg cond_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_cmp_imm(cond_reg, 0));
            break;
        }
        case XIR_SELECT: {
            // CSEL/FCSEL rd, true_val, false_val, NE
            if (ins->rep == XR_REP_F64) {
                A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                A64Reg rm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                a64_buf_emit(&ctx->buf, a64_fcsel(rd, rn, rm, A64_CC_NE));
            } else {
                A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                A64Reg rm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                a64_buf_emit(&ctx->buf, a64_csel(rd, rn, rm, A64_CC_NE));
            }
            break;
        }

        // Register move (GP or FP) — also handles REDEFINE (type narrowing copy)
        case XIR_MOV:
        case XIR_REDEFINE: {
            A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            if (rd != rn) {
                if (ins->rep == XR_REP_F64) {
                    a64_buf_emit(&ctx->buf, a64_fmov(rd, rn));
                } else {
                    a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
                }
            }
            break;
        }

        // BOX/UNBOX: JIT internal values are always raw (untagged).
        // BOX is a no-op inside JIT — tag is written only at memory boundaries
        // (STORE_FIELD, STORE_UPVAL). UNBOX extracts payload from tagged memory.
        case XIR_BOX_I64: {
            A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            if (rd != rn) {
                a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
            }
            break;
        }
        case XIR_BOX_F64: {
            A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            if (rd != rn) {
                a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
            }
            break;
        }

        // UNBOX_I64: extract raw i64 payload from a tagged XrValue pointer
        // If args[0] is already a raw i64 vreg (common case), just MOV.
        // If args[0] is PTR (pointing to XrValue in memory), load payload.
        case XIR_UNBOX_I64: {
            uint8_t src_type = XR_REP_I64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                if (vi < ctx->func->nvreg)
                    src_type = ctx->func->vregs[vi].rep;
            }
            if (src_type == XR_REP_PTR) {
                // Source is a pointer to XrValue — load payload from [ptr+8]
                A64Reg ptr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_ldr(rd, ptr, XIR_XRVALUE_PAYLOAD_OFFSET));
            } else {
                A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                if (rd != rn)
                    a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
            }
            break;
        }

        // UNBOX_F64: extract raw f64 payload from a tagged XrValue pointer
        case XIR_UNBOX_F64: {
            uint8_t src_type = XR_REP_F64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
                if (vi < ctx->func->nvreg)
                    src_type = ctx->func->vregs[vi].rep;
            }
            if (src_type == XR_REP_PTR) {
                // Source is a pointer to XrValue — load f64 payload from [ptr+8]
                A64Reg ptr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_ldr_fp(rd, ptr, XIR_XRVALUE_PAYLOAD_OFFSET));
            } else {
                A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                if (rd != rn)
                    a64_buf_emit(&ctx->buf, a64_mov(rd, rn));
            }
            break;
        }

        default:
            // Delegate to sub-emit functions (split for maintainability)
            if (xir_emit_call_ops(ctx, ins, rd))
                break;
            if (xir_emit_mem_ops(ctx, ins, rd))
                break;
            xr_log_warning("jit", "unhandled XIR opcode %d in emit_xir_ins", ins->op);
            a64_buf_emit(&ctx->buf, a64_nop());
            break;
    }
}

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
    a64_buf_emit(&ctx->buf, a64_ldr(JIT_CTX_REG, CORO_REG, XIR_CORO_JIT_CTX_OFFSET));

    // Store stack_map_ptr into frame from jit_ctx->active_stack_map (set by xir_jit_call)
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_ACTIVE_SMAP_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, A64_FP, FRAME_SMAP_PTR_OFFSET));
    // Initialize safepoint_id = UINT32_MAX (no active safepoint)
    // Write to both frame (for future FP chain walking) and jit_ctx (for current GC)
    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, 0xFFFF, 0));
    a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG, 0xFFFF, 16));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, A64_FP, FRAME_SMAP_ID_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_ACTIVE_SMAP_ID_OFFSET));
    // Save JIT frame SP for GC access: jit_ctx->jit_frame_sp = FP
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XIR_JIT_FRAME_SP_OFFSET));
    // Load guard page pointer into x20 from jit_ctx->safepoint_page.
    // JIT back-edges emit LDR WZR,[x20] which faults when page is armed.
    a64_buf_emit(&ctx->buf,
                 a64_ldr(SAFEPT_PAGE_REG, JIT_CTX_REG, (int32_t) XIR_JIT_SAFEPOINT_PAGE_OFFSET));

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
        // Init slot_runtime_tags for TAGGED (any/Json) params from param_tags[].
        // emit_call_args_from_pool dynamic-patches UNKNOWN tags by reading
        // slot_runtime_tags[bc_slot]. Without this init, dynamic ops on
        // any-typed params (OP_ADD, etc.) see stale/zero tags → wrong results.
        // param_tags[] is set by xir_jit_call (interp→JIT) or
        // xr_jit_call_func / CALL_KNOWN codegen (JIT→JIT).
        for (uint32_t i = 0; i < nparams && i < 8; i++) {
            if (i >= ctx->func->nvreg)
                continue;
            if (ctx->func->vregs[i].rep != XR_REP_TAGGED)
                continue;
            int16_t bc_slot = ctx->func->vregs[i].bc_slot;
            if (bc_slot < 0 || bc_slot >= 256)
                continue;
            int32_t pt_off = (int32_t) (XIR_JIT_PARAM_TAGS_OFFSET + i * 8);
            int32_t rt_off = (int32_t) XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot;
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
    a64_buf_emit(&ctx->buf, a64_ldr(JIT_CTX_REG, CORO_REG, XIR_CORO_JIT_CTX_OFFSET));

    // Store stack_map_ptr into frame from jit_ctx->active_stack_map
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_ACTIVE_SMAP_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, A64_FP, FRAME_SMAP_PTR_OFFSET));
    // Initialize safepoint_id = UINT32_MAX (both frame + jit_ctx)
    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, 0xFFFF, 0));
    a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG, 0xFFFF, 16));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, A64_FP, FRAME_SMAP_ID_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_ACTIVE_SMAP_ID_OFFSET));
    // Save JIT frame SP for GC access
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XIR_JIT_FRAME_SP_OFFSET));
    // Load guard page pointer into x20 from jit_ctx->safepoint_page
    a64_buf_emit(&ctx->buf,
                 a64_ldr(SAFEPT_PAGE_REG, JIT_CTX_REG, (int32_t) XIR_JIT_SAFEPOINT_PAGE_OFFSET));

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
    XirBlock *blk = ctx->func->blocks[block_idx];

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
    if (blk->is_loop_header && ctx->nosr_snap < XIR_MAX_OSR_ENTRIES && !ctx->func->has_coro_deopt) {
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
        if (blk->ins[i].op == XIR_CONST_I64 && const_i64_fully_folded(ctx, blk, blk->ins[i].dst)) {
            continue;
        }
        emit_xir_ins(ctx, &blk->ins[i]);

        // Spill store: if dst vreg has a spill slot, write to stack
        xra_maybe_spill(ctx, blk->ins[i].dst);

        // Exception check after call-sites
        {
            uint16_t op = blk->ins[i].op;
            if (op == XIR_CALL_C || op == XIR_CALL_DIRECT || op == XIR_CALL_SELF_DIRECT ||
                op == XIR_CALL_KNOWN_REG) {
                if (blk->exception_handler) {
                    // In try block: branch to catch handler
                    a64_buf_emit(&ctx->buf,
                                 a64_ldr(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_EXCEPTION_OFFSET));
                    add_patch(ctx, PATCH_CBNZ, blk->exception_handler->id, SCRATCH_REG);
                    a64_buf_emit(&ctx->buf, a64_nop());
                } else if (blk->ins[i].flags & XIR_FLAG_MAY_THROW) {
                    // No catch handler but may throw: deopt to interpreter
                    // so exception propagates through VM try-catch frames
                    a64_buf_emit(&ctx->buf,
                                 a64_ldr(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_EXCEPTION_OFFSET));
                    add_patch(ctx, PATCH_DEOPT_CBNZ, 0, SCRATCH_REG);
                    a64_buf_emit(&ctx->buf, a64_nop());
                    ctx->has_deopt = true;
                }
            }
        }
    }

    // Emit terminator
    switch (blk->jmp.type) {
        case XIR_JMP_JMP: {
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
        case XIR_JMP_BR: {
            XR_DCHECK(blk->s1 != NULL && blk->s2 != NULL, "assertion failed");

            if (!xir_ref_is_none(ctx->fused_cmp_ref)) {
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
        case XIR_JMP_RET: {
            if (!xir_ref_is_none(blk->jmp.arg)) {
                A64Reg ret_reg = xra_get(ctx, blk->jmp.arg);
                uint32_t ret_idx = XIR_REF_INDEX(blk->jmp.arg);
                bool is_fp =
                    (ret_idx < ctx->func->nvreg && ctx->func->vregs[ret_idx].rep == XR_REP_F64);

                // Return XrJitResult: x0=payload, x1=tag (ARM64 two-register struct).
                // call_c_stub captures both before restoring live regs, so callers
                // receive {payload, tag} without any memory side-channel.
                // Determine return value_tag (x1) from ctype
                uint8_t ret_vtag = VTAG_TAGGED;
                uint8_t ret_xr_tag = XR_TAG_I64;  // default
                if (ret_idx < ctx->func->nvreg) {
                    XirType rct = xir_ref_ctype(ctx->func, blk->jmp.arg);
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
        case XIR_JMP_UNREACHABLE:
            a64_buf_emit(&ctx->buf, 0xD4200020);  // BRK #1
            break;
        default:
            break;
    }
}

/* Safepoint stub removed: guard page safepoint uses global trampoline
 * via SIGSEGV signal handler instead of per-function BL stub. */

/* ========== Write Barrier Stubs ========== */

static void emit_barrier_stubs(CodegenCtx *ctx) {
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

static void emit_deopt_stub(CodegenCtx *ctx) {
    if (!ctx->has_deopt)
        return;

    ctx->deopt_stub = ctx->buf.count;

    // Write deopt_id (in w17/SCRATCH_REG2) to jit_ctx->deopt_id
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XIR_JIT_DEOPT_ID_OFFSET));

    // Save frame pointer for spill slot recovery
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XIR_JIT_DEOPT_SPILL_BASE_OFFSET));

    // Save all allocatable GP registers to jit_ctx->deopt_regs[reg_num]
    // MUST happen BEFORE epilogue restores callee-saved registers.
    // x19 = CORO_REG (preserved), x28 = JIT_CTX_REG (preserved)
    int32_t gp_base = XIR_JIT_DEOPT_REGS_OFFSET;
    for (int i = 0; i < MAX_PHYS_REGS; i++) {
        A64Reg r = alloc_regs[i];
        a64_buf_emit(&ctx->buf, a64_str(r, JIT_CTX_REG, gp_base + (int32_t) r * 8));
    }

    // Save FP registers d0-d15 to jit_ctx->deopt_fp_regs[d_num]
    int32_t fp_base = XIR_JIT_DEOPT_FP_REGS_OFFSET;
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
        int32_t save_base = (int32_t) XIR_JIT_DEOPT_SPILL_SAVE_OFFSET;
        for (uint32_t s = 0; s < nspill; s++) {
            int32_t frame_off = SPILL_BASE + (int32_t) s * 8;
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, A64_SP, frame_off));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, JIT_CTX_REG, save_base + (int32_t) s * 8));
        }
    }

    // Load deopt marker into x0
    a64_load_imm64(&ctx->buf, A64_X0, (uint64_t) XIR_DEOPT_MARKER);

    // Epilogue + return
    emit_epilogue(ctx);
    a64_buf_emit(&ctx->buf, a64_ret());
}

/* ========== CALL_C Stub ========== */

// Generic C call stub: caller sets x16=func_ptr, x17=extra_arg
// Stub: save x1-x15+LR, call func(coro, extra_arg), restore, return result in x0
static void emit_call_c_stub(CodegenCtx *ctx) {
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
    a64_buf_emit(&ctx->buf, a64_str(A64_SP, JIT_CTX_REG, XIR_JIT_SAFEPOINT_SAVED_SP_OFFSET));

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
    a64_buf_emit(&ctx->buf, a64_str(A64_X1, JIT_CTX_REG, XIR_JIT_CALL_RESULT_TAG_OFFSET));

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
    // Tag is in jit_ctx->call_result_tag for XIR_CALL_C codegen to read.
    a64_buf_emit(&ctx->buf, a64_mov(A64_X0, SCRATCH_REG));
    a64_buf_emit(&ctx->buf, a64_ret());
}

/* ========== OSR Helpers ========== */

// Check if a vreg should be skipped during OSR entry loading.
// Skip vregs defined inside the loop header block or coalesced PHI inputs.
static bool osr_should_skip_vreg(CodegenCtx *ctx, XirBlock *osr_blk, uint32_t v, int8_t ri) {
    if (!osr_blk)
        return false;
    XirRef vref = XIR_REF(XIR_REF_VREG, v);
    // Skip vregs defined by instructions in this block
    for (uint32_t ii = 0; ii < osr_blk->nins; ii++) {
        if (osr_blk->ins[ii].dst == vref)
            return true;
    }
    // Skip PHI inputs only when coalesced with the PHI dst
    // (same physical register). Unconditionally skipping all PHI
    // inputs is too aggressive: a vreg may be a PHI input for one
    // slot but also a live through-value for another slot.
    for (XirPhi *phi = osr_blk->phis; phi; phi = phi->next) {
        for (uint16_t ai = 0; ai < phi->narg; ai++) {
            if (phi->args[ai] == vref) {
                uint32_t pdv = XIR_REF_INDEX(phi->dst);
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
static void osr_materialize_const(CodegenCtx *ctx, XirIns *def, int8_t phys, int8_t fp) {
    if (!def)
        return;
    if (def->op == XIR_CONST_I64 && xir_ref_is_const(def->args[0])) {
        uint32_t ci = XIR_REF_INDEX(def->args[0]);
        uint64_t val = ctx->func->consts[ci].val.raw;
        if (phys >= 0)
            a64_load_imm64(&ctx->buf, alloc_regs[phys], val);
        else if (fp >= 0) {
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, val);
            a64_buf_emit(&ctx->buf, a64_fmov_from_gpr(alloc_fp_regs[fp], SCRATCH_REG2));
        }
    } else if (def->op == XIR_CONST_F64 && xir_ref_is_const(def->args[0])) {
        uint32_t ci = XIR_REF_INDEX(def->args[0]);
        uint64_t val = ctx->func->consts[ci].val.raw;
        if (fp >= 0) {
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, val);
            a64_buf_emit(&ctx->buf, a64_fmov_from_gpr(alloc_fp_regs[fp], SCRATCH_REG2));
        } else if (phys >= 0)
            a64_load_imm64(&ctx->buf, alloc_regs[phys], val);
    } else if (def->op == XIR_CONST_PTR && xir_ref_is_const(def->args[0])) {
        uint32_t ci = XIR_REF_INDEX(def->args[0]);
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
// Requires Blueprint loop info; loops without Blueprint are skipped.
static void emit_osr_stubs(CodegenCtx *ctx, XirCodegenResult *result) {
    for (uint32_t i = 0; i < ctx->nosr_snap; i++) {
        OsrSnapshot *snap = &ctx->osr_snaps[i];
        XirOsrEntry *entry = &result->osr_entries[result->nosr];

        entry->block_id = snap->block_id;
        // Copy bytecode PC from the XIR block for VM-side OSR matching
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
        a64_buf_emit(&ctx->buf, a64_ldr(JIT_CTX_REG, CORO_REG, XIR_CORO_JIT_CTX_OFFSET));

        // Store stack_map_ptr into frame from jit_ctx->active_stack_map
        a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, JIT_CTX_REG, XIR_JIT_ACTIVE_SMAP_OFFSET));
        a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, A64_FP, FRAME_SMAP_PTR_OFFSET));
        // Initialize safepoint_id = UINT32_MAX (both frame + jit_ctx)
        a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, 0xFFFF, 0));
        a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG2, 0xFFFF, 16));
        a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
        a64_buf_emit(&ctx->buf,
                     a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XIR_JIT_ACTIVE_SMAP_ID_OFFSET));
        // Save JIT frame SP for GC access
        a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XIR_JIT_FRAME_SP_OFFSET));
        // Load guard page pointer into x20 from jit_ctx->safepoint_page for OSR entry
        a64_buf_emit(&ctx->buf, a64_ldr(SAFEPT_PAGE_REG, JIT_CTX_REG,
                                        (int32_t) XIR_JIT_SAFEPOINT_PAGE_OFFSET));

        // Save values pointer to SCRATCH_REG (x16) before loading,
        // because x1 (alloc_regs[0]) may be overwritten by vreg loads
        a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, A64_X1));

        // Load live values from interpreter's register array into physical regs.
        // Uses Blueprint loop info for precise live slot information.
        // Loops without Blueprint are skipped (no legacy bc_slot fallback).
        XirBlock *osr_blk =
            (snap->block_id < ctx->func->nblk) ? ctx->func->blocks[snap->block_id] : NULL;
        uint16_t nslots = 0;

        // Look up Blueprint loop info for this header.
        // proto can be NULL when codegen is invoked outside a normal JIT
        // pipeline (e.g. unit tests that build XirFunc directly).
        XrBlueprint *bp = ctx->func->proto ? (XrBlueprint *) ctx->func->proto->blueprint : NULL;
        XrBpLoopInfo *bp_loop = NULL;
        if (bp && bp->loops) {
            uint32_t bc_off = entry->bc_offset;
            for (uint16_t li = 0; li < bp->nloops; li++) {
                if (bp->loops[li].header_pc == bc_off) {
                    bp_loop = &bp->loops[li];
                    break;
                }
            }
        }

        if (!bp_loop) {
            // No Blueprint for this loop — skip OSR entry.
            // All JIT-compiled functions have Blueprint, so this only
            // happens for edge cases (e.g. very large functions).
            continue;
        }

        // Load compiler-declared live slots from the values array.
        for (uint8_t li = 0; li < bp_loop->nlive && nslots < XIR_MAX_OSR_SLOTS; li++) {
            uint8_t slot = bp_loop->live[li].slot;

            // Find vreg(s) with this bc_slot that have a physical register
            for (uint32_t v = 0; v < ctx->func->nvreg; v++) {
                if (ctx->func->vregs[v].bc_slot != (int16_t) slot)
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
                    entry->slots[nslots].bc_slot = (int16_t) slot;
                    entry->slots[nslots].phys_reg = (uint8_t) dst;
                    entry->slots[nslots].type = XR_REP_I64;
                    nslots++;
                } else {
                    A64Reg dst = alloc_fp_regs[ri];
                    a64_buf_emit(&ctx->buf, a64_ldr_fp(dst, SCRATCH_REG, (int32_t) (slot * 8)));
                    entry->slots[nslots].bc_slot = (int16_t) slot;
                    entry->slots[nslots].phys_reg = (uint8_t) dst;
                    entry->slots[nslots].type = XR_REP_F64;
                    nslots++;
                }
                // Do NOT break: multiple vregs may share the same bc_slot
                // but have different physical registers (e.g. v0 and its phi
                // successor v11). All need to be loaded for correctness.
            }
        }

        // Materialize constants (vregs without bc_slot) into physical regs.
        for (uint32_t v = 0; v < ctx->func->nvreg && nslots < XIR_MAX_OSR_SLOTS; v++) {
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

static void patch_branches(CodegenCtx *ctx) {
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

// Convert XIR deopt infos to runtime deopt entries using final regalloc state.
// Must be called after codegen (regalloc is finalized) but before ctx is freed.
static void build_runtime_deopt_table(CodegenCtx *ctx, XirCodegenResult *result) {
    XirFunc *func = ctx->func;
    result->ndeopt = 0;
    if (!func->deopt_infos || func->ndeopt == 0)
        return;

    for (uint32_t d = 0; d < func->ndeopt && result->ndeopt < XIR_MAX_RT_DEOPT_ENTRIES; d++) {
        XirDeoptInfo *src = &func->deopt_infos[d];
        XirRtDeoptEntry *dst = &result->deopt_entries[result->ndeopt];
        dst->bc_pc = src->bc_pc;
        dst->deopt_id = src->deopt_id;
        dst->nslots = 0;

        for (uint16_t s = 0; s < src->nslots && dst->nslots < XIR_MAX_DEOPT_SLOTS; s++) {
            XirDeoptSlot *slot = &src->slots[s];
            XirRtDeoptSlot *rs = &dst->slots[dst->nslots];
            rs->bc_slot = slot->bc_slot;
            rs->type = slot->rep;
            rs->xr_tag = slot->xr_tag;
            memset(rs->_pad, 0, sizeof(rs->_pad));

            XirRef ref = slot->value;
            if (xir_ref_is_none(ref))
                continue;

            if (xir_ref_is_const(ref)) {
                // Constant: embed value directly
                uint32_t ci = XIR_REF_INDEX(ref);
                XirConst *c = &func->consts[ci];
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
                uint32_t vi = XIR_REF_INDEX(ref);
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
                        XirIns *def = (cur < func->nvreg) ? func->vregs[cur].def : NULL;
                        if (!def)
                            break;
                        if (!xir_op_is_copy(def->op))
                            break;
                        XirRef src = def->args[0];
                        if (!xir_ref_is_vreg(src))
                            break;
                        uint32_t sv = XIR_REF_INDEX(src);
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
 * When a coroutine is JIT-suspended (XIR_SUSPEND returned SUSPEND_MARKER),
 * the worker calls this entry point to re-enter JIT code. The stub:
 * 1. Sets up the same frame as the normal prologue
 * 2. Reloads ALL live registers from coro->jit_suspend (pointer)
 * 3. Loads the await result into the correct physical register
 * 4. Dispatches to the continuation point via suspend_id jump table
 *
 * Calling convention: same as XirJitFn — x0=coro, x1=unused
 * Returns: XrJitResult in x0 (payload) + x1 (tag)
 */
static void emit_resume_entry(CodegenCtx *ctx, XirCodegenResult *result) {
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
    a64_buf_emit(&ctx->buf, a64_ldr(JIT_CTX_REG, CORO_REG, XIR_CORO_JIT_CTX_OFFSET));

    // Store stack_map_ptr into frame
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_ACTIVE_SMAP_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, A64_FP, FRAME_SMAP_PTR_OFFSET));
    // Initialize safepoint_id = UINT32_MAX
    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, 0xFFFF, 0));
    a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG, 0xFFFF, 16));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, A64_FP, FRAME_SMAP_ID_OFFSET));
    a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_ACTIVE_SMAP_ID_OFFSET));
    // Save JIT frame SP for GC
    a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XIR_JIT_FRAME_SP_OFFSET));

    // === Load suspend_id ===
    // LDR w16, [x19, #suspend_id_offset]
    a64_buf_emit(&ctx->buf, a64_ldr_w(SCRATCH_REG, CORO_REG, XIR_CORO_SUSPEND_ID_OFFSET));

    // === Reload ALL registers from coro->jit_suspend (pointer deref) ===
    // LDR x17, [x19, #jit_suspend_ptr_offset]
    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, CORO_REG, XIR_CORO_SUSPEND_PTR_OFFSET));

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
                 a64_ldr(SAFEPT_PAGE_REG, JIT_CTX_REG, (int32_t) XIR_JIT_SAFEPOINT_PAGE_OFFSET));

    // === Clear jit_resume_entry (one-shot: prevent double-resume) ===
    a64_buf_emit(&ctx->buf, a64_str(A64_XZR, CORO_REG, XIR_CORO_RESUME_ENTRY_OFFSET));

    // === Restore spill slots from suspend_state.spill[] into new frame ===
    // The XIR_SUSPEND codegen saved the old frame's spill area into
    // suspend_state.spill[0..nspill-1].  Copy them to the new frame.
    {
        uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
        if (ns > XIR_SUSPEND_SPILL_MAX)
            ns = XIR_SUSPEND_SPILL_MAX;
        if (ns > 0) {
            // Load jit_suspend pointer into SCRATCH_REG for spill access
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, CORO_REG, XIR_CORO_SUSPEND_PTR_OFFSET));
            for (uint32_t s = 0; s < ns; s++) {
                int32_t regs_off = XIR_SUSPEND_SPILL_OFF + (int32_t) s * 8;
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
    a64_load_imm64(&ctx->buf, A64_X0, (uint64_t) XIR_DEOPT_MARKER);
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
        a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, CORO_REG, XIR_CORO_SUSPEND_PTR_OFFSET));

        // Load result from suspend_state.result into the correct register
        A64Reg rd = (A64Reg) ctx->suspend_result_regs[i];
        if (rd != A64_XZR) {
            a64_buf_emit(&ctx->buf, a64_ldr(rd, SCRATCH_REG2, XIR_SUSPEND_RESULT_OFF));
        }

        // Load result_tag and write to runtime_tags[bc_slot]
        int16_t bc_slot = ctx->suspend_result_bc_slots[i];
        if (bc_slot >= 0 && bc_slot < 256) {
            a64_buf_emit(&ctx->buf,
                         a64_ldrb(SCRATCH_REG, SCRATCH_REG2, (int32_t) XIR_SUSPEND_RESULT_TAG_OFF));
            int32_t tag_off = (int32_t) XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot;
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

/* ========== Main Codegen Entry ========== */

XirCodegenResult xir_codegen_arm64(XirFunc *func, XirCodeAlloc *alloc) {
    XR_DCHECK(func != NULL, "xir_codegen_arm64: func is NULL");
    XR_DCHECK(alloc != NULL, "xir_codegen_arm64: alloc is NULL");
    XirCodegenResult result = {
        .code = NULL, .code_size = 0, .success = false, .error = NULL, .nosr = 0};

    // Validate consistency between hardcoded constants and XirTarget
    XR_DCHECK(xir_current_target != NULL, "assertion failed");
    XR_DCHECK(xir_current_target->ngpr == MAX_PHYS_REGS, "assertion failed");
    XR_DCHECK(xir_current_target->nfpr == MAX_FP_REGS, "assertion failed");
    XR_DCHECK(xir_current_target->max_vregs == MAX_VREGS, "assertion failed");
    XR_DCHECK(xir_current_target->scratch_gpr[0] == SCRATCH_REG, "assertion failed");
    XR_DCHECK(xir_current_target->scratch_gpr[1] == SCRATCH_REG2, "assertion failed");
    XR_DCHECK(xir_current_target->coro_reg == CORO_REG, "assertion failed");
    XR_DCHECK(xir_current_target->spill_base == SPILL_BASE, "assertion failed");
    XR_DCHECK(xir_current_target->frame_base == JIT_FRAME_BASE, "assertion failed");

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
        XirBlock *blk = func->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nins; i++) {
            XirIns *ins = &blk->ins[i];
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t vi = XIR_REF_INDEX(ins->dst);
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
    xir_coalesce(func);

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

    // Memory estimate: 32 ARM64 insts per XIR ins + overhead.
    // CALL_DIRECT/CALL_KNOWN emit 50+ instructions (reg save/restore, frame push/pop,
    // safepoint bitmap store), so 8 was too conservative and caused buffer overflows.
    uint32_t total_xir_ins = 0;
    for (uint32_t i = 0; i < func->nblk; i++) {
        total_xir_ins += func->blocks[i]->nins + 4;
    }
    uint32_t alloc_size = (total_xir_ins * 32 + 256) * 4;
    alloc_size = (alloc_size + 4095) & ~(uint32_t) 4095;
    if (alloc_size < 8192)
        alloc_size = 8192;

    void *code_mem = xir_code_alloc(alloc, alloc_size, 16);
    if (!code_mem) {
        result.error = "failed to allocate executable memory";
        xr_free(ctx.block_offsets);
        xr_free(ctx.patches);
        xr_free(ctx.cs_patches);
        xra_result_free(ctx.xra);
        return result;
    }

#ifdef XR_OS_MACOS
    xir_code_make_writable(code_mem, alloc_size);
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
        XirBlock *blk = func->blocks[i];
        if (blk->is_loop_header && blk->jmp.type == XIR_JMP_BR && blk->s1) {
            XirBlock *body = blk->s1;
            if (func->blocks[i + 1] != body) {
                for (uint32_t j = i + 2; j < func->nblk; j++) {
                    if (func->blocks[j] == body) {
                        XirBlock *tmp = func->blocks[j];
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
    emit_barrier_stubs(&ctx);
    emit_deopt_stub(&ctx);
    emit_call_c_stub(&ctx);
    emit_osr_stubs(&ctx, &result);
    emit_resume_entry(&ctx, &result);

    // Patch all branches
    patch_branches(&ctx);

    // Build runtime deopt table from XIR deopt infos + regalloc state
    build_runtime_deopt_table(&ctx, &result);

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
    xir_peephole(&ctx.buf);

    uint32_t code_size = a64_buf_offset(&ctx.buf);

    xir_code_make_executable(code_mem, code_size);
    xir_code_flush_icache(code_mem, code_size);

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

// xir_insert_write_barriers moved to xir_pass.c

#endif  // __aarch64__
