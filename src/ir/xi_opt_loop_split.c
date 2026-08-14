/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_split.c - Loop splitting for Xi IR
 *
 * Removes a body-level early exit that the loop's own governing condition
 * already rules out.  The pattern handled is:
 *
 *   header:
 *     i = phi(start, i_next)
 *     hcond = i < limit
 *     if hcond → body, exit
 *   body:
 *     xcond = i < length        ← e.g. bounds check
 *     if xcond → continue, early_exit
 *   continue:
 *     ...
 *   latch:
 *     i_next = i + step
 *     → header
 *
 * The transform is admitted only when the header condition proves the body
 * condition: every path that reaches the body block took the header's
 * in-loop edge for the current value of the tested operand, so a constraint
 * the header established on that operand also holds at the body check.  When
 * the header constraint implies "the early exit is not taken", the exit edge
 * is dead and is deleted; the branch becomes an unconditional jump into the
 * loop.  Nothing is cloned and no pre-loop guard is created.
 *
 * WHY NO GUARD:
 *   A pre-loop guard that evaluates the body condition against the first
 *   iteration's operand values proves nothing about later iterations — the
 *   whole point of a loop-carried operand is that it changes.  Deleting the
 *   exit edge on the strength of such a guard turns `while (i < 100) { if (i
 *   >= 5) break; ... }` into a hundred-iteration loop.  It is equally wrong
 *   in the other direction: routing straight from the guard to the exit
 *   target skips iterations the program would have run, and the guard's edge
 *   into the exit bypasses the header, so header phis that the exit target
 *   consumes stop being dominated by their definition.  Proof obligations
 *   over a loop-carried operand have to hold for the whole operand range,
 *   which is what the header-implies-body test below establishes.
 */

#include "xi_opt_loop_split.h"
#include "xi_cfg_edit.h"
#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype.h"

/* ========== Constraint Model ========== */

/* One relational fact about a single SSA operand, normalised to
 *
 *   operand <= limit      (upper == true)
 *   operand >= limit      (upper == false)
 *
 * The limit is either an integer constant — already carrying the ±1 that
 * turns a strict comparison into a closed one — or an SSA value that both
 * facts must name literally, with the ±1 kept as a separate adjustment
 * because the value itself is unknown at compile time. */
typedef struct XiSplitFact {
    bool upper;
    bool limit_is_const;
    int64_t limit_const;
    const XiValue *limit_value;
    int limit_adjust;
} XiSplitFact;

static bool op_is_relational(XiOp op) {
    return op == XI_LT || op == XI_LE || op == XI_GT || op == XI_GE;
}

/* Mirror a comparison so the tested operand reads as the left-hand side. */
static XiOp op_swap_operands(XiOp op) {
    switch (op) {
        case XI_LT: return XI_GT;
        case XI_LE: return XI_GE;
        case XI_GT: return XI_LT;
        case XI_GE: return XI_LE;
        default: return op;
    }
}

/* Negate a comparison over a totally ordered domain. */
static XiOp op_negate(XiOp op) {
    switch (op) {
        case XI_LT: return XI_GE;
        case XI_LE: return XI_GT;
        case XI_GT: return XI_LE;
        case XI_GE: return XI_LT;
        default: return op;
    }
}

/* Integer operands only, and signed ones at that: the facts below are
 * compared as int64, which reads an unsigned limit above INT64_MAX as a
 * negative number.  Floats are excluded outright — NaN breaks the total
 * order the implication test assumes. */
static bool type_admits_signed_order(const XrType *t) {
    if (!t || t->kind != XR_KIND_INT || t->is_nullable)
        return false;
    return !xr_type_is_exact_unsigned_integer(t);
}

/* Build the fact `operand REL limit` states about `operand`.
 * `operand_is_lhs` says which side of the comparison the operand sits on,
 * and `negate` asks for the fact carried by the branch's false edge. */
