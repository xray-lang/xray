/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_gvn.c - Global Value Numbering for Xi IR
 *
 * ALGORITHM:
 *   Open-addressing hash table keyed by (op, arg0_ptr, arg1_ptr).
 *   For each pure value, normalize commutative operands, hash, and
 *   probe the table.  On a hit where the earlier definition dominates
 *   the current block, rewrite the current value to XI_COPY of the
 *   earlier result (subsequent copy_prop + DCE will clean up).
 *
 *   Only values with exactly 2 args are eligible for GVN (binary ops,
 *   comparisons).  Unary ops and 0-arg ops are handled by constfold
 *   and copy_prop which are cheaper.
 */

#include "xi_opt_gvn.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Pure / Commutative Classification ========== */

static bool xi_op_is_pure(uint16_t op) {
    switch (op) {
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_NEG:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_BNOT:
        case XI_SHL: case XI_SHR:
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE:
        case XI_NOT:
        case XI_ISNULL:
        case XI_CONVERT:
        case XI_NARROW_I8: case XI_NARROW_U8:
        case XI_NARROW_I16: case XI_NARROW_U16:
        case XI_NARROW_I32: case XI_NARROW_U32:
        case XI_NARROW_F32:
        case XI_WIDEN_I8: case XI_WIDEN_U8:
        case XI_WIDEN_I16: case XI_WIDEN_U16:
        case XI_WIDEN_I32: case XI_WIDEN_U32:
        case XI_WIDEN_F32:
            return true;
        default:
            return false;
    }
}

static bool xi_op_is_commutative(uint16_t op) {
    switch (op) {
        case XI_ADD: case XI_MUL:
        case XI_BAND: case XI_BOR: case XI_BXOR:
        case XI_EQ: case XI_NE:
            return true;
        default:
            return false;
    }
}

/* ========== Hash Table ========== */

#define GVN_MIN_TABLE 64

typedef struct {
    uint32_t key;           /* hash; 0 = empty slot */
    uint16_t op;
    XiValue *arg0, *arg1;
    XiValue *result;        /* first occurrence */
    uint32_t def_blk_id;   /* block id of first occurrence */
} GvnEntry;

static uint32_t gvn_hash(uint16_t op, const XiValue *a0, const XiValue *a1) {
    uint32_t h = (uint32_t)op * 2654435761u;
    h ^= (uint32_t)(uintptr_t)a0 * 2246822519u;
    h ^= (uint32_t)(uintptr_t)a1 * 3266489917u;
    /* Ensure non-zero so 0 remains the empty-slot sentinel */
    return h ? h : 1;
}

/* Normalize commutative ops: ensure arg0 <= arg1 by pointer value */
static void gvn_normalize(XiValue *v) {
    if (v->nargs == 2 && xi_op_is_commutative(v->op)) {
        if ((uintptr_t)v->args[0] > (uintptr_t)v->args[1]) {
            XiValue *tmp = v->args[0];
            v->args[0] = v->args[1];
            v->args[1] = tmp;
        }
    }
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_gvn(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_gvn: NULL func");
    if (f->nblocks < 2) return xi_pass_no_change();

    /* Ensure dominator tree is available */
    xi_compute_rpo(f);
    xi_compute_dominators(f);

    /* Size table to ~2x total values for ~50% load factor */
    uint32_t total_values = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++)
        total_values += f->blocks[bi]->nvalues;

    uint32_t tsize = GVN_MIN_TABLE;
    while (tsize < total_values * 2)
        tsize <<= 1;
    uint32_t tmask = tsize - 1;

    GvnEntry *table = (GvnEntry *)xr_calloc(tsize, sizeof(GvnEntry));
    if (!table) return xi_pass_no_change();

    uint32_t n_replaced = 0;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v) continue;

            /* Only pure binary ops are eligible */
            if (v->nargs != 2) continue;
            if (!xi_op_is_pure(v->op)) continue;
            if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW)) continue;
            if (!v->args[0] || !v->args[1]) continue;

            gvn_normalize(v);

            uint32_t h = gvn_hash(v->op, v->args[0], v->args[1]);
            uint32_t slot = h & tmask;

            for (uint32_t probe = 0; probe < tsize; probe++) {
                uint32_t idx = (slot + probe) & tmask;
                GvnEntry *e = &table[idx];

                if (e->key == 0) {
                    /* Empty: insert this value */
                    e->key = h;
                    e->op = v->op;
                    e->arg0 = v->args[0];
                    e->arg1 = v->args[1];
                    e->result = v;
                    e->def_blk_id = bi;
                    break;
                }

                if (e->key == h && e->op == v->op &&
                    e->arg0 == v->args[0] && e->arg1 == v->args[1]) {
                    /* Match: replace if earlier def dominates current block */
                    XiBlock *def_blk = f->blocks[e->def_blk_id];
                    XR_DCHECK(def_blk != NULL, "GVN: def block is NULL");
                    if (xi_dominates(def_blk, blk)) {
                        v->op = XI_COPY;
                        v->args[0] = e->result;
                        v->nargs = 1;
                        n_replaced++;
                    }
                    break;
                }
            }
        }
    }

    xr_free(table);

    if (n_replaced == 0) return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();
    chg.values_changed = true;
    chg.n_removed = n_replaced;
    return chg;
}
