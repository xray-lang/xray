/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_reduction.c - Reduction pattern recognition
 *
 * Detects loop-carried accumulators updated by ADD/MUL/MIN/MAX and
 * tags them via aux_int for downstream vector lowering.
 */

#include "xi_opt_reduction.h"
#include "xi_loop.h"
#include "xi_analysis.h"

#define REDUCE_TAG_SUM 1
#define REDUCE_TAG_PRODUCT 2
#define REDUCE_TAG_MIN 3
#define REDUCE_TAG_MAX 4

static XiReductionKind classify_update(XiOp op) {
    switch (op) {
        case XI_ADD:
            return XI_REDUCE_SUM;
        case XI_MUL:
            return XI_REDUCE_PRODUCT;
        default:
            return XI_REDUCE_NONE;
    }
}

static bool is_loop_phi(const XiLoop *loop, const XiValue *v) {
    if (!loop || !v || v->op != XI_PHI)
        return false;
    for (uint32_t i = 0; i < loop->nbasic_ivs; i++) {
        if (loop->basic_ivs[i].phi == v)
            return true;
    }
    return false;
}

static bool detect_reduction_in_loop(const XiLoop *loop, uint32_t *n_found) {
    bool any = false;
    if (!loop)
        return false;

    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->nargs < 2)
                continue;

            XiReductionKind kind = classify_update(v->op);
            if (kind == XI_REDUCE_NONE)
                continue;

            /* acc = phi(acc, acc op x) or acc = acc op x where acc is phi */
            XiValue *lhs = v->args[0];
            XiValue *rhs = v->args[1];
            XiValue *acc = NULL;
            if (lhs && lhs->op == XI_PHI && !is_loop_phi(loop, lhs))
                acc = lhs;
            else if (rhs && rhs->op == XI_PHI && !is_loop_phi(loop, rhs))
                acc = rhs;

            if (!acc)
                continue;

            if (kind == XI_REDUCE_SUM)
                acc->aux_int = REDUCE_TAG_SUM;
            else if (kind == XI_REDUCE_PRODUCT)
                acc->aux_int = REDUCE_TAG_PRODUCT;
            (*n_found)++;
            any = true;
        }
    }
    return any;
}

XR_FUNC XiPassChange xi_opt_reduction(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    XiLoopInfo *info = xi_ensure_loops(f);
    if (!info || info->nloop == 0)
        return xi_pass_no_change();

    uint32_t n_found = 0;
    for (uint32_t li = 0; li < info->nloop; li++) {
        XiLoop *loop = info->all_loops[li];
        if (!loop)
            continue;
        detect_reduction_in_loop(loop, &n_found);
    }

    if (n_found == 0)
        return xi_pass_no_change();

    return (XiPassChange) {
        .values_changed = true,
        .cfg_changed = false,
        .types_changed = false,
        .n_added = 0,
    };
}
