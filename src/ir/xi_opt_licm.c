/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_licm.c - Loop-Invariant Code Motion for Xi IR
 *
 * ALGORITHM:
 *   For each loop (innermost-first via XiLoopInfo):
 *     1. Build a per-block membership bitmap from the loop body.
 *     2. Build a value→block mapping (def_block[value_id] = block_index).
 *     3. For each pure value in the loop body: if all operands are
 *        defined outside the loop, move the value to the preheader.
 *     4. Iterate to handle chain-invariant propagation (a value
 *        whose operand was just hoisted becomes hoistable).
 *
 *   Hoisting works by removing the value from its source block's
 *   values[] array and appending it to the preheader's values[] array.
 *   The value pointer itself stays valid (arena-allocated).
 *
 * LIMITATIONS:
 *   - Only hoists pure arithmetic/comparison/bitwise values.
 *   - Does not hoist loads (Xi IR lacks field-level alias info).
 *   - Requires a unique preheader; skips loops without one.
 */

#include "xi_opt_licm.h"
#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

#define LICM_MAX_ITERATIONS 8

/* ========== Pure-Op Classification ========== */

static bool licm_is_pure(const XiValue *v) {
    if (!v) return false;
    if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_WRITES_MEM))
        return false;
    switch (v->op) {
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_NEG:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_BNOT:
        case XI_SHL: case XI_SHR:
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE:
        case XI_NOT:
        case XI_ISNULL:
        case XI_CONVERT:
        case XI_COPY:
        case XI_BOX: case XI_UNBOX:
            return true;
        default:
            return false;
    }
}

/* ========== Def-Block Mapping ========== */

/* Build value_id → block_index mapping for all values in the function. */
static void build_def_block(const XiFunc *f, uint32_t *def_blk, uint32_t max_id) {
    memset(def_blk, 0xFF, max_id * sizeof(uint32_t));  /* UINT32_MAX = unknown */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (v && v->id < max_id)
                def_blk[v->id] = bi;
        }
        /* Phi outputs are defined in their block */
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (phi->value.id < max_id)
                def_blk[phi->value.id] = bi;
        }
    }
    /* Params defined in entry block */
    for (uint16_t p = 0; p < f->nparams; p++) {
        if (f->params[p] && f->params[p]->id < max_id)
            def_blk[f->params[p]->id] = 0;
    }
}

/* Check if a value's definition is outside the loop body. */
static bool def_outside_loop(const XiValue *v, const uint32_t *def_blk,
                              uint32_t max_id, const bool *in_loop,
                              uint32_t nblocks) {
    if (!v) return true;  /* NULL arg (e.g. CLOSURE_NEW) treated as outside */
    if (v->op == XI_CONST) return true;  /* Constants are loop-invariant */
    if (v->id >= max_id) return false;
    uint32_t bi = def_blk[v->id];
    if (bi >= nblocks) return false;
    return !in_loop[bi];
}

/* ========== Block Array Manipulation ========== */

/* Append a value to a block's values[] array. */
static bool block_append_value(XiBlock *blk, XiValue *v) {
    if (blk->nvalues >= blk->values_cap) {
        uint32_t new_cap = blk->values_cap ? blk->values_cap * 2 : 8;
        XiValue **tmp = NULL;
        if (blk->values) {
            tmp = (XiValue **)xr_malloc(new_cap * sizeof(XiValue *));
            if (!tmp) return false;
            memcpy(tmp, blk->values, blk->nvalues * sizeof(XiValue *));
            /* old array is arena-allocated, no free needed */
        } else {
            tmp = (XiValue **)xr_calloc(new_cap, sizeof(XiValue *));
            if (!tmp) return false;
        }
        blk->values = tmp;
        blk->values_cap = new_cap;
    }
    blk->values[blk->nvalues++] = v;
    return true;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_licm(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_licm: NULL func");
    if (f->nblocks < 2) return xi_pass_no_change();

    /* Compute analysis prerequisites */
    xi_compute_rpo(f);
    xi_compute_dominators(f);
    XiLoopInfo *loops = xi_compute_loops(f);
    if (!loops || loops->nloop == 0) {
        if (loops) xi_loopinfo_free(loops);
        return xi_pass_no_change();
    }

    uint32_t max_id = f->next_value_id;
    uint32_t nblk = f->nblocks;

    uint32_t *def_blk = (uint32_t *)xr_malloc(max_id * sizeof(uint32_t));
    bool *in_loop = (bool *)xr_calloc(nblk, sizeof(bool));
    if (!def_blk || !in_loop) {
        xr_free(def_blk); xr_free(in_loop);
        xi_loopinfo_free(loops);
        return xi_pass_no_change();
    }

    build_def_block(f, def_blk, max_id);

    bool any_hoisted = false;

    /* Process innermost-first (all_loops[] sorted by XiLoopInfo) */
    for (uint32_t li = 0; li < loops->nloop; li++) {
        XiLoop *L = loops->all_loops[li];
        if (!L->preheader) continue;  /* no preheader → skip */

        /* Build loop body bitmap */
        memset(in_loop, 0, nblk * sizeof(bool));
        for (uint32_t i = 0; i < L->nbody; i++) {
            XR_DCHECK(L->body[i] != NULL, "LICM: NULL body block");
            if (L->body[i]->id < nblk)
                in_loop[L->body[i]->id] = true;
        }

        XiBlock *preheader = L->preheader;
        XR_DCHECK(preheader != NULL, "LICM: NULL preheader");

        /* Iterative hoisting for chain-invariant propagation */
        for (int iter = 0; iter < LICM_MAX_ITERATIONS; iter++) {
            bool hoisted_this_iter = false;

            for (uint32_t bi_idx = 0; bi_idx < L->nbody; bi_idx++) {
                XiBlock *blk = L->body[bi_idx];
                if (blk == preheader) continue;

                uint32_t write = 0;
                for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                    XiValue *v = blk->values[vi];
                    bool can_hoist = false;

                    if (v && licm_is_pure(v)) {
                        can_hoist = true;
                        for (uint16_t a = 0; a < v->nargs; a++) {
                            if (!def_outside_loop(v->args[a], def_blk,
                                                   max_id, in_loop, nblk)) {
                                can_hoist = false;
                                break;
                            }
                        }
                    }

                    if (can_hoist) {
                        /* Move to preheader */
                        if (block_append_value(preheader, v)) {
                            /* Update def_block mapping */
                            if (v->id < max_id)
                                def_blk[v->id] = preheader->id;
                            hoisted_this_iter = true;
                            any_hoisted = true;
                        } else {
                            /* Allocation failure: keep in place */
                            blk->values[write++] = v;
                        }
                    } else {
                        blk->values[write++] = v;
                    }
                }
                blk->nvalues = write;
            }

            if (!hoisted_this_iter) break;
        }
    }

    xr_free(def_blk);
    xr_free(in_loop);
    xi_loopinfo_free(loops);

    if (!any_hoisted) return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();
    chg.values_changed = true;
    return chg;
}
