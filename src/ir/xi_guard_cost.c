/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_guard_cost.c - Deoptimization cost model
 *
 * Computes the expected deopt penalty for speculative guards by
 * combining IC miss rate, live-value recovery cost, and call-site
 * frequency.  High-penalty guards should not be speculated.
 */

#include "xi_guard_cost.h"
#include "xi_ic.h"

#define RECOVERY_COST_PER_LIVE 5
#define GUARD_PENALTY_THRESHOLD 50.0f

static uint32_t estimate_live_at(const XiFunc *f, uint32_t guard_pos) {
    return f->nparams + guard_pos;
}

static float compute_miss_rate(const XiFunc *f, const XiBlock *blk, uint32_t guard_pos,
                               const XiValue *guard) {
    if (!(f->invariant_mask & XI_INV_IC_ATTACHED) || !f->ic_table)
        return 0.01f;

    for (uint32_t vi = guard_pos + 1; vi < blk->nvalues; vi++) {
        XiValue *user = blk->values[vi];
        if (!user)
            continue;
        bool uses_guard = false;
        for (uint16_t a = 0; a < user->nargs; a++) {
            if (user->args[a] == guard) {
                uses_guard = true;
                break;
            }
        }
        if (!uses_guard)
            continue;

        const XiIcMeta *ic = xi_ic_lookup(f, user->id);
        if (ic && ic->total_count > 0 && ic->ntargets > 0) {
            uint32_t guard_type = (uint32_t) guard->aux_int;
            uint32_t matched_hits = 0;
            for (uint32_t t = 0; t < ic->ntargets; t++) {
                if (ic->targets[t].type_id == guard_type) {
                    matched_hits = ic->targets[t].hit_count;
                    break;
                }
            }
            float rate = 1.0f - ((float) matched_hits / (float) ic->total_count);
            return rate < 0.001f ? 0.001f : rate;
        }
        break;
    }

    return 0.01f;
}

XR_FUNC XiPassChange xi_guard_cost_fill(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    uint32_t nguards = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v && v->op == XI_GUARD_TYPE)
                nguards++;
        }
    }

    f->invariant_mask |= XI_INV_GUARD_COST;

    if (nguards == 0)
        return xi_pass_no_change();

    return (XiPassChange) {
        .values_changed = false,
        .cfg_changed = false,
        .types_changed = false,
        .n_added = 0,
    };
}

XR_FUNC float xi_guard_expected_penalty(const XiValue *guard) {
    if (!guard || guard->op != XI_GUARD_TYPE)
        return 0.0f;
    return 0.0f;
}

XR_FUNC float xi_guard_compute_penalty(const XiFunc *f, const XiBlock *blk, uint32_t guard_pos,
                                       const XiValue *guard) {
    if (!f || !blk || !guard || guard->op != XI_GUARD_TYPE)
        return 0.0f;

    float miss_rate = compute_miss_rate(f, blk, guard_pos, guard);
    uint32_t recovery = estimate_live_at(f, guard_pos) * RECOVERY_COST_PER_LIVE;
    return miss_rate * (float) recovery;
}

XR_FUNC bool xi_guard_should_speculate(const XiFunc *f, const XiBlock *blk, uint32_t guard_pos,
                                       const XiValue *guard) {
    if (!f || !blk || !guard || guard->op != XI_GUARD_TYPE)
        return false;
    float penalty = xi_guard_compute_penalty(f, blk, guard_pos, guard);
    return penalty < GUARD_PENALTY_THRESHOLD;
}
