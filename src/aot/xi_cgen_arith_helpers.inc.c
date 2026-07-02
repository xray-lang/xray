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
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, " / INT64_C(%" PRId64 "))", divisor);
        }
        return true;
    }
    if (v->op == XI_MOD) {
        if (divisor == -1) {
            fprintf(out, "INT64_C(0)");
        } else {
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, " %% INT64_C(%" PRId64 "))", divisor);
        }
        return true;
    }
    return false;
}

static bool emit_native_positive_divisor_div_mod_expr(FILE *out, const XiFunc *f,
                                                      const XiValue *v) {
    if (!v || v->nargs < 2 || cg_rep(v) != XR_REP_I64 || cg_rep(v->args[0]) != XR_REP_I64 ||
        cg_rep(v->args[1]) != XR_REP_I64)
        return false;
    if (v->op != XI_DIV && v->op != XI_MOD)
        return false;
    if (!xi_value_known_positive_at(f, v->args[1], v->block))
        return false;

    fprintf(out, "(");
    emit_vref(out, v->args[0]);
    fprintf(out, v->op == XI_DIV ? " / " : " %% ");
    emit_vref(out, v->args[1]);
    fprintf(out, ")");
    return true;
}

static bool cg_div_mod_is_trusted_nothrow(const XiFunc *f, const XiValue *v) {
    if (!v || (v->op != XI_DIV && v->op != XI_MOD) || v->nargs < 2 || cg_rep(v) != XR_REP_I64 ||
        cg_rep(v->args[0]) != XR_REP_I64 || cg_rep(v->args[1]) != XR_REP_I64)
        return false;

    int64_t divisor = 0;
    if (cg_const_int_value(v->args[1], &divisor))
        return divisor != 0;

    return xi_value_known_positive_at(f, v->args[1], v->block);
}

static bool emit_native_unsigned_wrap_arith_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!ctx || !out || !v || v->nargs < 2 || cg_rep(v) != XR_REP_I64 ||
        cg_rep(v->args[0]) != XR_REP_I64 || cg_rep(v->args[1]) != XR_REP_I64)
        return false;
    /* For unsigned result storage, uint64 arithmetic followed by the normal C
     * conversion to uint8/16/32/64 preserves the same low bits as Xray's i64
     * wrap path, even when an inlined argument originated as an int constant. */
    if (!cg_type_is_unsigned_int(v->type))
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

    fprintf(out, "((uint64_t)(");
    emit_value_as_rep_ctx(ctx, out, v->args[0], XR_REP_I64);
    fprintf(out, ") %s (uint64_t)(", op);
    emit_value_as_rep_ctx(ctx, out, v->args[1], XR_REP_I64);
    fprintf(out, "))");
    return true;
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

static bool emit_native_rawptr_arith_expr(XiCgenCtx *ctx, FILE *out, const XiValue *v) {
    if (!out || !v || v->nargs < 2 || cg_rep(v) != XR_REP_RAWPTR)
        return false;
    if (v->op != XI_ADD && v->op != XI_SUB)
        return false;

    const XiValue *ptr = NULL;
    const XiValue *offset = NULL;
    bool subtract = v->op == XI_SUB;
    if (cg_rep(v->args[0]) == XR_REP_RAWPTR && cg_rep(v->args[1]) == XR_REP_I64) {
        ptr = v->args[0];
        offset = v->args[1];
    } else if (v->op == XI_ADD && cg_rep(v->args[0]) == XR_REP_I64 &&
               cg_rep(v->args[1]) == XR_REP_RAWPTR) {
        ptr = v->args[1];
        offset = v->args[0];
    } else {
        return false;
    }

    fprintf(out, "(void *)((uint8_t *)(");
    emit_value_as_rep_ctx(ctx, out, ptr, XR_REP_RAWPTR);
    fprintf(out, ") %c (intptr_t)(", subtract ? '-' : '+');
    emit_value_as_rep_ctx(ctx, out, offset, XR_REP_I64);
    fprintf(out, "))");
    return true;
}
