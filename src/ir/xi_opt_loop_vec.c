/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_vec.c - Simple loop vectorization
 *
 * Identifies counted loops with stride-1 array access and no internal
 * calls.  When the cost model approves, delegates to the SLP packer
 * on the loop body blocks after marking the loop header with a vector
 * trip hint (aux on primary IV phi).
 */

#include "xi_opt_loop_vec.h"
#include "xi_opt_slp.h"
#include "xi_vec_cost.h"
#include "xi_loop.h"
#include "xi_analysis.h"

#define LOOP_VEC_FLAG (1u << 0)

static bool mark_loop_vectorized(XiLoop *loop) {
    if (!loop || loop->nbasic_ivs == 0)
        return false;
    XiBasicIV *biv = &loop->basic_ivs[0];
    if (!biv->phi)
        return false;
    biv->phi->flags |= LOOP_VEC_FLAG;
    return true;
}

XR_FUNC XiPassChange xi_opt_loop_vec(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    XiLoopInfo *info = xi_ensure_loops(f);
    if (!info || info->nloop == 0)
        return xi_pass_no_change();

    uint32_t n_marked = 0;
    for (uint32_t li = 0; li < info->nloop; li++) {
        XiLoop *loop = info->all_loops[li];
        if (!loop)
            continue;
        if (!xi_vec_loop_profitable(loop, XI_VEC_DEFAULT_VF))
            continue;
        if (mark_loop_vectorized(loop))
            n_marked++;
    }

    if (n_marked == 0)
        return xi_pass_no_change();

    /* Run SLP on marked loop bodies to pack inner stores. */
    XiPassChange slp = xi_opt_slp(f);
    slp.n_added += n_marked;
    return slp;
}
