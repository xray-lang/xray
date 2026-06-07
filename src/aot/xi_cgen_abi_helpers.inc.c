/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_abi_helpers.inc.c - AOT function ABI and scalar boundary helpers
 */

static bool cg_func_is_module_init(const XiCgenCtx *ctx, const XiFunc *f) {
    if (!ctx || !f)
        return false;
    if (ctx->module && ctx->module->init == f)
        return true;
    for (int i = 0; i < ctx->all_nmodules; i++) {
        const XiModule *mod = ctx->all_modules ? ctx->all_modules[i] : NULL;
        if (mod && mod->init == f)
            return true;
    }
    return false;
}

static bool cg_func_has_nonlocal_exception_flow(const XiFunc *f) {
    if (!f)
        return false;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_THROW || v->op == XI_TRY || v->op == XI_END_TRY)
                return true;
        }
    }
    return false;
}

static XrRep cg_type_scalar_rep(const XrType *type) {
    if (!type)
        return XR_REP_TAGGED;
    return xaot_abi_type_can_use_typed_boundary(type) ? xaot_abi_storage_rep_for_type(type)
                                                      : XR_REP_TAGGED;
}

static bool cg_func_uses_typed_abi(XiCgenCtx *ctx, const XiFunc *f) {
    if (!f || cg_func_is_module_init(ctx, f))
        return false;
    if (f->ncaptures > 0 || cg_func_needs_aot_coro_ctx(ctx, f) ||
        cg_func_has_nonlocal_exception_flow(f))
        return false;
    if (cg_type_scalar_rep(f->return_type) == XR_REP_TAGGED)
        return false;
    return true;
}

static XrRep cg_func_return_abi_rep(XiCgenCtx *ctx, const XiFunc *f) {
    return cg_func_uses_typed_abi(ctx, f) ? cg_type_scalar_rep(f->return_type) : XR_REP_TAGGED;
}

static XrRep cg_func_param_abi_rep(XiCgenCtx *ctx, const XiFunc *f, uint16_t param_idx) {
    if (cg_class_func_uses_native_receiver(ctx, f) && param_idx > 0 && param_idx < f->nparams &&
        f->params[param_idx])
        return cg_rep(f->params[param_idx]);
    if (!cg_func_uses_typed_abi(ctx, f) || param_idx >= f->nparams || !f->params[param_idx])
        return XR_REP_TAGGED;
    return cg_rep(f->params[param_idx]);
}

static void emit_typed_abi_fname(XiCgenCtx *ctx, FILE *out, const char *prefix, const XiFunc *f) {
    emit_fname_suffix(ctx, out, prefix, f, "_boxed");
}

static void cg_prepare_func_tree_for_cgen(XiFunc *f) {
    if (!f)
        return;
    if (f->stage < XI_STAGE_REPPED) {
        XiRepPolicy policy = xi_rep_policy_native_boundary();
        xi_opt_select_rep_with_policy(f, &policy);
        xi_opt_box_elim(f);
    }
    if (f->stage < XI_STAGE_BACKEND)
        xi_backend_lower(f);
}

static bool emit_conversion_prefix(FILE *out, const XrType *type, XrRep from_rep, XrRep to_rep) {
    if (from_rep == to_rep)
        return false;
    if (to_rep == XR_REP_TAGGED) {
        if (from_rep == XR_REP_F64)
            fprintf(out, "XR_FROM_FLOAT(");
        else if (type && type->kind == XR_KIND_BOOL)
            fprintf(out, "XR_FROM_BOOL(");
        else
            fprintf(out, "XR_FROM_INT(");
        return true;
    }
    if (to_rep == XR_REP_I64) {
        if (from_rep == XR_REP_TAGGED)
            fprintf(out, "XR_TO_INT(");
        else
            fprintf(out, "(int64_t)(");
        return true;
    }
    if (to_rep == XR_REP_F64) {
        if (from_rep == XR_REP_TAGGED)
            fprintf(out, "XR_TO_FLOAT(");
        else
            fprintf(out, "(double)(");
        return true;
    }
    return false;
}

static void emit_conversion_suffix(FILE *out, bool wrapped) {
    if (wrapped)
        fprintf(out, ")");
}

static void emit_value_as_rep(FILE *out, const XiValue *v, XrRep target_rep) {
    bool wrapped = emit_conversion_prefix(out, v ? v->type : NULL, cg_rep(v), target_rep);
    emit_vref(out, v);
    emit_conversion_suffix(out, wrapped);
}
