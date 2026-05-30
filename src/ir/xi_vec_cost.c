/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_vec_cost.c - Auto-vectorization cost model
 */

#include "xi_vec_cost.h"
#include "xi.h"

static uint32_t loop_body_value_count(const XiLoop *loop) {
    uint32_t n = 0;
    if (!loop)
        return 0;
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (blk)
            n += blk->nvalues;
    }
    return n;
}

static bool loop_body_has_call(const XiLoop *loop) {
    if (!loop)
        return true;
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_CALL || v->op == XI_CALL_METHOD || v->op == XI_CALL_METHOD_DIRECT ||
                v->op == XI_TAIL_CALL || v->op == XI_CALL_BUILTIN) {
                return true;
            }
        }
    }
    return false;
}

XR_FUNC bool xi_vec_loop_profitable(const XiLoop *loop, uint32_t vf) {
    if (!loop || vf == 0)
        return false;
    if (!loop->has_trip_count || loop->trip_count < XI_VEC_MIN_TRIP)
        return false;
    if (loop->trip_count < vf * 2)
        return false;
    if (loop->child)
        return false;
    if (loop_body_has_call(loop))
        return false;

    uint32_t body_vals = loop_body_value_count(loop);
    if (body_vals == 0 || body_vals > 64)
        return false;

    return true;
}
