/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_jit_profile.c - Apply runtime profile to Xi IR for JIT tier-up
 */

#include "xi_jit_profile.h"
#include "xi_ic.h"
#include "xi_opt_spec_narrow.h"
#include "xi_opt_guard_combine.h"
#include "xi_opt_guard_motion.h"
#include "xi_opt_spec_inline.h"
#include "xi_guard_cost.h"
#include "xi_opt_spec_const.h"
#include "xi_pass.h"
#include "../base/xchecks.h"

static void apply_block_frequencies(XiFunc *f, const XiJitProfileInput *profile) {
    if (!f || !profile || !profile->block_freq || profile->block_freq_count == 0)
        return;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        if (blk->id < profile->block_freq_count)
            blk->frequency = profile->block_freq[blk->id];
    }
}

static bool run_speculative_passes(XiFunc *f) {
    XiPassChange total = xi_pass_no_change();

    XiPassChange c;

    c = xi_opt_spec_narrow(f);
    total = xi_pass_merge(total, c);

    c = xi_opt_guard_combine(f);
    total = xi_pass_merge(total, c);

    c = xi_opt_guard_motion(f);
    total = xi_pass_merge(total, c);

    c = xi_opt_spec_inline(f);
    total = xi_pass_merge(total, c);

    c = xi_guard_cost_fill(f);
    total = xi_pass_merge(total, c);

    c = xi_opt_spec_const(f);
    total = xi_pass_merge(total, c);

    return total.values_changed || total.cfg_changed || total.types_changed;
}

XR_FUNC bool xi_jit_apply_profile(XiFunc *f, const XiJitProfileInput *profile) {
    if (!f)
        return false;

    if (profile) {
        apply_block_frequencies(f, profile);
        (void) xi_ic_attach(f, profile->ic_fields, profile->ic_methods);
    } else {
        (void) xi_ic_attach(f, NULL, NULL);
    }

    return run_speculative_passes(f);
}
