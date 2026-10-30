/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_instance_pipeline.inc.c - Real instance evaluation and fresh replay
 */
static bool source_instance_selected(SourceContext *ctx,const XrXirArtifact *closed,
    const XirSourceInstanceSelection *selection,uint32_t *output) {
    const XrXirModule *module=&closed->module;const XrXirProvenance *proof=module->provenance;
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (!proof || !xir_effect_kind_is_closed(proof->kind) || !proof->source || !proof->origins ||
        proof->count!=module->function_count || selection->definition>=proof->source->module.function_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"instance selection has no actual generic definition");
    if (selection->function>=module->function_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_VALUE,"ordinary specialization did not collect the selected instance");
    const XrXirOrigin *origin=&proof->origins[selection->function];
    if (origin->function!=selection->definition || !origin->argument_count || !origin->arguments)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"selected instance changed its genuine definition or substitution");
    *output=selection->function;return true;
}
static void source_instance_receive(SourceContext *ctx,XrXirSourceResult *output,
    const XirSourceInstanceRequest *request,uint32_t *providers) {
    if (ctx->diagnostic.status!=XR_XIR_OK) return;
    if (ctx->comptime_count || ctx->linkage_kind!=XR_XIR_PROGRAM) {
        source_fail(ctx,NULL,XR_XIR_BAD_TYPE,"instance constant receiving requires an ordinary Program Source graph");return;
    }
    XrXirStatus status=XR_XIR_OK;uint32_t count=request->count;
    uint32_t *functions=xir_type_copy_zero(&ctx->compile,count,sizeof(*functions),&status,ctx->request);
    XirCtfeInstanceView *views=status==XR_XIR_OK?xir_type_copy_zero(&ctx->compile,count,sizeof(*views),&status,ctx->request):NULL;
    XrXirArtifact *closed=NULL;XirCtfeInstanceBatch *owner=NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(output->checked,&closed,NULL,ctx->request);
    if (status!=XR_XIR_OK) source_fail(ctx,NULL,status,"instance constant preparation failed");
    uint32_t progress=0;
    for (uint32_t i=0;ctx->diagnostic.status==XR_XIR_OK && i<count;++i) {
        if (!source_receive_step(ctx,&progress)) break;
        if (!source_instance_selected(ctx,closed,&request->selections[i],&functions[i])) break;
    }
    if (ctx->diagnostic.status==XR_XIR_OK && ((ctx)->diagnostic.status == XR_XIR_OK)) {
        XirCtfeInstanceBatchRequest evaluation={closed,functions,count,request->target,request->limits,request->call_depth};
        XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_instance_batch(&evaluation,ctx->request,&owner);
        if (evaluated==XR_XIR_CTFE_OK) evaluated=xr_xir_compile_ctfe_instance_batch_views(owner,views,count,ctx->request);
        if (evaluated!=XR_XIR_CTFE_OK) source_comptime_failure(ctx,NULL,evaluated);
    }
    if (ctx->diagnostic.status==XR_XIR_OK && ((ctx)->diagnostic.status == XR_XIR_OK)) {
        SourceInstanceBatch batch={views,providers,count};SourceContext received={0};
        received.request=ctx->request;received.compile=ctx->compile;received.graph=ctx->graph;received.linkage_kind=ctx->linkage_kind;
        received.manifests=ctx->manifests;received.prelude_module=ctx->prelude_module;received.core_factory=ctx->core_factory;
        received.remaining_blocks=ctx->remaining_blocks;received.remaining_instructions=ctx->remaining_instructions;
        received.instance_constants=&batch;
        XrXirSourceResult result={0};source_construct(&received,&result);
        if (received.diagnostic.status==XR_XIR_OK) source_query_publish(&received,&result);
        xr_xir_compile_source_result_free(output);
        if (received.diagnostic.status==XR_XIR_OK) *output=result;
        else { xr_xir_compile_source_result_free(&result);ctx->diagnostic=received.diagnostic;ctx->module=received.module; }
        source_comptime_context_free(&received,ctx);
    }
    xr_xir_compile_ctfe_instance_batch_free(owner);
    xr_compile_resources_free(views);xr_compile_resources_free(functions);xr_xir_compile_artifact_free(closed);
}
