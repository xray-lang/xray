/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_arith_helpers.inc.c - AOT arithmetic fast-path emission helpers
 */

static bool cg_const_int_value(const XiValue *value, int64_t *out) {
    const XiValue *v = cg_unwrap_identity_value(value);
    if (!v || v->op != XI_CONST || !v->type || v->type->kind != XR_KIND_INT || !out)
        return false;
    *out = v->aux_int;
    return true;
}

static bool emit_native_const_div_mod_expr(FILE *out, const XiValue *v) {
    if (!v || v->nargs < 2 || cg_rep(v) != XR_REP_I64 || cg_rep(v->args[0]) != XR_REP_I64 ||
        cg_rep(v->args[1]) != XR_REP_I64)
        return false;

    int64_t divisor = 0;
    if (!cg_const_int_value(v->args[1], &divisor) || divisor == 0)
        return false;

    if (v->op == XI_DIV) {
        if (divisor == -1) {
            fprintf(out, "(int64_t)(-(uint64_t)");
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
        } else {
            emit_binop(out, v, "/");
        }
        return true;
    }
    if (v->op == XI_MOD) {
        if (divisor == -1) {
            fprintf(out, "INT64_C(0)");
        } else {
            emit_binop(out, v, "%");
        }
        return true;
    }
    return false;
}

static bool emit_native_i64_wrap_arith_expr(FILE *out, const XiValue *v) {
    if (!out || !v || v->nargs < 2 || cg_rep(v) != XR_REP_I64 || cg_rep(v->args[0]) != XR_REP_I64 ||
        cg_rep(v->args[1]) != XR_REP_I64)
        return false;

    const char *op = NULL;
    switch (v->op) {
        case XI_ADD:
            op = "+";
            break;
        case XI_SUB:
            op = "-";
            break;
        case XI_MUL:
            op = "*";
            break;
        default:
            return false;
    }

    fprintf(out, "(int64_t)((uint64_t)(");
    emit_vref(out, v->args[0]);
    fprintf(out, ") %s (uint64_t)(", op);
    emit_vref(out, v->args[1]);
    fprintf(out, "))");
    return true;
}
