/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_guard_motion.c - Guard hoisting out of loops
 *
 * For each natural loop, scans body blocks for XI_GUARD_TYPE ops whose
 * receiver operand is defined outside the loop (loop-invariant).  Such
 * guards are moved to the loop preheader so the type check runs once
 * instead of every iteration.
 *
 * The hoisted guard value replaces its old position; downstream uses
 * within the loop body continue to reference the same SSA value.
 */

#include "xi_opt_guard_motion.h"
#include "xi.h"
#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xmalloc.h"

/* Check if a value is defined inside the loop. */
static bool is_loop_internal(const XiLoop *loop, const XiValue *v) {
    if (!v || !v->block)
        return false;
    return xi_loop_contains_block(loop, v->block);
}

/* Move a guard from body_blk[vi] to the end of preheader. */
static void hoist_guard(XiBlock *body_blk, uint32_t vi, XiBlock *preheader) {
    XiValue *guard = body_blk->values[vi];

    /* Remove from body: shift remaining values down. */
    for (uint32_t j = vi; j + 1 < body_blk->nvalues; j++) {
        body_blk->values[j] = body_blk->values[j + 1];
    }
    body_blk->nvalues--;

    /* Append to preheader (before terminator, which is encoded in
     * block kind + control, not in the values array). */
    if (preheader->nvalues >= preheader->values_cap) {
        uint32_t new_cap = preheader->values_cap ? preheader->values_cap * 2 : 8;
        XiValue **grown = (XiValue **) xr_realloc(preheader->values, new_cap * sizeof(XiValue *));
        if (!grown)
            return;
        preheader->values = grown;
        preheader->values_cap = new_cap;
    }
    preheader->values[preheader->nvalues++] = guard;
    guard->block = preheader;
}

XR_FUNC XiPassChange xi_opt_guard_motion(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    xi_ensure_dominators(f);
    XiLoopInfo *li = xi_ensure_loops(f);
    if (!li || li->nloop == 0)
        return xi_pass_no_change();

    uint32_t n_hoisted = 0;

    for (uint32_t l = 0; l < li->nloop; l++) {
        XiLoop *loop = li->all_loops[l];
        if (!loop || !loop->preheader)
            continue;

        XiBlock *preheader = loop->preheader;

        for (uint32_t b = 0; b < loop->nbody; b++) {
            XiBlock *blk = loop->body[b];
            if (!blk)
                continue;

            for (uint32_t vi = 0; vi < blk->nvalues; /* incremented conditionally */) {
                XiValue *v = blk->values[vi];
                if (!v || v->op != XI_GUARD_TYPE || v->nargs < 1) {
                    vi++;
                    continue;
                }

                XiValue *receiver = v->args[0];

                /* Guard is loop-invariant if its receiver is defined
                 * outside the loop. */
                if (!is_loop_internal(loop, receiver)) {
                    hoist_guard(blk, vi, preheader);
                    n_hoisted++;
                    /* Don't increment vi — array shifted down. */
                } else {
                    vi++;
                }
            }
        }
    }

    if (n_hoisted == 0)
        return xi_pass_no_change();

    return (XiPassChange) {
        .values_changed = true,
        .cfg_changed = false,
        .types_changed = false,
        .n_removed = 0,
        .n_added = 0,
    };
}
