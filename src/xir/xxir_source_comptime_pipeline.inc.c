/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_pipeline.inc.c - Checked thunk execution and fresh Source receiving
 *
 * KEY CONCEPT:
 *   Both actual Source constructions use the original finite ledger. Only the
 *   final independent Built checking and query publication become output.
 */
static bool source_comptime_failure(SourceContext *ctx,AstNode *node,XrXirCtfeStatus status) {
    XrXirStatus mapped=status==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        status==XR_XIR_CTFE_MEMORY_LIMIT?XR_XIR_MEMORY_LIMIT:
        status==XR_XIR_CTFE_OVERFLOW?XR_XIR_OVERFLOW:
        status==XR_XIR_CTFE_CANCELLED?XR_XIR_CANCELLED:
        status==XR_XIR_CTFE_DEADLINE?XR_XIR_DEADLINE:
        status==XR_XIR_CTFE_CLOCK_FAILURE?XR_XIR_CLOCK_FAILURE:
        status==XR_XIR_CTFE_COMPLEXITY_LIMIT || status==XR_XIR_CTFE_STEP_LIMIT || status==XR_XIR_CTFE_DEPTH_LIMIT ||
        status==XR_XIR_CTFE_FRAME_LIMIT || status==XR_XIR_CTFE_OUTPUT_LIMIT?XR_XIR_BUDGET:
        status==XR_XIR_CTFE_BAD_ARTIFACT || status==XR_XIR_CTFE_IDENTITY?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
    const char *message=status==XR_XIR_CTFE_STEP_LIMIT?"comptime step limit exceeded":
        status==XR_XIR_CTFE_OUTPUT_LIMIT?"comptime logical output limit exceeded":
        status==XR_XIR_CTFE_PROFILE?"comptime target profile is unsupported":
        status==XR_XIR_CTFE_DEPTH_LIMIT?"comptime call depth exceeded":
        status==XR_XIR_CTFE_FRAME_LIMIT?"comptime whole-stack frame limit exceeded":
        status==XR_XIR_CTFE_OUT_OF_MEMORY?"comptime allocation failed":
        status==XR_XIR_CTFE_COMPLEXITY_LIMIT?"comptime local complexity limit exceeded":
        status==XR_XIR_CTFE_MEMORY_LIMIT?"comptime held-memory limit exceeded":
        status==XR_XIR_CTFE_OVERFLOW?"comptime storage size overflow":
        status==XR_XIR_CTFE_CANCELLED?"comptime compilation cancelled":
        status==XR_XIR_CTFE_DEADLINE?"comptime compilation deadline exceeded":
        status==XR_XIR_CTFE_CLOCK_FAILURE?"comptime deadline clock failed":
        status==XR_XIR_CTFE_DIVIDE_BY_ZERO?"comptime division by zero":
        status==XR_XIR_CTFE_NUMERIC_RANGE?"comptime value range violation":
        status==XR_XIR_CTFE_NULL_UNWRAP?"comptime null unwrap":
        status==XR_XIR_CTFE_BOUNDS?"comptime index out of bounds":
        status==XR_XIR_CTFE_UNCAUGHT_ERROR?"comptime uncaught error":
        status==XR_XIR_CTFE_ASSERTION_FAILURE?"comptime assertion failed":
        status==XR_XIR_CTFE_MATCH_FAILURE?"comptime non-exhaustive match":
        status==XR_XIR_CTFE_CLEANUP_PANIC?"comptime panic escaped defer cleanup":
        "comptime requires an admitted deterministic closed-value call closure";
    return source_fail(ctx,node,mapped,message);
}
static bool source_comptime_helper(SourceContext *ctx,const XrXirArtifact *lowered,
    const SourceComptimeSite *site,uint32_t *output) {
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const XrXirFunction *original=&ctx->functions[site->helper];uint32_t found=UINT32_MAX;
    const XrXirFunctionIdentity *identity=&ctx->identities[site->helper];
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        const XrXirFunction *body=&module->functions[f];
        if (module->declarations->functions[f].module!=site->module || body->name_length!=original->name_length ||
            !source_span_same(ctx,site->syntax,body->name,original->name,body->name_length)) continue;
        if (found!=UINT32_MAX || body->parameter_count ||
            module->declarations->functions[f].nominal_owner!=identity->nominal_owner ||
            module->declarations->functions[f].method_kind!=identity->method_kind || module->declarations->functions[f].cleanup_owner)
            return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"comptime helper changed its actual identity or signature");
        if (!source_comptime_type_matches(ctx,site->syntax,module->types,body->result,original->result)) return false;
        found=f;
    }
    if (found==UINT32_MAX) return source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"comptime helper is absent after specialization");
    *output=found;return true;
}
static bool source_program_seal(SourceContext *ctx,const XrXirArtifact *source,XrXirArtifact **output);
static bool source_program_complete(SourceContext *ctx,const XrXirArtifact *source,XrXirArtifact **output);
static bool source_program_query_types(SourceContext *ctx,const XrXirArtifact *checked);
static bool source_comptime_evaluate_library(SourceContext *ctx,const XrXirArtifact *checked);
static bool source_comptime_store(SourceContext *ctx,const XrXirModule *module,
    SourceComptimeSite *site,uint32_t function,XrXirValue *value) {
    XrXirType result=module->functions[function].result;
    XrXirCtfeStatus shaped=xr_xir_compile_ctfe_type_admit(&ctx->compile,module->types,result);
    if (shaped!=XR_XIR_CTFE_OK) return source_comptime_failure(ctx,site->syntax,shaped);
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(module->types,result);
    site->result_arity=tuple?tuple->parameter_count:(xr_xir_type_is_nullable(module->types,result) ||
        xr_xir_type_is_array(module->types,result) || xr_xir_type_is_struct(module->types,result) ||
        xr_xir_type_is_enum(module->types,result))?1:0;
    if (!source_comptime_result_value(ctx,site->syntax,value,site->result_arity))
        return source_fail(ctx,site->syntax,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status,
            "comptime returned a noncanonical value");
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    site->value=*value;*value=(XrXirValue){0};return true;
}
/* All selected helpers belong to one private immutable Lowered owner. Batch
 * admission checks that current owner once; each root still admits its closure
 * and executes with independent original limits on the same compiler ledger. */
