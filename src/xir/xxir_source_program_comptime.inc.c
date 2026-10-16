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
typedef struct SourceProgramComptime {
    XirCtfeInstanceBatch *batch;
    XirLiteralDefinitions literals;
    XirSourceComptimeBinding *actual;
    uint32_t count;
} SourceProgramComptime;
#include "xxir_source_program_sites.inc.c"
#include "xxir_source_program_results.inc.c"
static void source_program_comptime_receive(SourceContext *ctx,XrXirSourceResult *output) {
    if (!source_comptime_evaluate(ctx,output->checked,true)) {
        xr_xir_compile_source_result_free(output);source_comptime_values_free(ctx);return;
    }
    if (!source_work_units(ctx,NULL,sizeof(SourceContext)+sizeof(SourceProgramComptime))) {
        xr_xir_compile_source_result_free(output);source_comptime_values_free(ctx);return;
    }
    SourceContext received={0};received.compile=ctx->compile;received.graph=ctx->graph;received.linkage_kind=ctx->linkage_kind;
    received.manifests=ctx->manifests;received.prelude_module=ctx->prelude_module;received.core_factory=ctx->core_factory;
    received.remaining_blocks=ctx->remaining_blocks;received.remaining_instructions=ctx->remaining_instructions;
    received.comptime_input=ctx->comptime_sites;received.comptime_input_types=&ctx->types;
    received.materialization_begin=ctx->function_count;
    XrXirSourceResult result={0};SourceProgramComptime job={0};XrXirArtifact *bound=NULL;
    source_construct(&received,&result);
    if (received.diagnostic.status==XR_XIR_OK &&
        (received.comptime_count!=ctx->comptime_count || received.comptime_matched!=ctx->comptime_count))
        source_fail(&received,NULL,XR_XIR_BAD_STRUCTURE,"comptime final occurrence coverage changed");
    bool ok=received.diagnostic.status==XR_XIR_OK && source_program_results(&received,result.checked,&job) &&
        source_program_seal(&received,result.checked,&job,&bound);
    if (ok) {
        xr_xir_compile_artifact_free(result.checked);result.checked=bound;bound=NULL;
        ok=source_program_query_types(&received,result.checked);
    }
    if (ok) source_query_publish(&received,&result);
    xr_xir_compile_source_result_free(output);
    if (received.diagnostic.status==XR_XIR_OK && result.checked && result.snapshot) *output=result;
    else {
        xr_xir_compile_source_result_free(&result);ctx->diagnostic=received.diagnostic;ctx->module=received.module;
        if (ctx->diagnostic.status==XR_XIR_OK) source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime final publication is incomplete");
    }
    xr_xir_compile_artifact_free(bound);xr_xir_compile_instance_literals_free(&job.literals);
    xr_xir_compile_ctfe_instance_batch_free(job.batch);source_comptime_context_free(&received);source_comptime_values_free(ctx);
}
