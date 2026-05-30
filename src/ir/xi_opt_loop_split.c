/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_split.c - Loop splitting for Xi IR
 *
 * Splits loops with early-exit conditions into a pre-exit loop
 * (guaranteed no exit) and a remainder, enabling downstream passes
 * to optimize the main loop body without the exit-check overhead.
 *
 * The pattern handled is:
 *
 *   header:
 *     i = phi(start, i_next)
 *     cond = i < limit
 *     if cond → body, exit
 *   body:
 *     ...
 *     exit_cond = some_check(i)   ← e.g. bounds check
 *     if exit_cond → continue, early_exit
 *   continue:
 *     ...
 *   latch:
 *     i_next = i + step
 *     → header
 *
 * When exit_cond can be statically bounded by the IV range (e.g.
 * bounds check where array length is loop-invariant), the loop is
 * split into a "safe" range and a "checking" remainder.
 *
 * MVP scope: only handles a single break-only exit from the body
 * where the break condition is a bounds check against a loop-
 * invariant length.
 */

#include "xi_opt_loop_split.h"
#include "xi_cfg_edit.h"
#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype.h"

/* ========== Eligibility ========== */

/* Check for a simple loop shape with a single body-level exit. */
static bool loop_shape_eligible(const XiLoop *loop) {
    if (!loop || !loop->header || !loop->preheader || !loop->latch)
        return false;
    XiBlock *header = loop->header;
    if (header->kind != XI_BLOCK_IF || !header->control)
        return false;
    if (header->npreds != 2)
        return false;
    if (loop->child)
        return false;

    bool has_pre = false, has_latch = false;
    for (uint16_t p = 0; p < header->npreds; p++) {
        if (header->preds[p] == loop->preheader)
            has_pre = true;
        else if (header->preds[p] == loop->latch)
            has_latch = true;
    }
    return has_pre && has_latch;
}

/* Find a body block with an early-exit IF whose condition is a
 * bounds check against a loop-invariant value.  Returns the block
 * or NULL if none found.  Sets *out_check to the bounds-check value
 * and *out_early_exit to the early-exit target block. */
static XiBlock *find_splittable_exit(const XiLoop *loop, XiValue **out_check,
                                     XiBlock **out_early_exit) {
    *out_check = NULL;
    *out_early_exit = NULL;

    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (!blk || blk == loop->header)
            continue;
        if (blk->kind != XI_BLOCK_IF || !blk->control)
            continue;

        /* One successor must be inside the loop, the other outside. */
        XiBlock *in_succ = NULL, *out_succ = NULL;
        for (int s = 0; s < 2; s++) {
            XiBlock *succ = blk->succs[s];
            if (!succ)
                continue;
            if (xi_loop_contains_block(loop, succ))
                in_succ = succ;
            else
                out_succ = succ;
        }
        if (!in_succ || !out_succ)
            continue;

        /* Check if the control is a bounds-check-like comparison
         * involving the IV and a loop-invariant value. */
        XiValue *cond = blk->control;
        if (cond->op != XI_LT && cond->op != XI_LE && cond->op != XI_GE && cond->op != XI_GT)
            continue;
        if (cond->nargs != 2)
            continue;

        /* One argument should be related to the IV, the other
         * should be loop-invariant. */
        bool has_iv_arg = false, has_invariant_arg = false;
        for (uint16_t a = 0; a < 2; a++) {
            XiValue *arg = cond->args[a];
            if (!arg)
                continue;
            bool in_loop = arg->block && xi_loop_contains_block(loop, arg->block);
            if (in_loop)
                has_iv_arg = true;
            else
                has_invariant_arg = true;
        }

        if (has_iv_arg && has_invariant_arg) {
            *out_check = cond;
            *out_early_exit = out_succ;
            return blk;
        }
    }
    return NULL;
}

/* ========== Split Core ========== */

/* The actual splitting logic: convert the early-exit branch into
 * an unconditional jump within the main loop, and add a pre-loop
 * guard that skips the loop entirely if the exit condition would
 * trigger on the first iteration. */
static bool split_loop(XiFunc *f, XiLoop *loop, XiBlock *exit_block, XiValue *check,
                       XiBlock *early_exit) {
    XiBlock *header = loop->header;
    XiBlock *preheader = loop->preheader;

    /* Create a pre-loop guard block that checks if the early-exit
     * condition would fail on the first iteration. */
    XiBlock *guard = xi_block_new(f);
    if (!guard)
        return false;

    /* Map header phis to preheader values for the guard check. */
    uint16_t pre_idx = xi_cfg_pred_index(header, preheader);
    if (pre_idx >= header->npreds)
        return false;

    /* Clone the exit condition with preheader (first-iteration) values. */
    XiValue *guard_args[2] = {NULL, NULL};
    for (uint16_t a = 0; a < check->nargs && a < 2; a++) {
        XiValue *arg = check->args[a];
        if (!arg)
            return false;
        /* If arg is a header phi, use the preheader incoming value. */
        bool is_phi = false;
        for (XiPhi *phi = header->phis; phi; phi = phi->next) {
            if (arg == &phi->value && pre_idx < phi->value.nargs) {
                guard_args[a] = phi->value.args[pre_idx];
                is_phi = true;
                break;
            }
        }
        if (!is_phi) {
            /* Loop-invariant or defined before the loop. */
            guard_args[a] = arg;
        }
    }

    if (!guard_args[0] || !guard_args[1])
        return false;

    XiValue *guard_cond = xi_binary(f, guard, check->op, check->type, guard_args[0], guard_args[1]);
    if (!guard_cond)
        return false;

    /* Determine which successor of exit_block is the early exit
     * and which continues in the loop. */
    bool exit_on_true = (exit_block->succs[1] == early_exit);
    XiBlock *continue_succ = exit_on_true ? exit_block->succs[0] : exit_block->succs[1];

    /* In the main loop body: convert the early-exit IF into an
     * unconditional jump to the continue successor. */
    xi_cfg_remove_pred(early_exit, exit_block);
    exit_block->kind = XI_BLOCK_PLAIN;
    exit_block->control = NULL;
    exit_block->succs[0] = continue_succ;
    exit_block->succs[1] = NULL;

    /* Wire the guard: if guard condition would cause early exit,
     * jump directly to the early exit; otherwise enter the loop. */
    if (exit_on_true) {
        /* Original: if (!cond) exit; in the guard: if (cond) exit. */
        xi_block_set_if(guard, guard_cond, early_exit, header);
    } else {
        xi_block_set_if(guard, guard_cond, header, early_exit);
    }

    /* Redirect preheader → guard → header. */
    if (!xi_cfg_redirect_edge(preheader, header, guard, NULL, 0))
        return false;

    guard->sealed = true;
    return true;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_loop_split(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_loop_split: NULL func");
    if (f->nblocks < 4)
        return xi_pass_no_change();

    XiLoopInfo *loops = xi_ensure_loops(f);
    if (!loops || loops->nloop == 0)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();

    for (uint32_t li = 0; li < loops->nloop; li++) {
        XiLoop *loop = loops->all_loops[li];
        if (!loop_shape_eligible(loop))
            continue;

        XiValue *check = NULL;
        XiBlock *early_exit = NULL;
        XiBlock *exit_block = find_splittable_exit(loop, &check, &early_exit);
        if (!exit_block)
            continue;

        if (split_loop(f, loop, exit_block, check, early_exit)) {
            chg.cfg_changed = true;
            chg.values_changed = true;
            chg.n_added += 2;
            return chg;
        }
    }

    return chg;
}
