/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_construct.inc.c - Ordinary Built checking for every source origin
 */
static void source_construct(SourceContext *ctx, XrXirSourceResult *output) {
    ctx->query_ready = true;
    ctx->declarations_building = true;
    bool declarations_ready = source_query_modules(ctx) && collect_declarations(ctx);
    ctx->declarations_building = false;
    if (declarations_ready && (!ctx->core_factory || source_core_contract(ctx)) && source_manifests_bind(ctx) &&
        source_implementations_bind(ctx) && build_bodies(ctx) && source_class_carriers(ctx)) {
        XrXirDeclarations declarations;
        XrXirModule built = source_module_view(ctx,&declarations);
        XrXirDiagnostic location = {0};
        XrXirStatus status = xr_xir_compile_check(&ctx->compile, &built, &output->checked, &location);
        for (uint32_t f=0;status==XR_XIR_OK && f<ctx->function_count;++f) {
            if (!ctx->bodies[f].initialization_regions) continue;
            location.function=f;
            status=xr_xir_compile_initialization_check(&ctx->compile, &built, f, ctx->bodies[f].initialization_regions, &location);
        }
        if (status != XR_XIR_OK) {
            xr_xir_compile_artifact_free(output->checked); output->checked=NULL;
            char message[128] = {0};
            if (status != XR_XIR_BUDGET && status != XR_XIR_OUT_OF_MEMORY &&
                source_format(ctx, message, sizeof(message),
                    "constructed XIR failed checking at function %u block %u instruction %u",
                    location.function, location.block, location.instruction) != XR_DIAG_OK)
                status = XR_XIR_BUDGET;
            AstNode *site = NULL;
            if (location.function < ctx->function_count) {
                site = ctx->bodies[location.function].node;
                ctx->module = ctx->bodies[location.function].module;
            }
            const char *cause = location.reason == XR_XIR_DIAGNOSTIC_NO_SUSPEND ?
                "declared no_suspend function may suspend or call an unqualified callable" :
                location.reason == XR_XIR_DIAGNOSTIC_CLEANUP_THROW ?
                "E0387: an error can escape the defer body" :
                location.reason == XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND ?
                "E0392: defer may suspend or create a task" :
                location.reason == XR_XIR_DIAGNOSTIC_UNINITIALIZED_READ ?
                "read requires storage initialized on every incoming path" :
                location.reason == XR_XIR_DIAGNOSTIC_READONLY_WRITE ?
                "write may overwrite already initialized const storage" : message;
            source_fail(ctx, site, status, cause);
        }
    }
}