static bool split_fact_build(XiOp op, bool operand_is_lhs, bool negate, const XiValue *limit,
                             XiSplitFact *out) {
    if (!op_is_relational(op) || !limit || !out)
        return false;
    if (!type_admits_signed_order(limit->type))
        return false;

    XiOp rel = operand_is_lhs ? op : op_swap_operands(op);
    if (negate)
        rel = op_negate(rel);

    int adjust;
    switch (rel) {
        case XI_LT: out->upper = true;  adjust = -1; break;
        case XI_LE: out->upper = true;  adjust = 0;  break;
        case XI_GT: out->upper = false; adjust = 1;  break;
        case XI_GE: out->upper = false; adjust = 0;  break;
        default: return false;
    }

    if (limit->op == XI_CONST) {
        int64_t base = limit->aux_int;
        /* Reject the range ends rather than wrap the closed form. */
        if (adjust < 0 && base == INT64_MIN)
            return false;
        if (adjust > 0 && base == INT64_MAX)
            return false;
        out->limit_is_const = true;
        out->limit_const = base + adjust;
        out->limit_value = NULL;
        out->limit_adjust = 0;
    } else {
        out->limit_is_const = false;
        out->limit_const = 0;
        out->limit_value = limit;
        out->limit_adjust = adjust;
    }
    return true;
}

/* Does `premise` entail `conclusion` for every value of the operand?
 * An upper bound never entails a lower bound over an unbounded domain, so
 * the two facts must point the same way and the premise must be the tighter
 * of the pair. */
static bool split_fact_entails(const XiSplitFact *premise, const XiSplitFact *conclusion) {
    if (!premise || !conclusion)
        return false;
    if (premise->upper != conclusion->upper)
        return false;

    if (premise->limit_is_const != conclusion->limit_is_const)
        return false;

    if (premise->limit_is_const) {
        return premise->upper ? premise->limit_const <= conclusion->limit_const
                              : premise->limit_const >= conclusion->limit_const;
    }

    /* Same unknown limit: the adjustments order the two closed forms. */
    if (!premise->limit_value || premise->limit_value != conclusion->limit_value)
        return false;
    return premise->upper ? premise->limit_adjust <= conclusion->limit_adjust
                          : premise->limit_adjust >= conclusion->limit_adjust;
}

/* ========== Eligibility ========== */

/* Check for a simple loop shape with a single body-level exit. */
/* Which way does the header branch have to go for control to be inside the
 * loop?  Reports false when both successors sit on the same side, which
 * leaves no fact to carry into the body. */
static bool header_in_loop_polarity(const XiLoop *loop, bool *out_wants_true) {
    const XiBlock *header = loop->header;
    if (!header->succs[0] || !header->succs[1])
        return false;
    bool then_inside = xi_loop_contains_block(loop, header->succs[0]);
    bool else_inside = xi_loop_contains_block(loop, header->succs[1]);
    if (then_inside == else_inside)
        return false;
    *out_wants_true = then_inside;
    return true;
}

/* Every path into a natural loop's body leaves the header on its in-loop
 * edge: a body block is dominated by the header, and the header's other
 * successor is outside the loop, so it cannot reach the body again without
 * passing the header once more.  The header's condition therefore holds —
 * with the in-loop polarity — for the operand value the body block sees.
 *
 * Returns true when that fact rules the early exit out for every iteration. */
static bool early_exit_is_dead(const XiLoop *loop, const XiValue *check, bool exit_on_true) {
    const XiValue *hcond = loop->header->control;
    if (!hcond || hcond->nargs != 2 || !op_is_relational(hcond->op))
        return false;
    if (!check || check->nargs != 2 || !op_is_relational(check->op))
        return false;

    bool header_wants_true = false;
    if (!header_in_loop_polarity(loop, &header_wants_true))
        return false;

    /* The two comparisons must constrain one shared operand.  Any operand
     * of the header's condition dominates the header, so it holds the same
     * value at the body check within an iteration. */
    for (uint16_t h = 0; h < 2; h++) {
        const XiValue *operand = hcond->args[h];
        if (!operand || !type_admits_signed_order(operand->type))
            continue;
        for (uint16_t c = 0; c < 2; c++) {
            if (check->args[c] != operand)
                continue;

            XiSplitFact premise, conclusion;
            if (!split_fact_build(hcond->op, h == 0, !header_wants_true, hcond->args[1 - h],
                                  &premise))
                continue;
            /* The exit is dead exactly when the check settles opposite to
             * the polarity that takes it. */
            if (!split_fact_build(check->op, c == 0, exit_on_true, check->args[1 - c],
                                  &conclusion))
                continue;
            if (split_fact_entails(&premise, &conclusion))
                return true;
        }
    }
    return false;
}

