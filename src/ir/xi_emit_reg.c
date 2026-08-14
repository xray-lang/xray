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
#include "xi_analysis.h"
#include "xi_op_name.h"
#include <stdio.h>
#include <stdlib.h>

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

/* ========== Variable-Coalescing Interference Detection ========== */

/*
 * reg_of() maps every SSA value carrying the same var_id onto one VM
 * register.  Reading that as SSA coalescing suggests an invariant — at most
 * one value per var_id live at any point — and a value whose reader runs
 * after a later same-var definition then sees the wrong version with no
 * diagnostic, because nothing models the shared register: XRAY_XI_CHECK
 * passes and the bytecode is well-formed.  Loop-invariant motion produced
 * exactly that failure by hoisting an inner-loop initializer past the outer
 * loop.
 *
 * The invariant does not hold, and is not what the emitter implements.  A
 * var_id names a storage cell, not a value: `defer` late binding, catch
 * blocks reached by OP_THROW outside the CFG, and reloads of a `ref` place
 * after a call all deliberately read the cell's latest contents through an
 * older SSA name.  Measured over the diff, regression and stdlib corpora,
 * overlapping same-var live ranges occur in around thirty functions, most of
 * them load-bearing, so overlap alone cannot be a rejection criterion and
 * this must not become an emit gate.
 *
 * What overlap does identify reliably is passes that emit a read of a stale
 * version.  It found an ARC release naming the pre-reassignment value of a
 * refcounted local, which releases whatever the cell holds afterwards and
 * aborts the VM on the next access.  So this stays an opt-in instrument:
 * set XRAY_XI_VAR_INTERFERENCE=1 to list every overlap with its function,
 * source variable, and the two values involved.
 *
 * The AOT backend materializes real phi copies and never consults var_id, so
 * none of this applies to it; it describes the VM emitter alone.
 */

typedef struct {
    XiValue *value;
    XiBlock *block;
    uint32_t index; /* position in block->values, or UINT32_MAX for a phi */
} XiVarDef;

static uint32_t var_index_in_block(const XiBlock *blk, const XiValue *v) {
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == v)
            return i;
    }
    return UINT32_MAX;
}

static bool var_is_phi_of(const XiBlock *blk, const XiValue *v) {
    for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
        if (&phi->value == v)
            return true;
    }
    return false;
}

/* True when some edge leaving blk feeds u into a successor phi.  Those reads
 * happen on the edge, so live_out does not record them. */
static bool var_edge_phi_reads(const XiBlock *blk, const XiValue *u) {
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
        for (const XiPhi *phi = succ->phis; phi; phi = phi->next) {
            if ((uint16_t) pred_idx < phi->value.nargs && phi->value.args[pred_idx] == u)
                return true;
        }
    }
    return false;
}

/* True when u is still read after blk->values[from-1] has run.  A read by the
 * defining value itself is deliberately excluded by the caller's choice of
 * `from`: `j = j + 1` ends one live range exactly where the next begins, which
 * is the case coalescing exists to serve. */
static bool var_read_after(const XiLiveness *live, const XiBlock *blk, uint32_t from,
                           const XiValue *u) {
    for (uint32_t k = from; k < blk->nvalues; k++) {
        const XiValue *c = blk->values[k];
        for (uint16_t a = 0; a < c->nargs; a++) {
            if (c->args[a] == u)
                return true;
        }
    }
    if (blk->control == u)
        return true;
    if (var_edge_phi_reads(blk, u))
        return true;
    return xi_is_live_out(live, blk, u);
}

/* True when u already owns the coalesced register at the given definition. */
static bool var_holds_reg_at(const XiLiveness *live, const XiBlock *blk, uint32_t at,
                             const XiValue *u) {
    if (var_is_phi_of(blk, u))
        return true;
    if (xi_is_live_in(live, blk, u))
        return true;
    uint32_t ui = var_index_in_block(blk, u);
    return ui != UINT32_MAX && ui < at;
}

/* u interferes with the definition of w when u holds the shared register on
 * the way in and is still read on the way out. */
static bool var_defs_interfere(const XiLiveness *live, const XiVarDef *w, const XiValue *u) {
    XR_DCHECK(live != NULL && w != NULL && u != NULL, "var_defs_interfere: NULL input");
    XR_DCHECK(w->block != NULL, "var_defs_interfere: definition without a block");
    XR_DCHECK(w->value != u, "var_defs_interfere: value compared against itself");
    XiBlock *blk = w->block;
    if (w->index == UINT32_MAX) {
        /* w is a phi: it is defined at block entry, before any value runs. */
        if (!var_is_phi_of(blk, u) && !xi_is_live_in(live, blk, u))
            return false;
        return var_read_after(live, blk, 0, u);
    }
    if (!var_holds_reg_at(live, blk, w->index, u))
        return false;
    return var_read_after(live, blk, w->index + 1u, u);
}

static const char *var_source_name(const XiFunc *f, XiVarId var_id) {
    XR_DCHECK(f != NULL, "var_source_name: NULL func");
    if (!f->source_var_names || var_id >= f->source_var_count)
        return NULL;
    const char *name = f->source_var_names[var_id];
    return (name && name[0]) ? name : NULL;
}

