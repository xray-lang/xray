/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_program_comptime.inc.c - Atomic public Program instance receiving
 *
 * KEY CONCEPT:
 *   Closed expressions replay once; genuine open calls retain symbolic bodies.
 *   Only complete owned literals and canonical bindings permit publication.
 */
#include "xxir_source_program_sites.inc.c"
#include "xxir_source_program_results.inc.c"
static bool source_program_complete(SourceContext *ctx,const XrXirArtifact *source,XrXirArtifact **output) {
    XrXirArtifact *sealed=NULL;
    if (!source_program_seal(ctx,source,&sealed)) return false;
    XirComptimeResolveRequest request={sealed,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        {XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},XR_XIR_CTFE_MAX_CALL_DEPTH};
    XirComptimeResolveFailure failure={0};XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_comptime_resolve(&request,output,&failure,&diagnostic);
    xr_xir_compile_artifact_free(sealed);
    if (status==XR_XIR_OK) return true;
    if (failure.root_local) return source_program_failure(ctx,failure.site.helper,failure.status);
    return source_fail(ctx,NULL,status,"comptime complete Source receiving failed");
}
static bool source_program_has_closed(SourceContext *ctx,bool *closed) {
    for (const SourceComptimeSite *site=ctx->comptime_sites;site;site=site->next) {
        if (!source_work_units(ctx,site->syntax,2)) return false;
        if (!ctx->generics[site->helper].parameter_count) *closed=true;
    }
    return true;
}
static void source_program_comptime_receive(SourceContext *ctx,XrXirSourceResult *output) {
    bool closed=false;
    XrXirArtifact *complete=NULL;
    bool ok=source_program_has_closed(ctx,&closed) && source_program_complete(ctx,output->checked,&complete);
    if (ok && !closed) {
        xr_xir_compile_artifact_free(output->checked);output->checked=complete;complete=NULL;
        ok=source_program_query_types(ctx,output->checked);
        if (ok) source_query_publish(ctx,output);
        if (!ok) xr_xir_compile_source_result_free(output);
        return;
    }
    if (ok) ok=source_comptime_evaluate(ctx,complete,true);
    xr_xir_compile_artifact_free(complete);complete=NULL;
    if (!ok || !source_work_units(ctx,NULL,sizeof(SourceContext))) {
        xr_xir_compile_source_result_free(output);source_comptime_values_free(ctx);return;
    }
    SourceContext received={0};received.compile=ctx->compile;received.graph=ctx->graph;received.linkage_kind=ctx->linkage_kind;
    received.manifests=ctx->manifests;received.prelude_module=ctx->prelude_module;received.core_factory=ctx->core_factory;
    received.remaining_blocks=ctx->remaining_blocks;received.remaining_instructions=ctx->remaining_instructions;
    received.comptime_input=ctx->comptime_sites;received.comptime_input_types=&ctx->types;
    received.materialization_begin=ctx->function_count;
    XrXirSourceResult result={0};source_construct(&received,&result);
    if (received.diagnostic.status==XR_XIR_OK &&
        (received.comptime_count!=ctx->comptime_count || received.comptime_matched!=ctx->comptime_count))
        source_fail(&received,NULL,XR_XIR_BAD_STRUCTURE,"comptime final occurrence coverage changed");
    ok=received.diagnostic.status==XR_XIR_OK && source_program_complete(&received,result.checked,&complete);
    if (ok) {
        xr_xir_compile_artifact_free(result.checked);result.checked=complete;complete=NULL;
        ok=source_program_query_types(&received,result.checked);
    }
    if (ok) source_query_publish(&received,&result);
    xr_xir_compile_source_result_free(output);
    if (received.diagnostic.status==XR_XIR_OK && result.checked && result.snapshot) *output=result;
    else {
        xr_xir_compile_source_result_free(&result);ctx->diagnostic=received.diagnostic;ctx->module=received.module;
        if (ctx->diagnostic.status==XR_XIR_OK) source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime final publication is incomplete");
    }
    xr_xir_compile_artifact_free(complete);source_comptime_context_free(&received);source_comptime_values_free(ctx);
}
