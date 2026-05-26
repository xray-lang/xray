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
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Helpers ========== */

/* Count how many successors a block has (0, 1, or 2). */
static uint32_t succ_count(const XiBlock *blk) {
    if (blk->kind == XI_BLOCK_RETURN || blk->kind == XI_BLOCK_UNREACHABLE)
        return 0;
    if (blk->kind == XI_BLOCK_IF)
        return 2;
    /* PLAIN */
    return blk->succs[0] ? 1 : 0;
}

/* Replace occurrences of old_pred with new_pred in blk's pred list.
 * Also fixes phi args: phi arg for the old_pred slot remains correct
 * since we just swap the predecessor identity. */
static void replace_pred(XiBlock *blk, XiBlock *old_pred, XiBlock *new_pred) {
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] == old_pred) {
            blk->preds[i] = new_pred;
            return;
        }
    }
}

/* Replace successor of src from old_succ to new_succ. */
static void replace_succ(XiBlock *src, XiBlock *old_succ, XiBlock *new_succ) {
    if (src->succs[0] == old_succ)
        src->succs[0] = new_succ;
    if (src->succs[1] == old_succ)
        src->succs[1] = new_succ;
}

/* Remove blk from target's pred list entirely. */
static void remove_pred(XiBlock *target, XiBlock *blk) {
    for (uint16_t i = 0; i < target->npreds; i++) {
        if (target->preds[i] == blk) {
            /* Shift remaining preds down */
            for (uint16_t j = i; j + 1 < target->npreds; j++)
                target->preds[j] = target->preds[j + 1];
            target->npreds--;

            /* Also remove phi arg at index i for all phis */
            for (XiPhi *phi = target->phis; phi; phi = phi->next) {
                if (i < phi->value.nargs) {
                    for (uint16_t j = i; j + 1 < phi->value.nargs; j++)
                        phi->value.args[j] = phi->value.args[j + 1];
                    phi->value.nargs--;
                }
            }
            return;
        }
    }
}

/* Check if a block is empty: no values and no phis. */
static bool block_is_empty(const XiBlock *blk) {
    return blk->nvalues == 0 && blk->phis == NULL;
}

/* ========== Empty Block Elimination ========== */

/* Try to eliminate an empty PLAIN block by redirecting its preds
 * to its successor.  Returns true if the block was eliminated. */
static bool try_eliminate_empty(XiFunc *f, XiBlock *blk) {
    /* Must be PLAIN with exactly one successor. */
    if (blk->kind != XI_BLOCK_PLAIN)
        return false;
    if (!blk->succs[0])
        return false;
    /* Must be truly empty (no values, no phis). */
    if (!block_is_empty(blk))
        return false;
    /* Must not be entry block. */
    if (blk == f->entry)
        return false;

    XiBlock *succ = blk->succs[0];
    XR_DCHECK(succ != NULL, "empty block has NULL successor");

    /* For each predecessor of blk, redirect to succ. */
    for (uint16_t i = 0; i < blk->npreds; i++) {
        XiBlock *pred = blk->preds[i];
        XR_DCHECK(pred != NULL, "NULL in pred list");
        replace_succ(pred, blk, succ);
        /* Add pred to succ's pred list (replacing blk's entry). */
        replace_pred(succ, blk, pred);
    }

    /* If succ still references blk in preds (maybe multiple edges), clean up */
    /* Mark block as unreachable for later removal. */
    blk->kind = XI_BLOCK_UNREACHABLE;
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

    /* Merge: append blk's values to pred. */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (!v)
            continue;
        /* Ensure pred has capacity. */
        if (pred->nvalues >= pred->values_cap) {
            uint32_t new_cap = pred->values_cap ? pred->values_cap * 2 : 16;
            XR_REALLOC_OR_ABORT(pred->values, new_cap * sizeof(XiValue *), "block merge values");
            pred->values_cap = new_cap;
        }
        pred->values[pred->nvalues++] = v;
    }

    /* Transfer terminator: pred takes blk's kind, control, and succs. */
    pred->kind = blk->kind;
    pred->control = blk->control;
    pred->succs[0] = blk->succs[0];
    pred->succs[1] = blk->succs[1];

    /* Update successors' pred lists: replace blk with pred. */
    if (pred->succs[0])
        replace_pred(pred->succs[0], blk, pred);
    if (pred->succs[1])
        replace_pred(pred->succs[1], blk, pred);

    /* Mark blk as dead. */
    blk->kind = XI_BLOCK_UNREACHABLE;
    blk->succs[0] = NULL;
    blk->succs[1] = NULL;
    blk->nvalues = 0;
    blk->npreds = 0;
    return true;
}

/* ========== Dead Block Removal ========== */

/* Compact the block array, removing unreachable blocks. */
static uint32_t remove_dead_blocks(XiFunc *f) {
    uint32_t write = 0;
    uint32_t orig = f->nblocks;
    for (uint32_t i = 0; i < f->nblocks; i++) {
        XiBlock *blk = f->blocks[i];
        if (blk->kind != XI_BLOCK_UNREACHABLE || blk == f->entry) {
            f->blocks[write] = blk;
            f->blocks[write]->id = write;
            write++;
        }
    }
    f->nblocks = write;
    return orig - write;
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
    uint32_t removed = remove_dead_blocks(f);

    XiPassChange chg = xi_pass_no_change();
    if (any_change || removed > 0) {
        chg.cfg_changed = true;
        chg.n_removed = removed;
    }
    return chg;
}
