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
 *   Iterates up to 3 rounds for nested diamond flattening.
 */

#include "xi_opt_ifconv.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#define IFCONV_MAX_ROUNDS    3
#define IFCONV_MAX_INS       2
#define IFCONV_MAX_PHIS      2

/* ========== Helpers ========== */

/* Check if a value is pure (safe to speculate past a branch). */
static bool ifconv_is_pure(const XiValue *v) {
    if (!v) return false;
    if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_WRITES_MEM))
        return false;
    switch (v->op) {
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_NEG:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_BNOT:
        case XI_SHL: case XI_SHR:
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE:
        case XI_NOT: case XI_ISNULL:
        case XI_CONVERT: case XI_COPY:
        case XI_BOX: case XI_UNBOX:
        case XI_CONST: case XI_SELECT:
            return true;
        default:
            return false;
    }
}

/* Check if a branch arm block is eligible: PLAIN, ≤N pure values,
 * single successor. */
static bool ifconv_ok_arm(const XiBlock *blk) {
    if (!blk) return false;
    if (blk->kind != XI_BLOCK_PLAIN) return false;
    uint32_t n = 0;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (!v) continue;
        if (v->op == XI_COPY) continue;  /* copies are free */
        if (!ifconv_is_pure(v)) return false;
        n++;
    }
    return n <= IFCONV_MAX_INS;
}

/* Check if join block has 1-2 phis and exactly 2 predecessors. */
static bool ifconv_ok_join(const XiBlock *blk) {
    if (!blk || blk->npreds != 2) return false;
    uint32_t n = 0;
    for (const XiPhi *p = blk->phis; p; p = p->next) {
        n++;
        if (n > IFCONV_MAX_PHIS) return false;
    }
    return n >= 1;
}

/* Find the phi arg for a given predecessor block. */
static XiValue *phi_arg_for_pred(const XiPhi *phi, const XiBlock *join,
                                  const XiBlock *pred) {
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
            if (!v) continue;
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

/* Append a value (already allocated) to a block's values array. */
static bool ifconv_append_value(XiBlock *blk, XiValue *v) {
    if (blk->nvalues >= blk->values_cap) {
        uint32_t new_cap = blk->values_cap ? blk->values_cap * 2 : 8;
        XiValue **tmp = (XiValue **)xr_malloc(new_cap * sizeof(XiValue *));
        if (!tmp) return false;
        if (blk->values) {
            for (uint32_t i = 0; i < blk->nvalues; i++)
                tmp[i] = blk->values[i];
        }
        blk->values = tmp;
        blk->values_cap = new_cap;
    }
    blk->values[blk->nvalues++] = v;
    return true;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_ifconv(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_ifconv: NULL func");
    if (f->nblocks < 3) return xi_pass_no_change();

    bool ever_converted = false;

    for (int round = 0; round < IFCONV_MAX_ROUNDS; round++) {
        bool converted_any = false;

        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            XiBlock *ifblk = f->blocks[bi];
            if (ifblk->kind != XI_BLOCK_IF) continue;
            if (!ifblk->control) continue;
            if (!ifblk->succs[0] || !ifblk->succs[1]) continue;
            if (ifblk->succs[0] == ifblk->succs[1]) continue;

            XiBlock *then_blk = ifblk->succs[0];
            XiBlock *else_blk = ifblk->succs[1];

            /* Both arms must be PLAIN and jump to the same join block. */
            if (then_blk->kind != XI_BLOCK_PLAIN) continue;
            if (else_blk->kind != XI_BLOCK_PLAIN) continue;
            if (!then_blk->succs[0] || !else_blk->succs[0]) continue;
            if (then_blk->succs[0] != else_blk->succs[0]) continue;

            XiBlock *join_blk = then_blk->succs[0];

            /* Validate arm blocks and join block. */
            if (!ifconv_ok_arm(then_blk)) continue;
            if (!ifconv_ok_arm(else_blk)) continue;
            if (!ifconv_ok_join(join_blk)) continue;

            /* === Conversion === */
            XiValue *cond = ifblk->control;

            /* Move then-block values to ifblk. */
            for (uint32_t i = 0; i < then_blk->nvalues; i++) {
                XiValue *v = then_blk->values[i];
                if (v) ifconv_append_value(ifblk, v);
            }

            /* Move else-block values to ifblk. */
            for (uint32_t i = 0; i < else_blk->nvalues; i++) {
                XiValue *v = else_blk->values[i];
                if (v) ifconv_append_value(ifblk, v);
            }

            /* Create XI_SELECT for each phi in join_blk. */
            for (XiPhi *phi = join_blk->phis; phi; phi = phi->next) {
                XiValue *true_val = phi_arg_for_pred(phi, join_blk, then_blk);
                XiValue *false_val = phi_arg_for_pred(phi, join_blk, else_blk);
                if (!true_val || !false_val) continue;

                XiValue *sel = xi_value_new(f, ifblk, XI_SELECT,
                                             phi->value.type, 3);
                if (!sel) continue;
                sel->args[0] = cond;
                sel->args[1] = true_val;
                sel->args[2] = false_val;

                /* Replace all uses of the phi with the select. */
                ifconv_replace_uses(f, &phi->value, sel);
            }

            /* Rewire: ifblk becomes PLAIN → join_blk. */
            ifblk->kind = XI_BLOCK_PLAIN;
            ifblk->control = NULL;
            ifblk->succs[0] = join_blk;
            ifblk->succs[1] = NULL;

            /* Clear then/else blocks. */
            then_blk->nvalues = 0;
            then_blk->kind = XI_BLOCK_UNREACHABLE;
            then_blk->succs[0] = NULL;
            else_blk->nvalues = 0;
            else_blk->kind = XI_BLOCK_UNREACHABLE;
            else_blk->succs[0] = NULL;

            /* Update join_blk predecessors: only ifblk now. */
            join_blk->preds[0] = ifblk;
            join_blk->npreds = 1;
            join_blk->phis = NULL;  /* phis replaced by selects */

            converted_any = true;
            ever_converted = true;
        }

        if (!converted_any) break;
    }

    if (!ever_converted) return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();
    chg.cfg_changed = true;
    chg.values_changed = true;
    return chg;
}
