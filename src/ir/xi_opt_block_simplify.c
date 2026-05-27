/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_block_simplify.c - CFG block simplification for Xi IR
 *
 * Performs iterative simplification until no more changes:
 *   1. Empty block elimination: a PLAIN block with no values/phis is
 *      bypassed (predecessors redirect to its successor)
 *   2. Block merge: a block with exactly one predecessor where that
 *      predecessor has exactly one successor is merged into the pred
 */

#include "xi_opt_block_simplify.h"
#include "xi_cfg_edit.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

/* ========== Helpers ========== */

/* Check if a block is empty: no values and no phis. */
static bool block_is_empty(const XiBlock *blk) {
    return blk->nvalues == 0 && blk->phis == NULL;
}

/* ========== Empty Block Elimination ========== */

/* Try to eliminate an empty PLAIN block by redirecting its preds to
 * its successor. Returns true if the block was eliminated.
 *
 * Multi-predecessor handling: an empty PLAIN block contributes a single
 * incoming slot to every phi in succ. After elimination each of blk's
 * preds reaches succ on its own edge, so every such phi must gain
 * (blk->npreds - 1) extra slots, all carrying the value that was at the
 * original blk slot (the empty block performs no value rewrites, so the
 * phi-incoming is identical for every replacement edge). */
static bool try_eliminate_empty(XiFunc *f, XiBlock *blk) {
    if (blk->kind != XI_BLOCK_PLAIN)
        return false;
    if (!blk->succs[0])
        return false;
    if (!block_is_empty(blk))
        return false;
    if (blk == f->entry)
        return false;
    if (blk->npreds == 0)
        return false;

    XiBlock *succ = blk->succs[0];
    XR_DCHECK(succ != NULL, "empty block has NULL successor");

    uint16_t blk_idx = xi_cfg_pred_index(succ, blk);
    if (blk_idx == succ->npreds)
        return false; /* succ does not list blk — defensive bail */

    /* Snapshot per-phi incoming values at the slot blk currently occupies
     * before we mutate the pred / phi arrays. */
    uint32_t nphis = xi_cfg_phi_count(succ);
    XiValue **phi_incoming = NULL;
    if (nphis > 0) {
        phi_incoming = (XiValue **) xi_func_arena_alloc(f, nphis * sizeof(XiValue *));
        if (!phi_incoming)
            return false;
        uint32_t i = 0;
        for (XiPhi *p = succ->phis; p; p = p->next, i++) {
            XR_DCHECK(blk_idx < p->value.nargs,
                      "phi.nargs and succ.npreds out of sync before elimination");
            phi_incoming[i] = p->value.args[blk_idx];
        }
    }

    /* Redirect every predecessor of blk to succ. The first one reuses
     * blk's slot; the rest are appended (with matching phi.args). */
    XiBlock *first_pred = blk->preds[0];
    if (!xi_cfg_replace_pred(succ, blk, first_pred))
        return false;
    if (!xi_cfg_replace_successor(first_pred, blk, succ))
        return false;

    for (uint16_t i = 1; i < blk->npreds; i++) {
        XiBlock *pred = blk->preds[i];
        XR_DCHECK(pred != NULL, "NULL in pred list");
        if (!xi_cfg_replace_successor(pred, blk, succ))
            return false;
        if (!xi_cfg_append_pred(succ, pred, phi_incoming, nphis))
            return false;
    }

    blk->kind = XI_BLOCK_UNREACHABLE;
    blk->control = NULL;
    blk->succs[0] = NULL;
    blk->succs[1] = NULL;
    blk->npreds = 0;
    return true;
}

/* ========== Block Merge ========== */

/* Try to merge blk into its sole predecessor.  Returns true if merged.
 * Conditions: blk has exactly one predecessor, and that pred has
 * exactly one successor (blk). */
static bool try_merge_into_pred(XiFunc *f, XiBlock *blk) {
    (void) f;

    /* Must have exactly one predecessor. */
    if (blk->npreds != 1)
        return false;
    /* Must not be entry block. */
    if (blk == f->entry)
        return false;

    XiBlock *pred = blk->preds[0];
    XR_DCHECK(pred != NULL, "single pred is NULL");

    /* Pred must have exactly one successor (this block). */
    if (pred->kind != XI_BLOCK_PLAIN)
        return false;
    if (pred->succs[0] != blk)
        return false;

    /* Blk must have no phis (since single pred, phis are trivial and
     * should have been simplified already by phi_simplify). */
    if (blk->phis != NULL)
        return false;

    /* Merge: append blk's values to pred and rebind their block pointer
     * so that xi_verify's "value->block == containing block" invariant
     * holds after the merge. */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (!v)
            continue;
        if (pred->nvalues >= pred->values_cap) {
            uint32_t new_cap = pred->values_cap ? pred->values_cap * 2 : 16;
            XR_REALLOC_OR_ABORT(pred->values, new_cap * sizeof(XiValue *), "block merge values");
            pred->values_cap = new_cap;
        }
        pred->values[pred->nvalues++] = v;
        v->block = pred;
    }

    /* Transfer terminator: pred takes blk's kind, control, and succs. */
    pred->kind = blk->kind;
    pred->control = blk->control;
    pred->succs[0] = blk->succs[0];
    pred->succs[1] = blk->succs[1];

    /* Update successors' pred lists: replace blk with pred. */
    if (pred->succs[0])
        xi_cfg_replace_pred(pred->succs[0], blk, pred);
    if (pred->succs[1])
        xi_cfg_replace_pred(pred->succs[1], blk, pred);

    /* Mark blk as dead. */
    blk->kind = XI_BLOCK_UNREACHABLE;
    blk->succs[0] = NULL;
    blk->succs[1] = NULL;
    blk->nvalues = 0;
    blk->npreds = 0;
    return true;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_block_simplify(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_block_simplify: NULL func");

    if (f->nblocks <= 1)
        return xi_pass_no_change();

    bool any_change = false;
    bool changed = true;

    /* Iterate until fixed point. */
    while (changed) {
        changed = false;

        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            XiBlock *blk = f->blocks[bi];
            if (blk->kind == XI_BLOCK_UNREACHABLE)
                continue;

            if (try_eliminate_empty(f, blk)) {
                changed = true;
                any_change = true;
                continue;
            }

            if (try_merge_into_pred(f, blk)) {
                changed = true;
                any_change = true;
                continue;
            }
        }
    }

    /* Remove dead blocks from array. */
    uint32_t removed = xi_cfg_compact_blocks(f);

    XiPassChange chg = xi_pass_no_change();
    if (any_change || removed > 0) {
        chg.cfg_changed = true;
        chg.n_removed = removed;
    }
    return chg;
}
