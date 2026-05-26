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
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Condition Matching ========== */

/* A comparison identity: op + two arg ids (canonicalized so lhs <= rhs). */
typedef struct {
    XiOp op;
    uint32_t lhs_id;
    uint32_t rhs_id;
} CmpKey;

/* Extract comparison key from a value, or return false if not a cmp. */
static bool extract_cmp_key(const XiValue *v, CmpKey *out) {
    if (!v || v->nargs < 2)
        return false;
    switch (v->op) {
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
            break;
        default:
            return false;
    }
    out->op = v->op;
    out->lhs_id = v->args[0]->id;
    out->rhs_id = v->args[1]->id;
    return true;
}

/* Check if two cmp keys test the same relation. */
static bool cmp_keys_equal(const CmpKey *a, const CmpKey *b) {
    if (a->op != b->op)
        return false;
    return a->lhs_id == b->lhs_id && a->rhs_id == b->rhs_id;
}

/* Check if two cmp keys test the same operands with negated relation.
 * e.g. LT(x,y) vs GE(x,y), EQ(x,y) vs NE(x,y). */
static XiOp negate_cmp(XiOp op) {
    switch (op) {
        case XI_EQ:
            return XI_NE;
        case XI_NE:
            return XI_EQ;
        case XI_LT:
            return XI_GE;
        case XI_LE:
            return XI_GT;
        case XI_GT:
            return XI_LE;
        case XI_GE:
            return XI_LT;
        default:
            return XI_OP_COUNT; /* sentinel: not a cmp */
    }
}

static bool cmp_keys_negated(const CmpKey *a, const CmpKey *b) {
    if (a->lhs_id != b->lhs_id || a->rhs_id != b->rhs_id)
        return false;
    return negate_cmp(a->op) == b->op;
}

/* ========== Pred Redirect ========== */

/* Replace pred's successor from old_target to new_target, and fix
 * pred arrays.  Returns true if redirect happened. */
static bool redirect_edge(XiBlock *pred, XiBlock *old_target, XiBlock *new_target) {
    XR_DCHECK(pred != NULL && old_target != NULL && new_target != NULL, "redirect_edge: NULL arg");
    if (old_target == new_target)
        return false;

    /* Update pred's successor pointer. */
    bool found = false;
    for (int i = 0; i < 2; i++) {
        if (pred->succs[i] == old_target) {
            pred->succs[i] = new_target;
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    /* Remove pred from old_target's pred list. */
    for (uint16_t i = 0; i < old_target->npreds; i++) {
        if (old_target->preds[i] == pred) {
            for (uint16_t j = i; j + 1 < old_target->npreds; j++)
                old_target->preds[j] = old_target->preds[j + 1];
            old_target->npreds--;

            /* Remove corresponding phi args. */
            for (XiPhi *phi = old_target->phis; phi; phi = phi->next) {
                if (i < phi->value.nargs) {
                    for (uint16_t j = i; j + 1 < phi->value.nargs; j++)
                        phi->value.args[j] = phi->value.args[j + 1];
                    phi->value.nargs--;
                }
            }
            break;
        }
    }

    /* Add pred to new_target's pred list. */
    xi_block_add_pred(new_target, pred);

    return true;
}

/* ========== Threading Logic ========== */

/* For a given IF block `target`, try to thread incoming edges from
 * predecessors that already know the branch outcome. */
static uint32_t thread_block(XiBlock *target) {
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
         *   from_else: pred proved condition false  -> thread to else_succ */
        if (cmp_keys_equal(&pred_cmp, &target_cmp)) {
            XiBlock *dest = from_then ? then_succ : else_succ;
            if (redirect_edge(pred, target, dest))
                threaded++;
        }
        /* If pred tests the negation of target's condition:
         *   from_then: pred proved negation true -> target is false -> else
         *   from_else: pred proved negation false -> target is true -> then */
        else if (cmp_keys_negated(&pred_cmp, &target_cmp)) {
            XiBlock *dest = from_then ? else_succ : then_succ;
            if (redirect_edge(pred, target, dest))
                threaded++;
        }
    }

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

            uint32_t n = thread_block(blk);
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
