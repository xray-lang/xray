/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_gvn.c - Global Value Numbering with Partial Redundancy Elimination
 *
 * ALGORITHM (dominator-based GVN-PRE):
 *
 *   1. VALUE NUMBERING:
 *      Hash-consing table keyed by (op, vn(arg0), vn(arg1), aux_int).
 *      Commutative ops are normalized.  Each unique expression gets a
 *      dense value number (VN).  Two values with the same VN are
 *      semantically equivalent.
 *
 *   2. FULL REDUNDANCY ELIMINATION:
 *      Walk blocks in dominator-tree RPO.  If a value's VN already
 *      has a dominating definition, replace with XI_COPY.
 *
 *   3. TBAA-AWARE LOAD ELIMINATION:
 *      Memory loads participate when XI_INV_TBAA_ANNOTATED is set.
 *      A load is redundant if an earlier load with the same VN exists
 *      and no intervening store may alias it (checked via mem_group).
 *
 *   Partial-redundancy insertion is deferred to a future upgrade —
 *   full redundancy elimination already covers the majority of cases
 *   in typical xray programs.
 *
 * SCOPE (vs legacy):
 *   - Unary + binary + conversion ops (was: binary only)
 *   - Memory loads with TBAA disjointness (was: none)
 *   - VN-based matching (was: pointer identity on args)
 */

#include "xi_opt_gvn.h"
#include "xi_analysis.h"
#include "xi_tbaa.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Pure / Commutative Classification ========== */

static bool gvn_is_eligible(const XiValue *v) {
    if (!v || v->nargs == 0)
        return false;
    if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW))
        return false;

    switch (v->op) {
        /* Arithmetic (binary) */
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        /* Bitwise (binary) */
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_SHL:
        case XI_SHR:
        /* Comparison (binary) */
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
        /* Unary */
        case XI_NEG:
        case XI_NOT:
        case XI_BNOT:
        case XI_ISNULL:
        case XI_CONVERT:
        /* Narrow / Widen */
        case XI_NARROW_I8:
        case XI_NARROW_U8:
        case XI_NARROW_I16:
        case XI_NARROW_U16:
        case XI_NARROW_I32:
        case XI_NARROW_U32:
        case XI_NARROW_F32:
        case XI_WIDEN_I8:
        case XI_WIDEN_U8:
        case XI_WIDEN_I16:
        case XI_WIDEN_U16:
        case XI_WIDEN_I32:
        case XI_WIDEN_U32:
        case XI_WIDEN_F32:
            return true;

        /* Memory loads participate if TBAA is active. */
        case XI_LOAD_FIELD:
        case XI_INDEX_GET:
        case XI_STRUCT_GET:
        case XI_JSON_GET_F:
        case XI_TUPLE_GET:
        case XI_GET_SHARED:
        case XI_GET_GLOBAL:
        case XI_LOAD_UPVAL:
            return true;

        default:
            return false;
    }
}

static bool gvn_is_commutative(uint16_t op) {
    switch (op) {
        case XI_ADD:
        case XI_MUL:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_EQ:
        case XI_NE:
            return true;
        default:
            return false;
    }
}

static bool gvn_is_load(uint16_t op) {
    return xi_is_memory_load(op);
}

/* ========== Value Number Table ========== */

#define GVN_MIN_TABLE 64
#define GVN_NO_VN UINT32_MAX

typedef struct {
    uint32_t hash; /* 0 = empty slot */
    uint16_t op;
    uint32_t vn_arg0;    /* value number of arg0 */
    uint32_t vn_arg1;    /* value number of arg1 (GVN_NO_VN for unary) */
    int64_t aux;         /* aux_int for field index disambiguation */
    uint32_t vn;         /* the assigned value number */
    XiValue *leader;     /* canonical leader value */
    uint32_t leader_blk; /* block index of leader (for dominance check) */
} VnEntry;

typedef struct {
    VnEntry *table;
    uint32_t size;
    uint32_t mask;
    uint32_t next_vn;

    /* Per-value VN assignment (indexed by value id). */
    uint32_t *val_vn;
    uint32_t val_vn_cap;
} VnTable;