static bool source_comptime_batch(SourceContext *ctx,const XrXirArtifact *lowered,
    const XrXirTarget *target,uint32_t count) {
    XrXirStatus status=XR_XIR_OK;bool ok=false;
    uint32_t *functions=xir_compile_calloc(&ctx->compile,count,sizeof(*functions),&status);
    SourceComptimeSite **sites=NULL;XrXirValue *values=NULL;
    if (functions) sites=xir_compile_calloc(&ctx->compile,count,sizeof(*sites),&status);
    if (sites) values=xir_compile_calloc(&ctx->compile,count,sizeof(*values),&status);
    if (!functions || !sites || !values) {
        source_fail(ctx,NULL,status,"comptime batch storage allocation failed");goto done;
    }
    uint32_t selected=0;
    for (SourceComptimeSite *site=ctx->comptime_sites;site;site=site->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
        if (ctx->generics[site->helper].parameter_count) continue;
        ctx->module=site->module;uint32_t function=UINT32_MAX;
        if (selected==count) { source_fail(ctx,site->syntax,XR_XIR_BAD_STRUCTURE,"comptime batch inventory grew");goto done; }
        if (!source_comptime_helper(ctx,lowered,site,&function) ||
            !((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
        functions[selected]=function;sites[selected++]=site;
    }
    if (selected!=count) { source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime batch inventory is incomplete");goto done; }
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) goto done;
    XirCtfeBatchRequest request={.artifact=lowered,.functions=functions,.count=count,.target=*target,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES},
        .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    /* Lowering already owns the canonical Checked packet and its digest. */
    memcpy(request.checked_identity,lowered->checked_identity,sizeof(request.checked_identity));
    uint32_t failed=UINT32_MAX;
    XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_batch_at(&request,ctx->request,values,&failed);
    if (evaluated!=XR_XIR_CTFE_OK) {
        if (failed!=UINT32_MAX && failed>=count) source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime failure ordinal is outside its inventory");
        else {
            SourceComptimeSite *site=sites[failed==UINT32_MAX?0:failed];ctx->module=site->module;
            source_comptime_failure(ctx,site->syntax,evaluated);
        }
        goto done;
    }
    for (uint32_t i=0;i<count;++i) {
        ctx->module=sites[i]->module;
        if (!((ctx)->diagnostic.status == XR_XIR_OK) ||
            !source_comptime_store(ctx,&lowered->module,sites[i],functions[i],&values[i])) goto done;
    }
    ok=true;
done:
    for (uint32_t i=0;values && i<count;++i) xr_xir_value_drop(&values[i]);
    xr_compile_resources_free(values);xr_compile_resources_free(sites);xr_compile_resources_free(functions);
    return ok;
}
static bool source_comptime_evaluate(SourceContext *ctx,const XrXirArtifact *checked,bool defer_generic) {
    uint32_t expected=0;
    for (const SourceComptimeSite *site=ctx->comptime_sites;site;site=site->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (ctx->generics[site->helper].parameter_count && !defer_generic) {
            ctx->module=site->module;
            return source_fail(ctx,site->syntax,XR_XIR_BAD_TYPE,"comptime requires per-instance Source result binding");
        }
        if (!ctx->generics[site->helper].parameter_count) ++expected;
    }
    if (!expected) return true;
    if (ctx->linkage_kind==XR_XIR_LIBRARY) return source_comptime_evaluate_library(ctx,checked);
    AstNode *entry=ctx->comptime_sites ? ctx->comptime_sites->syntax : NULL;
    if (ctx->comptime_sites) ctx->module=ctx->comptime_sites->module;
    XrXirArtifact *closed=NULL,*lowered=NULL;
    /* The public specializer completely admits its current input. */
    XrXirStatus status=xr_xir_compile_specialize(checked,&closed,NULL);
    /* The supported target profile is explicit. Family admission and canonical
     * receiving use authentic type owners, never host sizeof or pointers. */
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    /* Specialization returned this sole owner.
     * Lowering consumes it on success or failure; no later step reads closed. */
    if (status==XR_XIR_OK) status=xr_xir_compile_lower_take(&closed,&target,&lowered,NULL);
    if (status!=XR_XIR_OK) source_fail(ctx,entry,status,"comptime thunk pipeline failed complete receiving");
    else source_comptime_batch(ctx,lowered,&target,expected);
    xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(lowered);
    return ctx->diagnostic.status==XR_XIR_OK;
}
/* This destructor owns declaration/recipe data, never the borrowed input graph
 * or immutable manifest rows held by the original synchronous Source owner. */
static void source_comptime_values_free(SourceContext *ctx) {
    for (SourceComptimeSite *site=ctx->comptime_sites;site;site=site->next) xr_xir_value_drop(&site->value);
    for (SourceMaterialization *site=ctx->materializations;site;site=site->next) xr_xir_value_drop(&site->value);
}
static void source_comptime_context_free(SourceContext *ctx) {
    source_comptime_values_free(ctx);
    source_release_retired_arrays(ctx);
    xr_xir_compile_construction_free(ctx->construction);
    while (ctx->memory) { SourceMemory *next=ctx->memory->next;xr_compile_resources_free(ctx->memory);ctx->memory=next; }
}
#include "xxir_source_comptime_library.inc.c"
static void source_program_comptime_receive(SourceContext *ctx,XrXirSourceResult *output);
static void source_comptime_receive(SourceContext *ctx,XrXirSourceResult *output) {
    if (ctx->diagnostic.status!=XR_XIR_OK) return;
    if (!output->checked) { source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime has no checked Source input");return; }
    if (ctx->linkage_kind==XR_XIR_PROGRAM && output->checked->construction->comptime.site_count) {
        source_program_comptime_receive(ctx,output);return;
    }
    if (!ctx->comptime_count) return;
    for (const SourceComptimeSite *site=ctx->comptime_sites;ctx->linkage_kind==XR_XIR_PROGRAM && site;site=site->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) { xr_xir_compile_source_result_free(output);source_comptime_values_free(ctx);return; }
        if (ctx->generics[site->helper].parameter_count) { source_program_comptime_receive(ctx,output);return; }
    }
    if (!source_comptime_evaluate(ctx,output->checked,ctx->linkage_kind==XR_XIR_LIBRARY)) { xr_xir_compile_source_result_free(output);source_comptime_values_free(ctx);return; }
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) { xr_xir_compile_source_result_free(output);source_comptime_values_free(ctx);return; }
    SourceContext received={0};received.request=ctx->request;received.compile=ctx->compile;received.graph=ctx->graph;received.linkage_kind=ctx->linkage_kind;
    received.semantic_output=ctx->semantic_output;
    received.binding_readers=ctx->binding_readers;
    received.interactive=ctx->interactive;received.type_query=ctx->type_query;received.repl_tail=ctx->repl_tail;
    received.history_input=ctx->history_input;received.self_unit=ctx->self_unit;
    received.manifests=ctx->manifests;received.prelude_module=ctx->prelude_module;received.core_factory=ctx->core_factory;
    received.remaining_blocks=ctx->remaining_blocks;received.remaining_instructions=ctx->remaining_instructions;
    received.comptime_input=ctx->comptime_sites;
    /* Both Source owners stay live for this synchronous receiving pass. */
    received.comptime_input_types=&ctx->types;
    received.materialization_begin=ctx->function_count;
    XrXirSourceResult result={0};source_construct(&received,&result);
    if (received.diagnostic.status==XR_XIR_OK &&
        (received.comptime_count!=ctx->comptime_count || received.comptime_matched!=ctx->comptime_count))
        source_fail(&received,NULL,XR_XIR_BAD_STRUCTURE,"final Source comptime occurrence coverage changed");
    if (received.diagnostic.status==XR_XIR_OK && ctx->linkage_kind==XR_XIR_LIBRARY) {
        XrXirArtifact *sealed=NULL;
        if (source_program_seal(&received,result.checked,&sealed)) {
            xr_xir_compile_artifact_free(result.checked);result.checked=sealed;
            source_program_query_types(&received,result.checked);
        }
    }
    if (received.diagnostic.status==XR_XIR_OK) source_query_publish(&received,&result);
    xr_xir_compile_source_result_free(output);
    if (received.diagnostic.status==XR_XIR_OK) *output=result;
    else { xr_xir_compile_source_result_free(&result);ctx->diagnostic=received.diagnostic;ctx->module=received.module; }
    source_comptime_context_free(&received);source_comptime_values_free(ctx);
}
