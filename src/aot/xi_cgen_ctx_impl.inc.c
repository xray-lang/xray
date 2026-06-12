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
