/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_spec_inline.c - Speculative devirtualization via IC
 *
 * Converts XI_CALL_METHOD sites to XI_CALL_METHOD_DIRECT when the
 * receiver has been guarded by XI_GUARD_TYPE (from spec_narrow) and
 * the guard's type_id matches a monomorphic or polymorphic IC target.
 */

#include "xi_opt_spec_inline.h"
#include "xi.h"
#include "xi_ic.h"
#include "xi_guard_cost.h"

static XiValue *find_dominating_guard(const XiBlock *blk, const XiValue *receiver) {
    for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
        XiValue *v = blk->values[vi];
        if (!v || v->op != XI_GUARD_TYPE || v->nargs < 1)
            continue;
        if (v->args[0] == receiver)
            return v;
    }

    if (blk->idom)
        return find_dominating_guard(blk->idom, receiver);

    return NULL;
}

static const XiIcTarget *ic_target_for_guard(const XiIcMeta *ic, uint32_t guard_type_id) {
    if (!ic || ic->ntargets == 0)
        return NULL;

    const XiIcTarget *best = NULL;
    for (uint32_t i = 0; i < ic->ntargets; i++) {
        const XiIcTarget *t = &ic->targets[i];
        if (t->type_id != guard_type_id)
            continue;
        if (!best || t->hit_count > best->hit_count)
            best = t;
    }
    return best;
}

static bool try_spec_devirt(XiValue *call, XiValue *guard, const XiIcMeta *ic) {
    if (!call || !guard || !ic)
        return false;
    if (ic->kind != XI_IC_MONO && ic->kind != XI_IC_POLY)
        return false;

    const XiIcTarget *target = ic_target_for_guard(ic, (uint32_t) guard->aux_int);
    if (!target)
        return false;

    call->op = XI_CALL_METHOD_DIRECT;
    call->args[0] = guard;
    (void) target;
    return true;
}

XR_FUNC XiPassChange xi_opt_spec_inline(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    if (!(f->invariant_mask & XI_INV_IC_ATTACHED) || !f->ic_table)
        return xi_pass_no_change();

    if (f->ic_table->nentries == 0)
        return xi_pass_no_change();

    uint32_t n_devirt = 0;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->op != XI_CALL_METHOD)
                continue;
            if (v->nargs < 1 || !v->args[0])
                continue;

            const XiIcMeta *ic = xi_ic_lookup(f, v->id);
            if (!ic)
                continue;

            XiValue *guard = find_dominating_guard(blk, v->args[0]);
            if (!guard)
                continue;

            /* Reject speculation if deopt cost is too high. */
            uint32_t guard_pos = 0;
            for (uint32_t gi = 0; gi < blk->nvalues; gi++) {
                if (blk->values[gi] == guard) {
                    guard_pos = gi;
                    break;
                }
            }
            if (!xi_guard_should_speculate(f, blk, guard_pos, guard))
                continue;

            if (try_spec_devirt(v, guard, ic))
                n_devirt++;
        }
    }

    if (n_devirt == 0)
        return xi_pass_no_change();

    return (XiPassChange) {
        .values_changed = true,
        .cfg_changed = false,
        .types_changed = false,
        .n_added = 0,
    };
}
