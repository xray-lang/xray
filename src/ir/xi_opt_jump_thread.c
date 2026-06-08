/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_jump_thread.c - Jump threading for Xi IR
 *
 * For each IF block whose control is defined by a comparison,
 * look at each predecessor edge.  If the comparison result is
 * already known along that edge (the same comparison was tested
 * in a dominating block), redirect the edge to the known-true
 * successor.
 *
 * Current scope: thread when a predecessor is an IF block testing
 * the same condition and the edge comes from the then/else side
 * that makes the target condition's outcome known.
 */

#include "xi_opt_jump_thread.h"
#include "xi_cfg_edit.h"
#include "xi_effect.h"
#include "../base/xchecks.h"

/* ========== Condition Matching ========== */

/* A comparison identity: op + two arg ids (canonicalized so lhs <= rhs). */
typedef struct {
    XiOp op;
    uint32_t lhs_id;
    uint32_t rhs_id;
} CmpKey;

/* Extract comparison key from a value, or return false if not a cmp. */
static bool extract_cmp_key(const XiValue *v, CmpKey *out) {
    if (!v || !out || v->nargs < 2 || !v->args[0] || !v->args[1])
        return false;
    if (!xi_op_is_comparison(v->op))
        return false;
    out->op = v->op;
    out->lhs_id = v->args[0]->id;
    out->rhs_id = v->args[1]->id;
    if (xi_op_is_commutative(v->op) && out->lhs_id > out->rhs_id) {
        uint32_t tmp = out->lhs_id;
        out->lhs_id = out->rhs_id;
        out->rhs_id = tmp;
    }
    return true;
}

/* Check if two cmp keys test the same relation. */
static bool cmp_keys_equal(const CmpKey *a, const CmpKey *b) {
    if (a->op != b->op)
        return false;
    return a->lhs_id == b->lhs_id && a->rhs_id == b->rhs_id;
}

static bool cmp_keys_negated(const CmpKey *a, const CmpKey *b) {
    if (a->lhs_id != b->lhs_id || a->rhs_id != b->rhs_id)
        return false;
    return xi_op_negated_comparison(a->op) == b->op;
}

/* ========== Threading Logic ========== */

/* For a given IF block `target`, try to thread incoming edges from
 * predecessors that already know the branch outcome. */
static uint32_t thread_block(XiFunc *f, XiBlock *target) {
    if (target->kind != XI_BLOCK_IF || !target->control)
        return 0;

    CmpKey target_cmp;
    if (!extract_cmp_key(target->control, &target_cmp))
        return 0;

    XiBlock *then_succ = target->succs[0];
    XiBlock *else_succ = target->succs[1];
    if (!then_succ || !else_succ)
        return 0;

    uint32_t threaded = 0;

    /* Walk predecessors in reverse to allow safe removal during iteration. */
    for (int pi = (int) target->npreds - 1; pi >= 0; pi--) {
        XiBlock *pred = target->preds[pi];
        if (!pred)
            continue;

        /* Walk up through single-succ PLAIN blocks to find the
         * governing IF block that determines which edge reaches target. */
        XiBlock *gov = pred;
        XiBlock *child = target;
        uint32_t depth = 0;
        while (gov && gov->kind == XI_BLOCK_PLAIN && gov->npreds == 1 && depth < 16) {
            child = gov;
            gov = gov->preds[0];
            depth++;
        }
        if (!gov || gov->kind != XI_BLOCK_IF || !gov->control)
            continue;
        if (gov == target)
            continue;

        CmpKey pred_cmp;
        if (!extract_cmp_key(gov->control, &pred_cmp))
            continue;

        /* Determine which side of gov's branch leads toward target. */
        bool from_then = (gov->succs[0] == child);
        bool from_else = (gov->succs[1] == child);
        if (!from_then && !from_else)
            continue;

        /* If pred tests the same condition as target:
         *   from_then: pred proved condition true  -> thread to then_succ
         *   from_else: pred proved condition false -> thread to else_succ
         * If pred tests the negated condition:
         *   from_then: target is false -> else_succ
         *   from_else: target is true  -> then_succ */
        XiBlock *dest = NULL;
        if (cmp_keys_equal(&pred_cmp, &target_cmp))
            dest = from_then ? then_succ : else_succ;
        else if (cmp_keys_negated(&pred_cmp, &target_cmp))
            dest = from_then ? else_succ : then_succ;
        else
            continue;
        if (dest == pred)
            continue;

        /* If dest already has a phi, naively appending pred would leave
         * phi.nargs out of sync with dest.npreds, and the right phi
         * argument cannot be recovered without per-arg dominance
         * analysis on the value previously incoming on the target edge.
         * Refuse this thread candidate; the rest of the IR is left
         * untouched and the next pipeline iteration may revisit. */
        if (dest->phis != NULL)
            continue;

        if (xi_cfg_redirect_edge(pred, target, dest, NULL, 0))
            threaded++;
    }

    if (threaded > 0)
        xi_cfg_mark_unreachable_if_isolated(f, target);

    return threaded;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_jump_thread(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_jump_thread: NULL func");

    if (f->nblocks <= 1)
        return xi_pass_no_change();

    uint32_t total_threaded = 0;
    bool changed = true;

    /* Iterate until fixed point (threading can enable more threading). */
    while (changed) {
        changed = false;
        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            XiBlock *blk = f->blocks[bi];
            if (!blk || blk->kind != XI_BLOCK_IF)
                continue;

            uint32_t n = thread_block(f, blk);
            if (n > 0) {
                total_threaded += n;
                changed = true;
            }
        }
    }

    XiPassChange chg = xi_pass_no_change();
    if (total_threaded > 0) {
        chg.cfg_changed = true;
        chg.n_removed = total_threaded;
    }
    return chg;
}
