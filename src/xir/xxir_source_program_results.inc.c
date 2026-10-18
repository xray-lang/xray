/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_program_results.inc.c - Real closed calls supply owned literal data
 *
 * KEY CONCEPT:
 *   Exact failed selections retain their source site without executing again.
 *   Zero actual instances need an inventory, never a fabricated execution.
 */
static bool source_program_results(SourceContext *ctx,const XrXirArtifact *source,SourceProgramComptime *job) {
    XrXirArtifact *closed=NULL;XrXirStatus status=xr_xir_compile_specialize(source,&closed,NULL);
    if (status!=XR_XIR_OK) return source_fail(ctx,NULL,status,"comptime ordinary instance collection failed");
    bool ok=source_comptime_instances_collect(ctx,closed,NULL,NULL,&job->count);
    uint32_t *functions=ok && job->count?source_alloc(ctx,job->count,sizeof(*functions)):NULL;
    job->actual=ok && job->count?source_alloc(ctx,job->count,sizeof(*job->actual)):NULL;
    if (job->count && (!functions || !job->actual)) ok=false;
    uint32_t collected=0;
    if (ok) ok=source_comptime_instances_collect(ctx,closed,job->actual,functions,&collected);
    if (ok && collected!=job->count) ok=source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime actual call coverage changed");
    if (ok && job->count) {
        XirCtfeInstanceBatchRequest request={closed,functions,job->count,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
            {XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},XR_XIR_CTFE_MAX_CALL_DEPTH};
        uint32_t failed=UINT32_MAX;XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_instance_batch_at(&request,&job->batch,&failed);
        if (evaluated!=XR_XIR_CTFE_OK) ok=source_program_failure(ctx,
            job->actual[failed<job->count?failed:0].helper,evaluated);
    }
    xr_xir_compile_artifact_free(closed);
    if (!ok || !job->count) return ok;
    XirCtfeInstanceView *views=source_alloc(ctx,job->count,sizeof(*views));
    if (!views) return false;
    XrXirCtfeStatus queried=xr_xir_compile_ctfe_instance_batch_views(job->batch,views,job->count);
    if (queried!=XR_XIR_CTFE_OK) return source_program_failure(ctx,job->actual[0].helper,queried);
    for (uint32_t i=0;i<job->count;++i) {
        queried=source_comptime_binding_verify(&ctx->compile,&views[i],&job->actual[i]);
        if (queried!=XR_XIR_CTFE_OK) return source_program_failure(ctx,job->actual[i].helper,queried);
    }
    XirLiteralRequest request={&ctx->compile,job->batch,job->count};
    status=xr_xir_compile_instance_literals(&request,&job->literals,NULL);
    return status==XR_XIR_OK || source_fail(ctx,NULL,status,"comptime owned literal receiving failed");
}
static bool source_program_seal(SourceContext *ctx,const XrXirArtifact *source,
    const SourceProgramComptime *job,XrXirArtifact **output) {
    const XrXirArtifact *definition=job->count?job->literals.definition:source;
    XrXirComptimeTable table={0};
    if (!source_program_sites(ctx,&definition->module,&table) ||
        !source_program_rows(ctx,job,definition->module.types,&table)) return false;
    const XrXirConstruction *original=definition->construction;
    if (original->comptime.site_count || original->comptime.binding_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime provisional definition already has bindings");
    XrXirConstructionSpec input={original->rows,original->count,original->materializations,original->materialization_count,&table};
    XrXirConstruction *construction=NULL;
    XrXirStatus status=xr_xir_compile_construction_new_v3(&ctx->compile,definition->module.types,&input,&construction);
    if (status==XR_XIR_OK) {
        XrXirModule built=definition->module;built.stage=XR_XIR_BUILT;
        XrXirDiagnostic diagnostic={0};status=xr_xir_compile_check_v2(&ctx->compile,&built,construction,output,&diagnostic);
        if (status!=XR_XIR_OK) {
            const SourceComptimeSite *site=source_program_syntax(ctx,diagnostic.function);
            if (site) ctx->module=site->module;
            source_fail(ctx,site?site->syntax:NULL,status,"comptime final site binding failed complete checking");
        }
    } else source_fail(ctx,NULL,status,"comptime final site binding ownership failed");
    xr_xir_compile_construction_free(construction);return status==XR_XIR_OK;
}
/* Query-only display types may be appended afterward. Their earlier IDs are
 * the unchanged original prefix, while the final construction owns new keys. */
static bool source_program_query_types(SourceContext *ctx,const XrXirArtifact *checked) {
    const XrXirTypes *types=checked->module.types;
    if (!types) return true;
    if (types->count<ctx->types.count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime receiving lost original query types");
    XrXirTypeNode *nodes=types->count?source_alloc(ctx,types->count,sizeof(*nodes)):NULL;
    if ((types->count && !nodes) || !source_copy_bytes(ctx,NULL,nodes,types->nodes,(size_t)types->count*sizeof(*nodes))) return false;
    ctx->types=*types;ctx->types.nodes=nodes;ctx->type_capacity=types->count;
    if (types->nominals) { ctx->nominals=*types->nominals;ctx->types.nominals=&ctx->nominals; }
    if (types->interfaces) { ctx->interfaces=*types->interfaces;ctx->types.interfaces=&ctx->interfaces; }
    return true;
}
