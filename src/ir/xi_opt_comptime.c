/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_comptime.c - Compile-time constant evaluation
 *
 * Iterates lightweight constant-propagation passes (const-fold, copy-prop,
 * SCCP, DCE) until fixpoint or round limit.
 */

#include "xi_opt_comptime.h"
#include "xi_opt.h"
#include "xi_opt_sccp.h"
#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_comptime_eval(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    XiPassChange total = xi_pass_no_change();

    /* Fixpoint of const-fold + copy-prop + SCCP + DCE. */
    for (int round = 0; round < XI_COMPTIME_MAX_ROUNDS; round++) {
        XiPassChange round_chg = xi_pass_no_change();

        round_chg = xi_pass_merge(round_chg, xi_opt_const_fold(f));
        round_chg = xi_pass_merge(round_chg, xi_opt_copy_prop(f));
        round_chg = xi_pass_merge(round_chg, xi_opt_sccp(f));
        round_chg = xi_pass_merge(round_chg, xi_opt_dce(f));

        total = xi_pass_merge(total, round_chg);

        if (!round_chg.values_changed && !round_chg.cfg_changed && !round_chg.types_changed)
            break;
    }

    return total;
}
