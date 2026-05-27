/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_strength.c - Strength reduction for Xi IR
 */

#include "xi_opt_strength.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype.h"

/* ========== Helpers ========== */

static bool is_const_int(const XiValue *v) {
    return v && v->op == XI_CONST && v->type && v->type->kind == XR_KIND_INT;
}

static bool is_zero(const XiValue *v) {
    return is_const_int(v) && v->aux_int == 0;
}

static bool is_one(const XiValue *v) {
    return is_const_int(v) && v->aux_int == 1;
}

static bool is_numeric(const XiValue *v) {
    return v && v->type && v->type->kind != XR_KIND_STRING;
}

/* Rewrite `v` as a COPY of `src`. Caller ensures src is dominating. */
static void rewrite_to_copy(XiValue *v, XiValue *src) {
    v->op = XI_COPY;
    v->args[0] = src;
    v->nargs = 1;
}

/* Rewrite `v` as an integer constant zero. */
static void rewrite_to_zero(XiValue *v) {
    v->op = XI_CONST;
    v->aux_int = 0;
    v->nargs = 0;
}

/* ========== Per-Op Reducers ==========
 *
 * Each helper returns true when it rewrote `v`.  Drivers add the
 * change flag once at the call site to keep the main loop linear. */

static bool reduce_add(XiValue *v, XiValue *lhs, XiValue *rhs) {
    if (is_zero(rhs)) {
        rewrite_to_copy(v, lhs);
        return true;
    }
    if (is_zero(lhs)) {
        rewrite_to_copy(v, rhs);
        return true;
    }
    return false;
}

static bool reduce_sub(XiValue *v, XiValue *lhs, XiValue *rhs) {
    if (is_zero(rhs)) {
        rewrite_to_copy(v, lhs);
        return true;
    }
    if (lhs == rhs) {
        rewrite_to_zero(v);
        return true;
    }
    return false;
}

static bool reduce_mul(XiValue *v, XiValue *lhs, XiValue *rhs) {
    /* x * 0 / 0 * x: numeric only — string * 0 must produce "" via runtime. */
    if ((is_zero(rhs) || is_zero(lhs)) && is_numeric(lhs) && is_numeric(rhs)) {
        rewrite_to_zero(v);
        return true;
    }
    if (is_one(rhs)) {
        rewrite_to_copy(v, lhs);
        return true;
    }
    if (is_one(lhs)) {
        rewrite_to_copy(v, rhs);
        return true;
    }
    return false;
}

static bool reduce_div(XiValue *v, XiValue *lhs, XiValue *rhs) {
    if (is_one(rhs)) {
        rewrite_to_copy(v, lhs);
        return true;
    }
    return false;
}

static bool reduce_band(XiValue *v, XiValue *lhs, XiValue *rhs) {
    if (is_zero(rhs) || is_zero(lhs)) {
        rewrite_to_zero(v);
        return true;
    }
    if (lhs == rhs) {
        rewrite_to_copy(v, lhs);
        return true;
    }
    return false;
}

static bool reduce_bor(XiValue *v, XiValue *lhs, XiValue *rhs) {
    if (is_zero(rhs) || lhs == rhs) {
        rewrite_to_copy(v, lhs);
        return true;
    }
    if (is_zero(lhs)) {
        rewrite_to_copy(v, rhs);
        return true;
    }
    return false;
}

static bool reduce_bxor(XiValue *v, XiValue *lhs, XiValue *rhs) {
    if (is_zero(rhs)) {
        rewrite_to_copy(v, lhs);
        return true;
    }
    if (is_zero(lhs)) {
        rewrite_to_copy(v, rhs);
        return true;
    }
    if (lhs == rhs) {
        rewrite_to_zero(v);
        return true;
    }
    return false;
}

static bool reduce_shift(XiValue *v, XiValue *lhs, XiValue *rhs) {
    if (is_zero(rhs)) {
        rewrite_to_copy(v, lhs);
        return true;
    }
    return false;
}

/* Dispatch a single value through the reducer for its op.
 * Returns true if `v` was rewritten. */
static bool reduce_value(XiValue *v) {
    if (v->nargs != 2)
        return false;
    XiValue *lhs = v->args[0];
    XiValue *rhs = v->args[1];
    switch (v->op) {
        case XI_ADD:
            return reduce_add(v, lhs, rhs);
        case XI_SUB:
            return reduce_sub(v, lhs, rhs);
        case XI_MUL:
            return reduce_mul(v, lhs, rhs);
        case XI_DIV:
            return reduce_div(v, lhs, rhs);
        case XI_BAND:
            return reduce_band(v, lhs, rhs);
        case XI_BOR:
            return reduce_bor(v, lhs, rhs);
        case XI_BXOR:
            return reduce_bxor(v, lhs, rhs);
        case XI_SHL:
        case XI_SHR:
            return reduce_shift(v, lhs, rhs);
        default:
            return false;
    }
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_strength_reduce(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_strength_reduce: NULL func");
    XiPassChange chg = xi_pass_no_change();

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (reduce_value(blk->values[i]))
                chg.values_changed = true;
        }
    }
    return chg;
}
