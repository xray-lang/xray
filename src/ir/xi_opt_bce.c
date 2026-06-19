/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_bce.c - Bounds Check Elimination for Xi IR
 *
 * Eliminates XI_BOUNDS_CHECK(idx, len) when range analysis can prove
 * that range(idx) is entirely within [0, range(len).lo - 1].
 *
 * Two elimination strategies (non-loop only):
 *   1. Range proof: range(idx).lo >= 0 && range(idx).hi < range(len).lo
 *   2. Dominator dedup: a dominating block already contains an identical
 *      XI_BOUNDS_CHECK with the same (idx.id, len.id) pair
 *
 * Eliminated checks are replaced with XI_COPY of the index value,
 * preserving the SSA def chain.
 */

#include "xi_opt_bce.h"
#include "xi_range.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Dominator Check Dedup ========== */

/* Key: (idx_id, len_id) pair identifying a specific bounds check. */
typedef struct {
    uint32_t idx_id;
    uint32_t len_id;
} BceKey;

/* Simple linear scan for dominated-check dedup.  Sufficient for
 * typical function sizes (< 200 blocks).  Stores checks seen in
 * dominator-tree pre-order so that earlier entries dominate later. */
typedef struct {
    BceKey *keys;
    uint32_t *block_ids; /* block where the check lives */
    uint32_t len;
    uint32_t cap;
} BceSeen;

static void bce_seen_init(BceSeen *s) {
    memset(s, 0, sizeof(*s));
}

static void bce_seen_free(BceSeen *s) {
    xr_free(s->keys);
    xr_free(s->block_ids);
}

static void bce_seen_push(BceSeen *s, uint32_t idx_id, uint32_t len_id, uint32_t blk_id) {
    if (s->len >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 32;
        XR_REALLOC_OR_ABORT(s->keys, s->cap * sizeof(BceKey), "bce keys");
        XR_REALLOC_OR_ABORT(s->block_ids, s->cap * sizeof(uint32_t), "bce blk_ids");
    }
    s->keys[s->len] = (BceKey) {idx_id, len_id};
    s->block_ids[s->len] = blk_id;
    s->len++;
}

/* Check if (idx_id, len_id) is dominated by an existing check.
 * blk must be dominated by the block of the existing check. */
static bool bce_seen_dominates(const BceSeen *s, uint32_t idx_id, uint32_t len_id,
                               const XiBlock *blk) {
    for (uint32_t i = 0; i < s->len; i++) {
        if (s->keys[i].idx_id == idx_id && s->keys[i].len_id == len_id) {
            /* Walk idom chain from blk to see if the seen block dominates. */
            const XiBlock *cur = blk;
            while (cur) {
                if (cur->id == s->block_ids[i])
                    return true;
                cur = cur->idom;
            }
        }
    }
    return false;
}

/* ========== Range-based Proof ========== */

/* Returns true if range analysis proves idx is in [0, len). */
static bool range_proves_safe(const XiValue *idx, const XiValue *len) {
    XR_DCHECK(idx != NULL, "bce: null idx");
    XR_DCHECK(len != NULL, "bce: null len");

    XiRange idx_r = xi_range_of(idx);
    XiRange len_r = xi_range_of(len);

    /* Need concrete bounds on both sides. */
    if (idx_r.is_top || idx_r.is_bot)
        return false;
    if (len_r.is_top || len_r.is_bot)
        return false;

    /* Proof: idx.lo >= 0 && idx.hi < len.lo
     * (len.lo is the minimum possible length) */
    return idx_r.lo >= 0 && idx_r.hi < len_r.lo;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_bce(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_bce: NULL func");

    if (f->nblocks == 0)
        return xi_pass_no_change();

    /* Range analysis must have run. */
    if (!(f->invariant_mask & XI_INV_RANGE_ANNOTATED))
        return xi_pass_no_change();

    BceSeen seen;
    bce_seen_init(&seen);
    uint32_t eliminated = 0;

    /* Iterate blocks in RPO (approximates dominator-tree pre-order). */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        XR_DCHECK(blk != NULL, "bce: null block");

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->op != XI_BOUNDS_CHECK)
                continue;
            XR_DCHECK(v->nargs >= 2, "BOUNDS_CHECK needs idx + len args");

            XiValue *idx = v->args[0];
            XiValue *len = v->args[1];
            XR_DCHECK(idx != NULL && len != NULL, "bce: null check args");

            bool can_eliminate = false;

            /* Strategy 1: range proof */
            if (range_proves_safe(idx, len)) {
                can_eliminate = true;
            }

            /* Strategy 2: dominator dedup */
            if (!can_eliminate && bce_seen_dominates(&seen, idx->id, len->id, blk)) {
                can_eliminate = true;
            }

            if (can_eliminate) {
                /* Replace BOUNDS_CHECK with COPY of index (preserves SSA def). */
                v->op = XI_COPY;
                v->args[0] = idx;
                v->nargs = 1;
                v->aux_int = XI_COPY_KIND_IDENTITY;
                v->aux = NULL;
                v->flags &= ~(XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW);
                eliminated++;
            } else {
                /* Record this check for future dedup. */
                bce_seen_push(&seen, idx->id, len->id, blk->id);
            }
        }
    }

    bce_seen_free(&seen);

    XiPassChange chg = xi_pass_no_change();
    if (eliminated > 0)
        chg.values_changed = true;
    return chg;
}