/* Find a body block whose conditional leaves the loop on one edge and whose
 * exit edge the header condition already rules out.  Returns the block, or
 * NULL when no such exit exists.  Sets *out_early_exit to the exit target
 * and *out_exit_on_true to the branch polarity that would take it. */
static XiBlock *find_dead_exit(const XiLoop *loop, XiBlock **out_early_exit,
                               bool *out_exit_on_true) {
    *out_early_exit = NULL;
    *out_exit_on_true = false;

    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (!blk || blk == loop->header || blk == loop->preheader || blk == loop->latch)
            continue;
        if (blk->kind != XI_BLOCK_IF || !blk->control)
            continue;

        /* One successor must be inside the loop, the other outside. */
        if (!blk->succs[0] || !blk->succs[1])
            continue;
        bool then_inside = xi_loop_contains_block(loop, blk->succs[0]);
        bool else_inside = xi_loop_contains_block(loop, blk->succs[1]);
        if (then_inside == else_inside)
            continue;

        bool exit_on_true = !then_inside;
        if (!early_exit_is_dead(loop, blk->control, exit_on_true))
            continue;

        *out_early_exit = exit_on_true ? blk->succs[0] : blk->succs[1];
        *out_exit_on_true = exit_on_true;
        return blk;
    }
    return NULL;
}

/* ========== Rewrite ========== */

/* Drop the proven-dead exit edge, leaving an unconditional jump onward
 * inside the loop.  Removing an edge only ever strengthens dominance, so
 * no use can lose its definition here. */
static void drop_dead_exit(XiFunc *f, XiBlock *exit_block, XiBlock *early_exit,
                           bool exit_on_true) {
    XiBlock *continue_succ = exit_on_true ? exit_block->succs[1] : exit_block->succs[0];
    XR_DCHECK(continue_succ != NULL, "loop_split: NULL in-loop successor");
    XR_DCHECK(continue_succ != early_exit, "loop_split: exit target is also the in-loop successor");

    xi_cfg_remove_pred(early_exit, exit_block);
    exit_block->kind = XI_BLOCK_PLAIN;
    exit_block->control = NULL;
    exit_block->line = 0;
    exit_block->succs[0] = continue_succ;
    exit_block->succs[1] = NULL;

    /* The exit target may have had no other way in. */
    xi_cfg_mark_unreachable_if_isolated(f, early_exit);
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_loop_split(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_loop_split: NULL func");
    if (f->nblocks < 4)
        return xi_pass_no_change();

    XiLoopInfo *loops = xi_ensure_loops(f);
    if (!loops || loops->nloop == 0)
        return xi_pass_no_change();

    for (uint32_t li = 0; li < loops->nloop; li++) {
        XiLoop *loop = loops->all_loops[li];
        if (!xi_loop_shape_is_simple(loop))
            continue;

        XiBlock *early_exit = NULL;
        bool exit_on_true = false;
        XiBlock *exit_block = find_dead_exit(loop, &early_exit, &exit_on_true);
        if (!exit_block)
            continue;

        drop_dead_exit(f, exit_block, early_exit, exit_on_true);

        /* The edge is gone, so the cached loop forest describes a graph
         * that no longer exists.  Hand the change back and let the driver
         * re-derive it before the next visit. */
        XiPassChange chg = xi_pass_no_change();
        chg.cfg_changed = true;
        chg.values_changed = true;
        return chg;
    }

    return xi_pass_no_change();
}
