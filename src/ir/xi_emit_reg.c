/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit_reg.c - Register allocation and liveness computation
 *
 * Handles last-use tracking for register recycling and initial
 * register assignment for params and phi nodes.
 */

#include "xi_emit_internal.h"

/* ========== Last-Use Computation ========== */

/* Pre-compute last-use ordinals for register recycling.
 * Walks all blocks in RPO, assigning each value a monotonic ordinal.
 * For each arg reference, updates last_use[arg_id] = max ordinal.
 * Also accounts for block terminators that reference values. */
XR_FUNC void compute_last_use(EmitCtx *ctx) {
    uint32_t ord = 1;
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            /* Update last-use of all args referenced by this value */
            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *arg = v->args[a];
                if (arg && arg->id < ctx->reg_map_size)
                    ctx->last_use[arg->id] = ord;
            }
            ord++;
        }

        /* Terminator references: control value and phi args in successors */
        if (blk->control && blk->control->id < ctx->reg_map_size)
            ctx->last_use[blk->control->id] = ord;

        /* Phi args from this block's successors reference values too */
        for (int s = 0; s < 2; s++) {
            XiBlock *succ = blk->succs[s];
            if (!succ)
                continue;
            int pred_idx = -1;
            for (uint16_t p = 0; p < succ->npreds; p++) {
                if (succ->preds[p] == blk) {
                    pred_idx = (int) p;
                    break;
                }
            }
            if (pred_idx < 0)
                continue;
            for (XiPhi *phi = succ->phis; phi; phi = phi->next) {
                if ((uint16_t) pred_idx < phi->value.nargs) {
                    XiValue *src = phi->value.args[pred_idx];
                    if (src && src->id < ctx->reg_map_size)
                        ctx->last_use[src->id] = ord;
                }
            }
        }
        ord++; /* account for terminator */
    }

    /* Phi registers must never be freed: they are referenced by
     * emit_phi_moves from any predecessor, which is not captured
     * by the ordinal-based last-use tracking above. */
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk)
            continue;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (phi->value.id < ctx->reg_map_size)
                ctx->last_use[phi->value.id] = UINT32_MAX;
        }
    }

    /* Loop-invariant liveness: values defined outside a loop but used inside
     * must stay live for the entire loop — the single RPO walk above only
     * records one ordinal per use, but the VM re-executes loop blocks.
     *
     * Algorithm: detect back edges (succ.rpo <= block.rpo).  For each back
     * edge target (loop header), any value whose def-block RPO < header RPO
     * and whose last_use falls inside the loop range must be pinned. */
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk)
            continue;
        for (int s = 0; s < 2; s++) {
            XiBlock *succ = blk->succs[s];
            if (!succ || succ->rpo == 0)
                continue;
            if (succ->rpo > blk->rpo)
                continue; /* not a back edge */

            /* Back edge: blk → succ.  Loop spans RPO [succ->rpo, blk->rpo].
             * Pin every value defined before the loop that is used inside. */
            uint32_t loop_lo = succ->rpo;
            uint32_t loop_hi = blk->rpo;
            for (uint32_t lr = loop_lo; lr <= loop_hi; lr++) {
                XiBlock *lb = ctx->rpo_order[lr];
                if (!lb)
                    continue;
                for (uint32_t i = 0; i < lb->nvalues; i++) {
                    XiValue *v = lb->values[i];
                    for (uint16_t a = 0; a < v->nargs; a++) {
                        XiValue *arg = v->args[a];
                        if (!arg || arg->id >= ctx->reg_map_size)
                            continue;
                        if (!arg->block)
                            continue;
                        if (arg->block->rpo > 0 && arg->block->rpo < loop_lo)
                            ctx->last_use[arg->id] = UINT32_MAX;
                    }
                }
            }
        }
    }
}

/* ========== Register Allocation ========== */

/* Params get R[0..nparams-1], phis pre-assigned, last-use computed. */
XR_FUNC void alloc_registers(EmitCtx *ctx) {
    XiFunc *f = ctx->func;

    /* Assign parameter registers by scanning entry block for XI_PARAM ops.
     * This is robust whether f->params is populated or not. */
    XiBlock *entry = f->entry;
    if (entry) {
        for (uint32_t i = 0; i < entry->nvalues; i++) {
            XiValue *v = entry->values[i];
            if (v->op == XI_PARAM) {
                uint16_t pidx = (uint16_t) v->aux_int;
                if (v->id < ctx->reg_map_size && pidx < MAX_REGS) {
                    ctx->reg_map[v->id] = (uint8_t) pidx;
                    if (pidx + 1 > ctx->next_reg) {
                        ctx->next_reg = (uint8_t) (pidx + 1);
                        ctx->max_reg = ctx->next_reg;
                    }
                }
            }
        }
    }

    /* Pre-assign phi registers to avoid conflicts with phi moves.
     * Phis get their own registers before instruction values. */
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            (void) reg_of(ctx, &phi->value);
            if (ctx->status != XI_EMIT_OK)
                return;
        }
    }
}
