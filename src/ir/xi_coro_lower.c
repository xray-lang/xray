/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_coro_lower.c - Shared coroutine CFG lowering for Xi IR
 */

#include "xi_coro_lower.h"

/* Lower a single function body into an explicit state machine.  Children are
 * lowered first so a parent's direct-suspend-call analysis observes fully
 * lowered callees.  The coroutine plan is materialized (and cached on the
 * function) here so it is stable before representation selection runs. */
static bool coro_lower_func(XiFunc *f, const XiCoroResolver *resolver) {
    if (!f)
        return false;

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (!coro_lower_func(f->children[i], resolver))
            return false;
    }

    XiCoroPlan *plan = xi_coro_analyze(f, resolver);
    if (!plan)
        return false;

    if (plan->is_coroutine) {
        /* The CFG rewrite (entry dispatch + suspend-split blocks + explicit
         * frame) is implemented incrementally on top of this scaffold. */
    }

    return true;
}

XR_FUNC bool xi_coro_lower(XiFunc *f, const XiCoroResolver *resolver) {
    if (!f)
        return false;
    if (!coro_lower_func(f, resolver))
        return false;
    xi_func_set_stage_recursive(f, XI_STAGE_CORO_LOWERED);
    return true;
}
