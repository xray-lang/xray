/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_block_layout.c - Profile-guided block reordering
 *
 * When block frequency data is available, blocks are sorted so that
 * hot blocks appear earlier and fall-through edges follow the most
 * frequent successor.  When no profile data exists, blocks are
 * ordered by RPO (the default from SSA construction), which already
 * places loop headers before loop bodies.
 *
 * The layout pass never changes CFG edges — it only reorders the
 * f->blocks[] array and updates block IDs accordingly.
 */

#include "xi_block_layout.h"
#include "xi.h"
#include "xi_analysis.h"
#include "../base/xmalloc.h"
#include <string.h>

/* Chain formation: greedily build chains starting from highest-frequency
 * blocks, appending the hottest unvisited successor. */

/* Comparison function for sorting blocks by frequency (descending). */
static int cmp_block_freq_desc(const void *a, const void *b) {
    const XiBlock *ba = *(const XiBlock *const *) a;
    const XiBlock *bb = *(const XiBlock *const *) b;
    if (ba->frequency > bb->frequency)
        return -1;
    if (ba->frequency < bb->frequency)
        return 1;
    /* Tie-break by RPO to keep stability. */
    if (ba->rpo < bb->rpo)
        return -1;
    if (ba->rpo > bb->rpo)
        return 1;
    return 0;
}

/* Check if profile data is available (any block has frequency > 0). */
static bool has_profile(const XiFunc *f) {
    for (uint32_t i = 0; i < f->nblocks; i++) {
        if (f->blocks[i] && f->blocks[i]->frequency > 0)
            return true;
    }
    return false;
}

/* Layout with profile: chain formation.
 * Returns true if the block order changed. */
static bool layout_with_profile(XiFunc *f) {
    uint32_t n = f->nblocks;
    if (n <= 2)
        return false;

    XiBlock **sorted = (XiBlock **) xr_malloc(n * sizeof(XiBlock *));
    if (!sorted)
        return false;
    memcpy(sorted, f->blocks, n * sizeof(XiBlock *));

    /* Sort by frequency descending. */
    qsort(sorted, n, sizeof(XiBlock *), cmp_block_freq_desc);

    /* Chain formation: greedily pick successor with highest frequency. */
    uint8_t *placed = (uint8_t *) xr_malloc(n);
    if (!placed) {
        xr_free(sorted);
        return false;
    }
    memset(placed, 0, n);

    XiBlock **result = (XiBlock **) xr_malloc(n * sizeof(XiBlock *));
    if (!result) {
        xr_free(sorted);
        xr_free(placed);
        return false;
    }

    uint32_t ri = 0;

    /* Entry block must always be first. */
    if (f->entry) {
        result[ri++] = f->entry;
        placed[f->entry->id] = 1;
    }

    /* Process remaining blocks in frequency order, building chains. */
    for (uint32_t si = 0; si < n && ri < n; si++) {
        XiBlock *seed = sorted[si];
        if (!seed || placed[seed->id])
            continue;

        /* Start a chain from this seed. */
        XiBlock *cur = seed;
        while (cur && !placed[cur->id] && ri < n) {
            result[ri++] = cur;
            placed[cur->id] = 1;

            /* Follow the hottest successor. */
            XiBlock *best = NULL;
            uint32_t best_freq = 0;
            for (int s = 0; s < 2; s++) {
                XiBlock *succ = cur->succs[s];
                if (succ && !placed[succ->id] && succ->frequency > best_freq) {
                    best = succ;
                    best_freq = succ->frequency;
                }
            }
            cur = best;
        }
    }

    /* Check if order actually changed. */
    bool changed = false;
    for (uint32_t i = 0; i < n && !changed; i++) {
        if (result[i] != f->blocks[i])
            changed = true;
    }

    if (changed) {
        memcpy(f->blocks, result, n * sizeof(XiBlock *));
        for (uint32_t i = 0; i < n; i++) {
            if (f->blocks[i])
                f->blocks[i]->id = i;
        }
    }

    xr_free(result);
    xr_free(placed);
    xr_free(sorted);

    return changed;
}

/* Layout without profile: RPO order (already mostly correct from
 * SSA construction, but ensure it after CFG edits). */
static bool layout_rpo(XiFunc *f) {
    xi_ensure_dominators(f);

    uint32_t n = f->nblocks;
    if (n <= 2)
        return false;

    /* Check if blocks are already in RPO order. */
    bool already_rpo = true;
    for (uint32_t i = 0; i < n && already_rpo; i++) {
        if (f->blocks[i] && f->blocks[i]->rpo != i)
            already_rpo = false;
    }
    if (already_rpo)
        return false;

    /* Sort blocks by RPO index. */
    XiBlock **sorted = (XiBlock **) xr_malloc(n * sizeof(XiBlock *));
    if (!sorted)
        return false;

    memset(sorted, 0, n * sizeof(XiBlock *));
    for (uint32_t i = 0; i < n; i++) {
        XiBlock *b = f->blocks[i];
        if (b && b->rpo < n)
            sorted[b->rpo] = b;
    }

    /* Fill any gaps (blocks not in RPO order) at the end. */
    uint32_t fill = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (sorted[i]) {
            fill = i + 1;
            continue;
        }
    }

    bool changed = false;
    for (uint32_t i = 0; i < n; i++) {
        if (sorted[i] != f->blocks[i])
            changed = true;
    }

    if (changed) {
        memcpy(f->blocks, sorted, n * sizeof(XiBlock *));
        for (uint32_t i = 0; i < n; i++) {
            if (f->blocks[i])
                f->blocks[i]->id = i;
        }
    }

    xr_free(sorted);
    return changed;
}

XR_FUNC XiPassChange xi_opt_block_layout(XiFunc *f) {
    if (!f || f->nblocks <= 1)
        return xi_pass_no_change();

    /* Only reorder when profile data is available.
     * Without profile, the SSA construction order is already good. */
    if (!has_profile(f))
        return xi_pass_no_change();

    bool changed = layout_with_profile(f);

    if (!changed)
        return xi_pass_no_change();

    /* Block reordering invalidates CFG caches. */
    f->cfg_version++;

    return (XiPassChange) {
        .values_changed = false,
        .cfg_changed = true,
        .types_changed = false,
    };
}
