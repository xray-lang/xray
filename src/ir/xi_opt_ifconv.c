/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_ifconv.c - If-Conversion for Xi IR
 *
 * ALGORITHM:
 *   Detect diamond CFG patterns:
 *     ifblk(IF, cond) → then_blk, else_blk
 *     then_blk(PLAIN) → join_blk   [≤2 pure values]
 *     else_blk(PLAIN) → join_blk   [≤2 pure values]
 *     join_blk: 1-2 phi nodes, exactly 2 predecessors
 *
 *   Convert to:
 *     ifblk(PLAIN) → join_blk
 *       [then values] + [else values] + XI_SELECT per phi
 *     join_blk: phis removed, predecessors = [ifblk only]
 *
 *   Single-pass: nested diamonds are handled by jump_threading +
 *   block_simplify, so IfConv only needs one scan.
 */

#include "xi_opt_ifconv.h"
#include "../base/xchecks.h"
#include "xi_effect.h"
#include "xi_own.h"

#define IFCONV_MAX_INS 2
#define IFCONV_MAX_PHIS 2

/* ========== Helpers ========== */

/* Check if a value can be speculated past a branch. */
static bool ifconv_can_speculate(const XiValue *v) {
    if (!v)
        return false;
    if (v->flags &
        (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND | XI_FLAG_WRITES_MEM))
        return false;
    return xi_op_can_speculate(v->op);
}

/* Check if a branch arm block is eligible: PLAIN, ≤N speculatable values,
 * single successor. */
static bool ifconv_ok_arm(const XiBlock *blk) {
    if (!blk)
        return false;
    if (blk->kind != XI_BLOCK_PLAIN)
        return false;
    uint32_t n = 0;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (!v)
            continue;
        if (v->op == XI_COPY)
            continue; /* copies are free */
        if (!ifconv_can_speculate(v))
            return false;
        n++;
    }
    return n <= IFCONV_MAX_INS;
}

/* Check if join block has 1-2 phis and exactly 2 predecessors. */
static bool ifconv_ok_join(const XiBlock *blk) {
    if (!blk || blk->npreds != 2)
        return false;
    uint32_t n = 0;
    for (const XiPhi *p = blk->phis; p; p = p->next) {
        /* If-conversion runs after ARC. Replacing a path-sensitive owning PHI
         * with SELECT would require rebuilding transfer/disposition evidence
         * for the two arms, which this local CFG pass cannot prove. */
        if (xi_own_type_is_rc(p->value.type))
            return false;
        n++;
        if (n > IFCONV_MAX_PHIS)
            return false;
    }
    return n >= 1;
}

/* Find the phi arg for a given predecessor block. */
static XiValue *phi_arg_for_pred(const XiPhi *phi, const XiBlock *join, const XiBlock *pred) {
    for (uint16_t i = 0; i < join->npreds && i < phi->value.nargs; i++) {
        if (join->preds[i] == pred)
            return phi->value.args[i];
    }
    return NULL;
}

/* Replace all uses of old_val with new_val in the function. */
static void ifconv_replace_uses(XiFunc *f, XiValue *old_val, XiValue *new_val) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] == old_val)
                    v->args[a] = new_val;
            }
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == old_val)
                    phi->value.args[a] = new_val;
            }
        }
        if (blk->control == old_val)
            blk->control = new_val;
    }
}

/* Append a value (already allocated) to a block's values array.
 *
 * A value carrying a var_id is a write to that source variable's storage, not
 * just an SSA definition: the VM emitter coalesces every version of a variable
 * into one register.  While the two arms sat on exclusive paths, at most one of
 * those writes ran, so sharing the location was sound.  Flattening the diamond
 * puts both writes in one straight line, and the second one overwrites the
 * first before either is read.  A merge over two such writes then reads one
 * location twice and answers with whichever arm was appended last, whatever the
 * condition said.
 *
 * The hoisted values stop being the variable's reaching definition here.  The
 * select that replaces the phi becomes it, and it takes the phi's var_id below,
 * so the variable keeps exactly one write and one storage identity. */
