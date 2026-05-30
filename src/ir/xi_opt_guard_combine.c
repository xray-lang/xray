/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_guard_combine.c - Redundant guard elimination
 *
 * Within each basic block, tracks which (receiver_id, guard_type_id)
 * pairs have already been checked.  Subsequent guards with the same
 * pair are eliminated by replacing their uses with the earlier guard's
 * result and marking them dead.
 *
 * Cross-block elimination: if a block's single dominator already guards
 * a value, the dominated block's guard is redundant (dominator-based).
 */

#include "xi_opt_guard_combine.h"
#include "xi.h"
#include <string.h>

#define GUARD_SEEN_CAP 32

typedef struct {
    uint32_t receiver_id; /* guarded value's SSA id */
    int64_t type_id;      /* expected type (aux_int of XI_GUARD_TYPE) */
    XiValue *guard;       /* canonical guard value */
} GuardSeen;

typedef struct {
    GuardSeen entries[GUARD_SEEN_CAP];
    uint32_t count;
} GuardSeenTable;

static XiValue *find_seen(const GuardSeenTable *t, uint32_t recv_id, int64_t type_id) {
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->entries[i].receiver_id == recv_id && t->entries[i].type_id == type_id)
            return t->entries[i].guard;
    }
    return NULL;
}

static void add_seen(GuardSeenTable *t, uint32_t recv_id, int64_t type_id, XiValue *guard) {
    if (t->count >= GUARD_SEEN_CAP)
        return;
    t->entries[t->count].receiver_id = recv_id;
    t->entries[t->count].type_id = type_id;
    t->entries[t->count].guard = guard;
    t->count++;
}

/* Replace all uses of `old` with `rep` across the function. */
static void replace_all_uses(XiFunc *f, XiValue *old, XiValue *rep) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] == old)
                    v->args[a] = rep;
            }
        }
        if (blk->control == old)
            blk->control = rep;
    }
}

/* Remove value at index `idx` from block, shifting subsequent values down. */
static void block_remove_at(XiBlock *blk, uint32_t idx) {
    for (uint32_t j = idx; j + 1 < blk->nvalues; j++)
        blk->values[j] = blk->values[j + 1];
    blk->nvalues--;
}

XR_FUNC XiPassChange xi_opt_guard_combine(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    uint32_t n_elim = 0;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        GuardSeenTable seen;
        memset(&seen, 0, sizeof(seen));

        /* Inherit guards from immediate dominator (single predecessor
         * that dominates this block).  This catches guards placed in
         * a preheader that dominate the loop body. */
        if (blk->idom) {
            XiBlock *dom = blk->idom;
            for (uint32_t vi = 0; vi < dom->nvalues; vi++) {
                XiValue *v = dom->values[vi];
                if (v && v->op == XI_GUARD_TYPE && v->nargs >= 1) {
                    add_seen(&seen, v->args[0]->id, v->aux_int, v);
                }
            }
        }

        for (uint32_t vi = 0; vi < blk->nvalues; /* manual */) {
            XiValue *v = blk->values[vi];
            if (!v || v->op != XI_GUARD_TYPE || v->nargs < 1) {
                vi++;
                continue;
            }

            uint32_t recv_id = v->args[0]->id;
            int64_t type_id = v->aux_int;

            XiValue *existing = find_seen(&seen, recv_id, type_id);
            if (existing) {
                replace_all_uses(f, v, existing);
                block_remove_at(blk, vi);
                n_elim++;
                /* Don't increment vi — array shifted down. */
            } else {
                add_seen(&seen, recv_id, type_id, v);
                vi++;
            }
        }
    }

    if (n_elim == 0)
        return xi_pass_no_change();

    return (XiPassChange) {
        .values_changed = true,
        .cfg_changed = false,
        .types_changed = false,
        .n_removed = n_elim,
    };
}
