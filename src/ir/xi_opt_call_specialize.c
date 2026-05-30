/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_call_specialize.c - Call-site constant specialization
 *
 * Marks call sites where arguments are compile-time constants so
 * downstream inlining can treat them as specialized entry points.
 */

#include "xi_opt_call_specialize.h"

#define CALL_SPEC_FLAG (1u << 6)

XR_FUNC XiPassChange xi_opt_call_specialize(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();

    uint32_t n_spec = 0;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || (v->op != XI_CALL && v->op != XI_CALL_METHOD))
                continue;

            bool all_const_args = true;
            for (uint16_t a = 1; a < v->nargs; a++) {
                XiValue *arg = v->args[a];
                if (!arg || arg->op != XI_CONST) {
                    all_const_args = false;
                    break;
                }
            }

            if (all_const_args && v->nargs > 1) {
                v->flags |= CALL_SPEC_FLAG;
                n_spec++;
            }
        }
    }

    if (n_spec == 0)
        return xi_pass_no_change();

    return (XiPassChange) {
        .values_changed = false,
        .types_changed = false,
        .n_added = 0,
    };
}
