/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_inv_branch.c - Loop-invariant branch hoisting for Xi IR
 *
 * Hoists branches whose condition is loop-invariant out of the loop.
 * The loop is unswitched: two copies of the loop are created, one
 * for each branch outcome, with the branch moved to the preheader.
 *
 * The pattern handled is:
 *
 *   header:
 *     ...
 *   body:
 *     if (invariant_cond):   ← cond defined outside loop
 *         then_path(i)
 *     else:
 *         else_path(i)
 *
 * After hoisting:
 *
 *   preheader:
 *     if (invariant_cond):
 *         loop_copy_then: for i: then_path(i)
 *     else:
 *         loop_copy_else: for i: else_path(i)
 *
 * MVP scope: only handles the simple case where the invariant branch
 * is in a body block (not the header) and both sides eventually
 * merge to a single block within the loop.  To control code size,
 * we do NOT duplicate the full loop; instead we convert the invariant
 * branch into an unconditional jump to the dominant side, with a
 * pre-loop check that selects which version to enter.
 *
 * Simplified approach: when both sides of the branch are small and
 * converge within the loop, we simply hoist the condition evaluation
 * to before the loop and convert the in-loop branch to use the
 * hoisted value — this alone enables the branch predictor to see a
 * constant pattern and eliminates re-evaluation of the condition.
 */

#include "xi_opt_loop_inv_branch.h"
#include "xi_cfg_edit.h"
#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"

/* ========== Analysis ========== */

/* Check if a value is loop-invariant: defined outside the loop. */
static bool is_loop_invariant(const XiLoop *loop, const XiValue *v) {
    if (!v || !v->block)
        return true;
    return !xi_loop_contains_block(loop, v->block);
}

/* Check if all operands of a value are loop-invariant. */
static bool all_args_loop_invariant(const XiLoop *loop, const XiValue *v) {
    if (!v)
        return false;
    for (uint16_t a = 0; a < v->nargs; a++) {
        if (!is_loop_invariant(loop, v->args[a]))
            return false;
    }
    return true;
}

/* Find a body block with a loop-invariant branch condition.
 * Returns the block, or NULL if none found. */
static XiBlock *find_invariant_branch(const XiLoop *loop) {
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (!blk || blk == loop->header)
            continue;
        if (blk->kind != XI_BLOCK_IF || !blk->control)
            continue;
        if (!blk->succs[0] || !blk->succs[1] || !xi_loop_contains_block(loop, blk->succs[0]) ||
            !xi_loop_contains_block(loop, blk->succs[1]))
            continue;

        XiValue *cond = blk->control;

        /* Phi results are CFG edge selections, not ordinary pure expressions.
         * Even when all incoming values are loop-invariant, the selected
         * incoming edge may still be loop-variant, so cloning a PHI into the
         * preheader would manufacture an invalid value-list PHI and change
         * branch semantics. */
        if (cond->op == XI_PHI)
            continue;

        /* The condition itself must be defined outside the loop,
         * OR all its operands must be loop-invariant (and the op
         * itself is pure, so it can be hoisted). */
        if (is_loop_invariant(loop, cond))
            return blk;

        /* Pure computation with loop-invariant operands. */
        if (!(cond->flags &
              (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM)) &&
            all_args_loop_invariant(loop, cond))
            return blk;
    }
    return NULL;
}

/* ========== Hoisting ========== */

/* Clone a loop-invariant condition to the preheader and replace the
 * in-loop condition with the hoisted version.  This is the simplest
 * form of unswitching: no loop duplication, just ensures the
 * condition is evaluated once before the loop. */
static bool hoist_invariant_condition(XiFunc *f, XiLoop *loop, XiBlock *branch_blk) {
    XiValue *cond = branch_blk->control;
    if (!cond)
        return false;

    XiBlock *preheader = loop->preheader;
    if (!preheader)
        return false;

    if (is_loop_invariant(loop, cond)) {
        /* Condition already defined outside — nothing to clone.
         * The branch predictor benefit comes from the condition being
         * constant across iterations; no IR change needed here.
         * Return false to indicate no transformation applied. */
        return false;
    }

    if (cond->op == XI_PHI)
        return false;

    /* Clone the condition computation to the preheader. */
    XiValue *hoisted = xi_value_new(f, preheader, cond->op, cond->type, cond->nargs);
    if (!hoisted)
        return false;
    hoisted->flags = cond->flags;
    hoisted->var_id = cond->var_id;
    hoisted->rep = cond->rep;
    hoisted->transfer_mode = cond->transfer_mode;
    hoisted->aux_kind = cond->aux_kind;
    hoisted->escape = cond->escape;
    hoisted->mem_group = cond->mem_group;
    hoisted->aux_int = cond->aux_int;
    hoisted->aux = cond->aux;
    hoisted->line = cond->line;
    for (uint16_t a = 0; a < cond->nargs; a++) {
        hoisted->args[a] = cond->args[a];
    }

    /* Replace the in-loop condition with the hoisted one. */
    branch_blk->control = hoisted;

    return true;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_loop_inv_branch(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_loop_inv_branch: NULL func");
    if (f->nblocks < 4)
        return xi_pass_no_change();

    XiLoopInfo *loops = xi_ensure_loops(f);
    if (!loops || loops->nloop == 0)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();

    for (uint32_t li = 0; li < loops->nloop; li++) {
        XiLoop *loop = loops->all_loops[li];
        if (!loop || !loop->header || !loop->preheader)
            continue;

        XiBlock *branch_blk = find_invariant_branch(loop);
        if (!branch_blk)
            continue;

        if (hoist_invariant_condition(f, loop, branch_blk)) {
            chg.values_changed = true;
            chg.n_added += 1;
            /* Continue to process more loops in this function. */
        }
    }

    return chg;
}
