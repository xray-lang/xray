/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_construct.inc.c - Ordinary Built checking for every source origin
 */
/* Dense construction facts freeze before Built enters the common checker.
 * The producer's bookkeeping supplies bindings, not independent authority. */
static bool source_construction_prepare(SourceContext *ctx) {
    if (ctx->construction)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source construction owner was already published");
    uint32_t count=ctx->nominals.count;
    XrXirConstructionRow *rows=count ? source_alloc(ctx,count,sizeof(*rows)) : NULL;
    if (count && !rows) return false;
    for (uint32_t n=0; n<count; ++n) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirNominalDeclaration *nominal=&ctx->nominals.declarations[n];
        rows[n].field_count=nominal->field_count;
        uint32_t *fields=nominal->field_count ? source_alloc(ctx,nominal->field_count,sizeof(*fields)) : NULL;
        if (nominal->field_count && !fields) return false;
        rows[n].field_initializers=fields;
        if (ctx->nominal_defaultable[n]) {
            uint32_t function=ctx->nominal_constructors[n];
            if (!function || function>=ctx->function_count)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source implicit construction target is missing");
            rows[n].default_initializer=function+1;
        }
        for (uint32_t f=0; f<nominal->field_count; ++f) {
            if (!source_work(ctx,NULL)) return false;
            uint32_t function=ctx->nominal_defaults[n] ? ctx->nominal_defaults[n][f] : 0;
            if (function>=ctx->function_count)
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source field construction target is invalid");
            fields[f]=function ? function+1 : 0;
        }
    }
    XrXirStatus status=xr_xir_compile_construction_new(&ctx->compile,&ctx->types,rows,count,&ctx->construction);
    return status==XR_XIR_OK || source_fail(ctx,NULL,status,"source construction ownership failed");
}
static void source_construct(SourceContext *ctx, XrXirSourceResult *output) {
    ctx->query_ready = true;
    ctx->declarations_building = true;
    bool declarations_ready = source_query_modules(ctx) && collect_declarations(ctx);
    ctx->declarations_building = false;
    if (declarations_ready && (!ctx->core_factory || source_core_contract(ctx)) && source_manifests_bind(ctx) &&
        source_implementations_bind(ctx) && build_bodies(ctx) && source_class_carriers(ctx) && source_effect_prepare(ctx) && source_construction_prepare(ctx)) {
        XrXirDeclarations declarations;
        XrXirModule built = source_module_view(ctx,&declarations);
        XrXirDiagnostic location = {0};
        XrXirRootRefiner refiner = {ctx,source_root_refine};
        XrXirStatus status = xr_xir_compile_check_refined_v2(&ctx->compile, &built, ctx->construction, &refiner, &output->checked, &location);
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
            const char *cause = location.reason == XR_XIR_DIAGNOSTIC_GO_ROOT_REQUIRED ?
                "GO target requires the current instance root execution" :
                location.reason == XR_XIR_DIAGNOSTIC_GO_ROOT_UNRESOLVED ?
                "GO target lacks proof for worker execution" :
                location.reason == XR_XIR_DIAGNOSTIC_NO_SUSPEND ?
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
