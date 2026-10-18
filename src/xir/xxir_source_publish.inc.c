/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_publish.inc.c - Publish independently owned query observations
 */
/* Graph declarations have birth-qualified identities. Publish those exact
 * names; appended native/core observations retain their governed origins.
 * Resolver canonicals and physical paths remain graph facts. */
static bool source_query_module_identities(SourceContext *ctx,const XrXirArtifact *checked) {
    if(!ctx->module_scope)return true;
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    if(!ctx->graph || ctx->graph->spec_count<0 || (uint32_t)ctx->graph->spec_count>ctx->module_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"interactive query graph inventory diverged");
    uint32_t count=(uint32_t)ctx->graph->spec_count;
    if(!module || !module->declarations || module->declarations->module_count!=ctx->module_count ||
        ctx->query.module_count<ctx->module_count || (ctx->module_count &&
        (!ctx->modules || !module->declarations->modules || !ctx->query.modules)))
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"interactive query module inventory diverged");
    XrXirSourceQueryModule *queries=(XrXirSourceQueryModule *)ctx->query.modules;
    for(uint32_t m=0;m<count;++m) {
        const XrXirSourceModule *source=&ctx->modules[m];
        const XrXirSourceModule *owner=&module->declarations->modules[m];
        if(!source_work_units(ctx,NULL,sizeof(*queries)+(uint64_t)source->name_length+sizeof(queries[m].identity)))
            return false;
        if(!source->name || !owner->name || source->name_length!=owner->name_length ||
            memcmp(source->name,owner->name,source->name_length))
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"interactive query module identity diverged");
        queries[m].identity=source->name;
    }
    return true;
}
static void source_query_publish(SourceContext *ctx, XrXirSourceResult *output) {
    if (!output || !ctx->query_ready || ctx->diagnostic.status != XR_XIR_OK) {
        if (output) xr_xir_compile_source_result_free(output);
        return;
    }
    source_release_retired_arrays(ctx);
    ctx->query.complete = ctx->diagnostic.status == XR_XIR_OK;
    ctx->query.implementations = ctx->implementations_ready && ctx->implementations.count ? &ctx->implementations : NULL;
    ctx->query.diagnostic = ctx->diagnostic;
    ctx->query.types = ctx->types.count || ctx->types.nominals || ctx->types.interfaces ? &ctx->types : NULL;
    if(!source_native_query_types(ctx) || !source_hash_query_types(ctx)){xr_xir_compile_source_result_free(output);return;}
    if(!source_display_names_publish(ctx) || !source_query_module_identities(ctx,output->checked)){
        xr_xir_compile_source_result_free(output);return;
    }
    const char *failure = "source query snapshot publication failed";
    XrXirStatus status = xr_xir_compile_source_snapshot_copy_v2(&ctx->compile, &ctx->query, xr_xir_compile_artifact_construction(output->checked), &output->snapshot);
    if (status == XR_XIR_OK) {
        failure = "source syntax snapshot publication failed";
        status = xr_xir_compile_source_snapshot_syntax_copy(output->snapshot, &ctx->syntax);
    }
    if (status == XR_XIR_OK) {
        failure = "source call snapshot publication failed";
        status = source_calls_publish(ctx, output->snapshot);
    }
    if (status == XR_XIR_OK) {
        failure = "source debug snapshot publication failed";
        status = source_debug_publish(ctx, output);
    }
    if (status == XR_XIR_OK) {
        failure = "source dependency snapshot publication failed";
        status = source_dependencies_publish(ctx, output->snapshot);
    }
    if (status == XR_XIR_OK) {
        failure = "source type snapshot publication failed";
        status = source_type_facts_publish(ctx, output->snapshot);
    }
    if (status==XR_XIR_OK && ctx->semantic_output && !source_semantic_capture(ctx,ctx->semantic_output)) {
        xr_xir_compile_source_result_free(output);return;
    }
    if (status != XR_XIR_OK) {
        xr_xir_compile_source_result_free(output); memset(&ctx->diagnostic, 0, sizeof(ctx->diagnostic));
        source_fail(ctx, NULL, status, failure);
    }
}
