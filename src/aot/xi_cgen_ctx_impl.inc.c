/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen_ctx_impl.inc.c - AOT codegen context accessors
 */

XR_FUNC XiCgenCtx *xi_cgen_ctx_new(void) {
    XiCgenCtx *ctx = (XiCgenCtx *) xr_calloc(1, sizeof(XiCgenCtx));
    if (!ctx)
        return NULL;
    ctx->shared_name = "xrt_shared";
    /* Allocate the grow-on-demand shared-slot / method / import tables at
     * their initial capacity (cg_reserve_* grow them for large modules). */
    ctx->shared_funcs = (const XiFunc **) xr_calloc(CG_INIT_SHARED, sizeof(const XiFunc *));
    ctx->shared_class =
        (const XiClassData **) xr_calloc(CG_INIT_SHARED, sizeof(const XiClassData *));
    ctx->shared_enum = (const XiEnumData **) xr_calloc(CG_INIT_SHARED, sizeof(const XiEnumData *));
    ctx->shared_native_instances =
        (CgSharedNativeInstance *) xr_calloc(CG_INIT_SHARED, sizeof(CgSharedNativeInstance));
    ctx->shared_cap = CG_INIT_SHARED;
    ctx->shared_native_exports =
        (CgSharedNativeExport *) xr_calloc(CG_INIT_SHARED, sizeof(CgSharedNativeExport));
    ctx->shared_native_exports_cap = CG_INIT_SHARED;
    ctx->methods = (CgMethodEntry *) xr_calloc(CG_INIT_METHODS, sizeof(CgMethodEntry));
    ctx->methods_cap = CG_INIT_METHODS;
    ctx->imports = (CgImportEntry *) xr_calloc(CG_INIT_IMPORTS, sizeof(CgImportEntry));
    ctx->imports_cap = CG_INIT_IMPORTS;
    if (!ctx->shared_funcs || !ctx->shared_class || !ctx->shared_enum ||
        !ctx->shared_native_instances || !ctx->shared_native_exports || !ctx->methods ||
        !ctx->imports) {
        xi_cgen_ctx_free(ctx);
        return NULL;
    }
    return ctx;
}

XR_FUNC void xi_cgen_ctx_free(XiCgenCtx *ctx) {
    if (!ctx)
        return;
    for (int i = 0; i < ctx->nstrlit; i++) {
        xr_free(ctx->strlit_list[i]->str);
        xr_free(ctx->strlit_list[i]);
    }
    xr_free(ctx->strlit_list);
    xr_free(ctx->shared_funcs);
    xr_free(ctx->shared_class);
    xr_free(ctx->shared_enum);
    xr_free(ctx->shared_native_instances);
    xr_free(ctx->shared_native_exports);
    xr_free(ctx->methods);
    xr_free(ctx->imports);
    xr_free(ctx->xmod_ref_funcs);
    xr_free(ctx->xmod_ref_prefixes);
    xr_free(ctx->cell_vars);
    xr_free(ctx->cell_origins);
    xr_free(ctx->phi_repr);
    xr_free(ctx->array_data_cache_decls);
    xr_free(ctx);
}

XR_FUNC void xi_cgen_ctx_set_aot_bundle(XiCgenCtx *ctx, const XaotBundle *bundle) {
    if (ctx)
        ctx->aot_bundle = bundle;
}

XR_FUNC bool xi_cgen_has_error(const XiCgenCtx *ctx) {
    return ctx && ctx->error;
}

XR_FUNC XiCgenCoroFrameStats xi_cgen_coro_frame_stats(const XiCgenCtx *ctx) {
    XiCgenCoroFrameStats stats = {0};
    if (ctx)
        stats = ctx->coro_frame_stats;
    return stats;
}

XR_FUNC XiCgenStats xi_cgen_stats(const XiCgenCtx *ctx) {
    XiCgenStats stats = {0};
    if (ctx)
        stats = ctx->stats;
    return stats;
}
