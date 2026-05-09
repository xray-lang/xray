/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_defuse.c - Def-use chain construction for Xi IR
 *
 * ALGORITHM:
 *   Two-pass construction:
 *     Pass 1: count uses per value ID
 *     Pass 2: fill use records into pre-allocated flat array
 *   This avoids linked lists and gives cache-friendly iteration.
 */

#include "xi_defuse.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Pass 1: Count Uses ========== */

/* Count a reference to value |v| if non-NULL. */
static inline void count_ref(uint32_t *cnt, uint32_t max_id, const XiValue *v) {
    if (v && v->id < max_id)
        cnt[v->id]++;
}

/* Count all uses across the function. */
static void count_all_uses(uint32_t *cnt, uint32_t max_id, const XiFunc *f) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        /* Value args */
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++)
                count_ref(cnt, max_id, v->args[a]);
        }

        /* Phi args */
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++)
                count_ref(cnt, max_id, phi->value.args[a]);
        }

        /* Block control */
        count_ref(cnt, max_id, blk->control);
    }
}

/* ========== Pass 2: Record Use Sites ========== */

static inline void record_site(XiDefUse *du, uint32_t vid, uint32_t block_id, uint32_t value_id,
                               uint8_t kind, uint8_t arg_idx) {
    XR_DCHECK(vid < du->max_id, "record_site: vid out of range");
    uint32_t pos = du->offset[vid] + du->count[vid];
    XR_DCHECK(pos < du->total_sites, "record_site: pos out of range");
    du->sites[pos].block_id = block_id;
    du->sites[pos].value_id = value_id;
    du->sites[pos].kind = kind;
    du->sites[pos].arg_idx = arg_idx;
    du->count[vid]++;
}

static void fill_all_uses(XiDefUse *du, const XiFunc *f) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        /* Value args */
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] && v->args[a]->id < du->max_id) {
                    record_site(du, v->args[a]->id, blk->id, v->id, XI_USE_VALUE_ARG, a);
                }
            }
        }

        /* Phi args */
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] && phi->value.args[a]->id < du->max_id) {
                    record_site(du, phi->value.args[a]->id, blk->id, phi->value.id, XI_USE_PHI_ARG,
                                a);
                }
            }
        }

        /* Block control */
        if (blk->control && blk->control->id < du->max_id) {
            record_site(du, blk->control->id, blk->id, UINT32_MAX, XI_USE_CONTROL, 0);
        }
    }
}

/* ========== Public API ========== */

XR_FUNC void xi_defuse_build(XiDefUse *du, XiFunc *f) {
    XR_DCHECK(du != NULL, "xi_defuse_build: NULL du");
    XR_DCHECK(f != NULL, "xi_defuse_build: NULL func");
    memset(du, 0, sizeof(*du));

    uint32_t max_id = f->next_value_id;
    if (max_id == 0)
        return;

    du->max_id = max_id;
    du->count = (uint32_t *) xr_calloc(max_id, sizeof(uint32_t));
    du->offset = (uint32_t *) xr_calloc(max_id, sizeof(uint32_t));
    if (!du->count || !du->offset) {
        xr_free(du->count);
        xr_free(du->offset);
        memset(du, 0, sizeof(*du));
        return;
    }

    /* Pass 1: count */
    count_all_uses(du->count, max_id, f);

    /* Compute offsets (prefix sum) */
    uint32_t total = 0;
    for (uint32_t v = 0; v < max_id; v++) {
        du->offset[v] = total;
        total += du->count[v];
    }
    du->total_sites = total;

    if (total == 0)
        return;

    du->sites = (XiUseSite *) xr_calloc(total, sizeof(XiUseSite));
    if (!du->sites) {
        xr_free(du->count);
        xr_free(du->offset);
        memset(du, 0, sizeof(*du));
        return;
    }

    /* Reset counts — Pass 2 uses them as write cursors */
    memset(du->count, 0, max_id * sizeof(uint32_t));

    /* Pass 2: fill */
    fill_all_uses(du, f);
}

XR_FUNC void xi_defuse_free(XiDefUse *du) {
    if (!du)
        return;
    xr_free(du->sites);
    xr_free(du->offset);
    xr_free(du->count);
    memset(du, 0, sizeof(*du));
}