static void var_report_interference(const XiFunc *f, XiVarId var_id, const XiValue *live_value,
                                    const XiVarDef *clobber) {
    const char *var_name = var_source_name(f, var_id);
    fprintf(stderr,
            "[xi_emit] variable-coalescing interference in function '%s': source variable "
            "'%s' (var_id %u) is held by value #%u (%s, line %u) and overwritten by the "
            "definition of value #%u (%s, line %u) in block b%u; both are mapped to one VM "
            "register\n",
            f->name ? f->name : "<anonymous>", var_name ? var_name : "<unnamed>",
            (unsigned) var_id, live_value->id, xi_op_name(live_value->op),
            (unsigned) live_value->line, clobber->value->id, xi_op_name(clobber->value->op),
            (unsigned) clobber->value->line, clobber->block->id);
}

/* Collect every definition that carries a var_id, in emission order. */
static uint32_t var_collect_defs(EmitCtx *ctx, XiVarDef *defs) {
    uint32_t n = 0;
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk)
            continue;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!xi_emit_var_id_in_state(ctx, phi->value.var_id))
                continue;
            defs[n].value = &phi->value;
            defs[n].block = blk;
            defs[n].index = UINT32_MAX;
            n++;
        }
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!xi_emit_var_id_in_state(ctx, v->var_id))
                continue;
            defs[n].value = v;
            defs[n].block = blk;
            defs[n].index = i;
            n++;
        }
    }
    return n;
}

/* Count the definitions var_collect_defs will produce. */
static uint32_t var_count_defs(EmitCtx *ctx) {
    uint32_t n = 0;
    for (uint32_t r = 1; r <= ctx->rpo_count; r++) {
        XiBlock *blk = ctx->rpo_order[r];
        if (!blk)
            continue;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (xi_emit_var_id_in_state(ctx, phi->value.var_id))
                n++;
        }
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (xi_emit_var_id_in_state(ctx, blk->values[i]->var_id))
                n++;
        }
    }
    return n;
}

static bool var_interference_reporting_enabled(void) {
    const char *env = getenv("XRAY_XI_VAR_INTERFERENCE");
    return env && env[0] == '1' && env[1] == '\0';
}

XR_FUNC void check_var_interference(EmitCtx *ctx) {
    XR_DCHECK(ctx != NULL && ctx->func != NULL, "check_var_interference: NULL context");
    XiFunc *f = ctx->func;
    if (ctx->var_state_count == 0)
        return;
    if (!var_interference_reporting_enabled())
        return;

    uint32_t ndefs = var_count_defs(ctx);
    if (ndefs < 2)
        return;

    XiVarDef *defs = (XiVarDef *) xr_malloc((size_t) ndefs * sizeof(*defs));
    uint32_t *bucket = (uint32_t *) xr_malloc((size_t) ctx->var_state_count * sizeof(*bucket));
    uint32_t *next = (uint32_t *) xr_malloc((size_t) ndefs * sizeof(*next));
    XiLiveness *live = f->entry && f->entry->rpo > 0 ? xi_compute_liveness(f) : NULL;
    if (!defs || !bucket || !next || !live) {
        xi_liveness_free(live);
        xr_free(next);
        xr_free(bucket);
        xr_free(defs);
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    uint32_t collected = var_collect_defs(ctx, defs);
    XR_DCHECK(collected == ndefs, "check_var_interference: definition count drifted");

    for (uint32_t i = 0; i < ctx->var_state_count; i++)
        bucket[i] = UINT32_MAX;
    /* Chain each var_id's definitions, newest first. */
    for (uint32_t i = 0; i < collected; i++) {
        XiVarId var_id = defs[i].value->var_id;
        next[i] = bucket[var_id];
        bucket[var_id] = i;
    }

    for (uint32_t i = 0; i < collected; i++) {
        XiVarId var_id = defs[i].value->var_id;
        for (uint32_t j = bucket[var_id]; j != UINT32_MAX; j = next[j]) {
            if (j == i)
                continue;
            if (!var_defs_interfere(live, &defs[i], defs[j].value))
                continue;
            var_report_interference(f, var_id, defs[j].value, &defs[i]);
        }
    }

    xi_liveness_free(live);
    xr_free(next);
    xr_free(bucket);
    xr_free(defs);
}

/* ========== Register Allocation ========== */

/* Params get R[0..nparams-1], phis pre-assigned, last-use computed. */
XR_FUNC void alloc_registers(EmitCtx *ctx) {
    XiFunc *f = ctx->func;

    /* Verify the precondition of var_id register coalescing before any
     * value is pinned, so a violation is a refusal instead of a wrong answer. */
    check_var_interference(ctx);
    if (ctx->status != XI_EMIT_OK)
        return;

    /* Assign parameter registers by scanning entry block for XI_PARAM ops.
     * This is robust whether f->params is populated or not. */
    XiBlock *entry = f->entry;
    if (entry) {
        for (uint32_t i = 0; i < entry->nvalues; i++) {
            XiValue *v = entry->values[i];
            if (v->op == XI_PARAM) {
                uint16_t pidx = (uint16_t) v->aux_int;
                if (v->id < ctx->reg_map_size && pidx < MAX_REGS) {
                    ctx->reg_map[v->id] = (XiEmitReg) pidx;
                    if (xi_var_id_is_valid(v->var_id) && !xi_emit_var_id_in_state(ctx, v->var_id)) {
                        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
                        return;
                    }
                    if (xi_emit_var_id_in_state(ctx, v->var_id) &&
                        ctx->var_reg[v->var_id] == NO_REG)
                        ctx->var_reg[v->var_id] = (XiEmitReg) pidx;
                    if (pidx + 1 > ctx->next_reg) {
                        ctx->next_reg = pidx + 1;
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