static void vn_init(VnTable *vn, uint32_t est_values) {
    vn->size = GVN_MIN_TABLE;
    while (vn->size < est_values * 2)
        vn->size <<= 1;
    vn->mask = vn->size - 1;
    vn->table = (VnEntry *) xr_calloc(vn->size, sizeof(VnEntry));
    vn->next_vn = 1; /* VN 0 reserved for "unique" (no match) */
    vn->val_vn_cap = est_values + 16;
    vn->val_vn = (uint32_t *) xr_malloc(vn->val_vn_cap * sizeof(uint32_t));
    if (vn->val_vn) {
        for (uint32_t i = 0; i < vn->val_vn_cap; i++)
            vn->val_vn[i] = GVN_NO_VN;
    }
}

static void vn_destroy(VnTable *vn) {
    xr_free(vn->table);
    xr_free(vn->val_vn);
    memset(vn, 0, sizeof(*vn));
}

static uint32_t vn_get(const VnTable *vn, const XiValue *v) {
    if (!v || v->id >= vn->val_vn_cap)
        return GVN_NO_VN;
    return vn->val_vn[v->id];
}

static void vn_set(VnTable *vn, const XiValue *v, uint32_t num) {
    XR_DCHECK(v != NULL, "vn_set: NULL value");
    if (v->id >= vn->val_vn_cap)
        return;
    vn->val_vn[v->id] = num;
}

static uint32_t vn_compute_hash(uint16_t op, uint32_t vn0, uint32_t vn1, int64_t aux) {
    uint32_t h = (uint32_t) op * 2654435761u;
    h ^= vn0 * 2246822519u;
    h ^= vn1 * 3266489917u;
    h ^= (uint32_t) aux * 2034824023u;
    return h ? h : 1;
}

/* Look up or insert an expression.  Returns the VN and sets *leader
 * to the canonical value (NULL if this is the first occurrence). */
static uint32_t vn_lookup(VnTable *vn, uint16_t op, uint32_t vn0, uint32_t vn1, int64_t aux,
                          XiValue *val, uint32_t blk_idx, XiValue **leader) {
    XR_DCHECK(vn->table != NULL, "vn_lookup: NULL table");

    /* Normalize commutative: ensure vn0 <= vn1. */
    if (gvn_is_commutative(op) && vn0 > vn1) {
        uint32_t tmp = vn0;
        vn0 = vn1;
        vn1 = tmp;
    }

    uint32_t h = vn_compute_hash(op, vn0, vn1, aux);
    uint32_t slot = h & vn->mask;

    for (uint32_t probe = 0; probe < vn->size; probe++) {
        uint32_t idx = (slot + probe) & vn->mask;
        VnEntry *e = &vn->table[idx];

        if (e->hash == 0) {
            /* Empty: this is a new expression. */
            uint32_t new_vn = vn->next_vn++;
            e->hash = h;
            e->op = op;
            e->vn_arg0 = vn0;
            e->vn_arg1 = vn1;
            e->aux = aux;
            e->vn = new_vn;
            e->leader = val;
            e->leader_blk = blk_idx;
            *leader = NULL;
            return new_vn;
        }

        if (e->hash == h && e->op == op && e->vn_arg0 == vn0 && e->vn_arg1 == vn1 &&
            e->aux == aux) {
            /* Match: return existing VN + leader. */
            *leader = e->leader;
            return e->vn;
        }
    }

    /* Table full (should not happen at 50% load factor). */
    *leader = NULL;
    return vn->next_vn++;
}

/* ========== Store Tracking for Load Elimination ========== */

/* Check if any store between a leader load and the current load may
 * alias.  Conservative: scans instructions in the current block before
 * the current position.  For cross-block loads, falls back to "may alias"
 * unless both are in the same block. */