static bool ifconv_append_value(XiBlock *blk, XiValue *v) {
    if (!xi_block_ensure_value_capacity(blk, blk->nvalues + 1))
        return false;
    v->block = blk;
    v->var_id = XI_NO_VAR_ID;
    blk->values[blk->nvalues++] = v;
    return true;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_ifconv(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_ifconv: NULL func");
    if (f->nblocks < 3)
        return xi_pass_no_change();

    bool converted_any = false;

    {
        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            XiBlock *ifblk = f->blocks[bi];
            if (ifblk->kind != XI_BLOCK_IF)
                continue;
            if (!ifblk->control)
                continue;
            if (!ifblk->succs[0] || !ifblk->succs[1])
                continue;
            if (ifblk->succs[0] == ifblk->succs[1])
                continue;

            XiBlock *then_blk = ifblk->succs[0];
            XiBlock *else_blk = ifblk->succs[1];

            /* Both arms must be PLAIN and jump to the same join block. */
            if (then_blk->kind != XI_BLOCK_PLAIN)
                continue;
            if (else_blk->kind != XI_BLOCK_PLAIN)
                continue;
            if (!then_blk->succs[0] || !else_blk->succs[0])
                continue;
            if (then_blk->succs[0] != else_blk->succs[0])
                continue;

            XiBlock *join_blk = then_blk->succs[0];

            /* Validate arm blocks and join block. */
            if (!ifconv_ok_arm(then_blk))
                continue;
            if (!ifconv_ok_arm(else_blk))
                continue;
            if (!ifconv_ok_join(join_blk))
                continue;

            /* === Conversion === */
            XiValue *cond = ifblk->control;
            /* A reference-capable condition may have edge-specific releases.
             * Flattening the branch would invalidate that lifetime frontier. */
            if (xi_own_type_is_rc(cond ? cond->type : NULL))
                continue;

            /* Move then-block values to ifblk. */
            for (uint32_t i = 0; i < then_blk->nvalues; i++) {
                XiValue *v = then_blk->values[i];
                if (v)
                    ifconv_append_value(ifblk, v);
            }

            /* Move else-block values to ifblk. */
            for (uint32_t i = 0; i < else_blk->nvalues; i++) {
                XiValue *v = else_blk->values[i];
                if (v)
                    ifconv_append_value(ifblk, v);
            }

            /* Create XI_SELECT for each phi in join_blk. */
            for (XiPhi *phi = join_blk->phis; phi; phi = phi->next) {
                XiValue *true_val = phi_arg_for_pred(phi, join_blk, then_blk);
                XiValue *false_val = phi_arg_for_pred(phi, join_blk, else_blk);
                if (!true_val || !false_val)
                    continue;

                XiValue *sel = xi_value_new(f, ifblk, XI_SELECT, phi->value.type, 3);
                if (!sel)
                    continue;
                sel->args[0] = cond;
                sel->args[1] = true_val;
                sel->args[2] = false_val;
                /* The select inherits the merge's source variable: it is now
                 * the single write that reaches every later read of it. */
                sel->var_id = phi->value.var_id;

                /* Replace all uses of the phi with the select. */
                ifconv_replace_uses(f, &phi->value, sel);
            }

            /* Rewire: ifblk becomes PLAIN → join_blk. */
            ifblk->kind = XI_BLOCK_PLAIN;
            ifblk->control = NULL;
            ifblk->line = 0;
            ifblk->succs[0] = join_blk;
            ifblk->succs[1] = NULL;

            /* Clear then/else blocks. */
            then_blk->nvalues = 0;
            then_blk->kind = XI_BLOCK_UNREACHABLE;
            then_blk->line = 0;
            then_blk->succs[0] = NULL;
            else_blk->nvalues = 0;
            else_blk->kind = XI_BLOCK_UNREACHABLE;
            else_blk->line = 0;
            else_blk->succs[0] = NULL;

            /* Update join_blk predecessors: only ifblk now. */
            join_blk->preds[0] = ifblk;
            join_blk->npreds = 1;
            join_blk->phis = NULL; /* phis replaced by selects */

            converted_any = true;
        }
    }

    if (!converted_any)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();
    chg.cfg_changed = true;
    chg.values_changed = true;
    return chg;
}
