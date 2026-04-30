/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_rep.h - Shared representation helpers for Xi IR
 *
 * Determines the machine representation (I64/F64/TAGGED) a value
 * naturally produces. Used by both select_rep (xi_opt.c) and C
 * code generation (xi_cgen.c) to avoid duplicated logic.
 */

#ifndef XI_REP_H
#define XI_REP_H

#include "xi.h"
#include "../runtime/value/xtype.h"

/* Determine the machine representation a value naturally produces.
 * Constants and arithmetic with known numeric types produce I64/F64.
 * Calls, loads, parameters, and polymorphic ops produce TAGGED. */
static inline XrRep xi_value_def_rep(const XiValue *v) {
    if (!v || !v->type) return XR_REP_TAGGED;
    switch (v->op) {
        case XI_CONST: {
            if (v->type->kind == XR_KIND_NULL ||
                v->type->kind == XR_KIND_STRING)
                return XR_REP_TAGGED;
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
        }
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_NEG:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_BNOT:
        case XI_SHL: case XI_SHR: {
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
        }
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE:
        case XI_NOT: case XI_ISNULL: case XI_IS:
            return XR_REP_I64;
        case XI_BOX:
            return XR_REP_TAGGED;
        case XI_UNBOX:
            return xr_type_base_rep(v->type);
        case XI_CONVERT: {
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
        }
        default:
            return XR_REP_TAGGED;
    }
}

#endif  // XI_REP_H
