/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_comptime.c - IR constant optimization fixpoint
 *
 * Iterates lightweight constant-propagation passes (const-fold, copy-prop,
 * SCCP, DCE) until fixpoint or round limit. This pass is not the language
 * frontend `comptime` evaluator.
 */

#include "xi_opt_comptime.h"
#include "xi_opt.h"
#include "xi_opt_sccp.h"
#include "xi_pass.h"

XR_FUNC XiPassChange xi_opt_const_fixpoint(XiFunc *f, XiOptDisableMask disabled) {
    if (!f)
        return xi_pass_no_change();

    /* This pass re-runs four passes that the optimizer policy can withhold by
     * name. Reusing them without consulting the mask made every one of them
     * run again after the driver had skipped it, so a spec that named one of
     * them changed nothing and its measured contribution was somebody else's.
     *
     * A withheld constituent is dropped from the loop rather than cancelling
     * the whole fixpoint. The mask names one pass per name and exists so a
     * divergence can be bisected pass by pass; if `-sccp` also stopped
     * const-fold, copy-prop and DCE from iterating, the difference it
     * measured would again be four passes wide. `-const_fixpoint` is the
     * existing spelling for stopping the fixpoint itself. */
    const bool run_const_fold = !xi_pass_withheld_by_mask(disabled, XI_OPT_PASS_CONSTFOLD);
    const bool run_copy_prop = !xi_pass_withheld_by_mask(disabled, XI_OPT_PASS_COPY_PROP);
    const bool run_sccp = !xi_pass_withheld_by_mask(disabled, XI_OPT_PASS_SCCP);
    const bool run_dce = !xi_pass_withheld_by_mask(disabled, XI_OPT_PASS_DCE);

    XiPassChange total = xi_pass_no_change();

    /* Fixpoint of const-fold + copy-prop + SCCP + DCE, minus the withheld. */
    for (int round = 0; round < XI_CONST_FIXPOINT_MAX_ROUNDS; round++) {
        XiPassChange round_chg = xi_pass_no_change();

        if (run_const_fold)
            round_chg = xi_pass_merge(round_chg, xi_opt_const_fold(f));
        if (run_copy_prop)
            round_chg = xi_pass_merge(round_chg, xi_opt_copy_prop(f));
        if (run_sccp)
            round_chg = xi_pass_merge(round_chg, xi_opt_sccp(f));
        if (run_dce)
            round_chg = xi_pass_merge(round_chg, xi_opt_dce(f));

        total = xi_pass_merge(total, round_chg);

        if (!round_chg.values_changed && !round_chg.cfg_changed && !round_chg.types_changed)
            break;
    }

    return total;
}
