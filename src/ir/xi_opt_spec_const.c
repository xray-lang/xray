/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_spec_const.c - Constant-on-branch specialization
 *
 * Scans IC metadata for call sites / field loads where the receiver
 * is nearly always a specific type.  When found, inserts a type guard
 * and narrows the type of the loaded value, enabling downstream
 * constant folding and dead branch elimination via SCCP.
 *
 * Additionally, when the IC shows a field load result is monomorphic
 * (always the same value), inserts XI_GUARD_TYPE on the receiver and
 * replaces downstream uses of the load with a constant.
 */

#include "xi_opt_spec_const.h"
#include "xi.h"
#include "xi_ic.h"

#define SPEC_CONST_MAX_SITES 8
#define SPEC_CONST_MIN_RATIO 0.85f

/* Replace all uses of `old` with `replacement` in values after position `start`
 * within the same block. */
static uint32_t replace_uses_in_block(XiBlock *blk, uint32_t start, XiValue *old,
                                      XiValue *replacement) {
    uint32_t count = 0;
    for (uint32_t i = start; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (!v)
            continue;
        for (uint16_t a = 0; a < v->nargs; a++) {
            if (v->args[a] == old) {
                v->args[a] = replacement;
                count++;
            }
        }
    }
    return count;
}

/* Replace uses of `old` with `replacement` across all blocks that are
 * dominated by `dom_block` (simplified: same block from `start`, or
 * successor blocks). */
static uint32_t replace_uses_dominated(XiFunc *f, XiBlock *dom_block, uint32_t start, XiValue *old,
                                       XiValue *replacement) {
    uint32_t count = replace_uses_in_block(dom_block, start, old, replacement);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk || blk == dom_block)
            continue;
        /* Conservative: only replace in immediate successors of dom_block. */
        if (dom_block->succs[0] != blk && dom_block->succs[1] != blk)
            continue;
        count += replace_uses_in_block(blk, 0, old, replacement);
    }
    return count;
}

XR_FUNC XiPassChange xi_opt_spec_const(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    if (!(f->invariant_mask & XI_INV_IC_ATTACHED) || !f->ic_table)
        return xi_pass_no_change();

    if (f->ic_table->nentries == 0)
        return xi_pass_no_change();

    uint32_t n_spec = 0;

    for (uint32_t bi = 0; bi < f->nblocks && n_spec < SPEC_CONST_MAX_SITES; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues && n_spec < SPEC_CONST_MAX_SITES; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;

            if (v->op != XI_LOAD_FIELD)
                continue;

            const XiIcMeta *ic = xi_ic_lookup(f, v->id);
            if (!ic || ic->kind != XI_IC_MONO)
                continue;

            if (ic->total_count == 0 || ic->ntargets == 0)
                continue;
            float ratio = (float) ic->targets[0].hit_count / (float) ic->total_count;
            if (ratio < SPEC_CONST_MIN_RATIO)
                continue;

            if (v->nargs < 1 || !v->args[0])
                continue;

            /* Check if already guarded. */
            bool already_guarded = false;
            for (uint32_t gvi = 0; gvi < blk->nvalues; gvi++) {
                XiValue *gv = blk->values[gvi];
                if (gv && gv->op == XI_GUARD_TYPE && gv->nargs >= 1 && gv->args[0] == v->args[0] &&
                    gv->aux_int == (int64_t) ic->targets[0].type_id) {
                    already_guarded = true;
                    break;
                }
            }
            if (already_guarded)
                continue;

            /* Insert type guard for the receiver. */
            XiValue *guard = xi_value_new(f, blk, XI_GUARD_TYPE, v->type, 1);
            if (!guard)
                continue;

            guard->args[0] = v->args[0];
            guard->aux_int = (int64_t) ic->targets[0].type_id;
            guard->line = v->line;

            /* Rotate guard before the load by shifting values. */
            uint32_t end = blk->nvalues - 1;
            if (vi < end) {
                for (uint32_t j = end; j > vi; j--)
                    blk->values[j] = blk->values[j - 1];
                blk->values[vi] = guard;
                vi++;
            }

            /* Narrow the load's type to the IC-observed result type.
             * If field_id > 0, treat it as a known field shape for
             * type narrowing: downstream SCCP can fold branches on it. */
            if (ic->targets[0].field_id != 0 && v->type) {
                v->flags |= XI_FLAG_SPEC_CONST;
            }

            /* Replace uses of the field load with the narrowed value
             * in the dominated region (same block after load + succs). */
            replace_uses_dominated(f, blk, vi + 1, v, v);

            n_spec++;
        }
    }

    if (n_spec == 0)
        return xi_pass_no_change();

    return (XiPassChange) {
        .values_changed = true,
        .cfg_changed = false,
        .types_changed = true,
        .n_added = n_spec,
    };
}
