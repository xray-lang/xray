/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_optional.inc.c - Closed optional injection in retained C emission
 *
 * KEY CONCEPT:
 *   The verified injection owns the payload transfer. C emission only projects
 *   its None/Some distinction into the immutable value plan's storage.
 */

static void xicgen_sum_inject(XiCgenCtx *ctx, FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    (void) prefix;
    if (!v || !v->type || !v->type->is_nullable || (v->aux_int != 0 && v->aux_int != 1) ||
        (v->aux_int == 0 && v->nargs != 0) ||
        (v->aux_int == 1 && (v->nargs != 1 || !v->args || !v->args[0]))) {
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    XrRep result_rep = xicgen_value_c_storage_rep(ctx, f, v);
    if (result_rep != XR_REP_TAGGED && result_rep != XR_REP_PTR) {
        ctx->error = true;
        emit_codegen_abort_expr(out);
        return;
    }
    if (v->aux_int == 0) {
        fprintf(out, "%s", result_rep == XR_REP_PTR ? "NULL" : "XR_NULL_VAL");
        return;
    }
    emit_value_as_rep_ctx(ctx, out, v->args[0], result_rep);
}