static bool has_aliasing_store_between(const XiFunc *f, const XiValue *leader,
                                       const XiValue *current, const XiBlock *cur_blk,
                                       uint32_t cur_vi) {
    (void) f;

    /* If leader is not in the same block, be conservative. */
    if (!leader->block || leader->block != cur_blk)
        return true;

    /* Scan from leader's position to current position in the block. */
    bool past_leader = false;
    for (uint32_t vi = 0; vi < cur_vi; vi++) {
        XiValue *v = cur_blk->values[vi];
        if (!v)
            continue;
        if (v == leader) {
            past_leader = true;
            continue;
        }
        if (!past_leader)
            continue;

        /* Check if this instruction is a store that may alias current. */
        if (xi_is_memory_store(v->op) || v->op == XI_CALL || v->op == XI_CALL_METHOD ||
            v->op == XI_CALL_BUILTIN) {
            if (xi_tbaa_may_alias(v, current))
                return true;
        }
    }

    return false;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_gvn(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_gvn: NULL func");
    if (f->nblocks < 1)
        return xi_pass_no_change();

    /* Ensure dominator tree is available. */
    xi_compute_rpo(f);
    xi_compute_dominators(f);

    bool tbaa_active = (f->invariant_mask & XI_INV_TBAA_ANNOTATED) != 0;

    /* Count total values for table sizing. */
    uint32_t total_values = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (f->blocks[bi])
            total_values += f->blocks[bi]->nvalues;
    }

    VnTable vn;
    vn_init(&vn, total_values);
    if (!vn.table || !vn.val_vn) {
        vn_destroy(&vn);
        return xi_pass_no_change();
    }

    /* Assign unique VNs to params and constants (they are their own leaders). */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_PARAM || v->op == XI_CONST) {
                vn_set(&vn, v, vn.next_vn++);
            }
        }
    }

    uint32_t n_replaced = 0;

    /* Walk blocks in RPO (dominator-tree pre-order).
     * f->blocks is already in approximate RPO after lowering. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;

            /* Skip already-processed or ineligible values. */
            if (!gvn_is_eligible(v)) {
                /* Assign unique VN so dependents can still hash. */
                if (vn_get(&vn, v) == GVN_NO_VN)
                    vn_set(&vn, v, vn.next_vn++);
                continue;
            }

            /* Build VN key from operand VNs. */
            uint32_t vn0 = GVN_NO_VN, vn1 = GVN_NO_VN;
            if (v->nargs >= 1 && v->args[0])
                vn0 = vn_get(&vn, v->args[0]);
            if (v->nargs >= 2 && v->args[1])
                vn1 = vn_get(&vn, v->args[1]);

            /* If any operand has no VN yet, assign a unique VN to this value. */
            if ((v->nargs >= 1 && vn0 == GVN_NO_VN) || (v->nargs >= 2 && vn1 == GVN_NO_VN)) {
                vn_set(&vn, v, vn.next_vn++);
                continue;
            }

            /* Include aux_int in the key for field/slot disambiguation. */
            int64_t aux_key = 0;
            if (gvn_is_load(v->op) || v->op == XI_LOAD_FIELD || v->op == XI_STRUCT_GET ||
                v->op == XI_JSON_GET_F || v->op == XI_TUPLE_GET || v->op == XI_GET_SHARED ||
                v->op == XI_GET_GLOBAL || v->op == XI_LOAD_UPVAL) {
                aux_key = v->aux_int;
            }

            XiValue *leader = NULL;
            uint32_t this_vn = vn_lookup(&vn, v->op, vn0, vn1, aux_key, v, bi, &leader);
            vn_set(&vn, v, this_vn);

            if (!leader)
                continue; /* First occurrence — nothing to eliminate. */

            /* Check dominance. */
            if (!leader->block || !xi_dominates(leader->block, blk))
                continue;

            /* For memory loads: check no aliasing store intervenes. */
            if (gvn_is_load(v->op)) {
                if (!tbaa_active)
                    continue; /* Cannot prove safety without TBAA. */
                if (has_aliasing_store_between(f, leader, v, blk, vi))
                    continue;
            }

            /* Replace with copy of leader. */
            v->op = XI_COPY;
            v->args[0] = leader;
            v->nargs = 1;
            v->mem_group = XI_MEM_NONE;
            n_replaced++;
        }
    }

    vn_destroy(&vn);

    if (n_replaced == 0)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();
    chg.values_changed = true;
    chg.n_removed = n_replaced;
    return chg;
}
