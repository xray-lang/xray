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
        status==XR_XIR_CTFE_BUDGET || status==XR_XIR_CTFE_STEP_LIMIT || status==XR_XIR_CTFE_DEPTH_LIMIT ||
        status==XR_XIR_CTFE_FRAME_LIMIT || status==XR_XIR_CTFE_OUTPUT_LIMIT?XR_XIR_BUDGET:
        status==XR_XIR_CTFE_BAD_ARTIFACT || status==XR_XIR_CTFE_IDENTITY?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
    const char *message=status==XR_XIR_CTFE_STEP_LIMIT?"comptime step limit exceeded":
        status==XR_XIR_CTFE_DEPTH_LIMIT?"comptime call depth exceeded":
        status==XR_XIR_CTFE_FRAME_LIMIT?"comptime whole-stack frame limit exceeded":
        status==XR_XIR_CTFE_OUT_OF_MEMORY?"comptime allocation failed":
        status==XR_XIR_CTFE_BUDGET?"comptime compiler ledger exhausted":
        status==XR_XIR_CTFE_DIVIDE_BY_ZERO?"comptime division by zero":
        status==XR_XIR_CTFE_NUMERIC_RANGE?"comptime value range violation":
        status==XR_XIR_CTFE_NULL_UNWRAP?"comptime null unwrap":
        status==XR_XIR_CTFE_BOUNDS?"comptime index out of bounds":
        "comptime requires an admitted deterministic scalar direct-call closure";
    return source_fail(ctx,node,mapped,message);
}
static bool source_comptime_helper(SourceContext *ctx,const XrXirArtifact *lowered,
    const SourceComptimeSite *site,uint32_t *output) {
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    const XrXirFunction *original=&ctx->functions[site->helper];uint32_t found=UINT32_MAX;
    const XrXirFunctionIdentity *identity=&ctx->identities[site->helper];
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!source_work_units(ctx,site->syntax,7)) return false;
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
static bool source_comptime_evaluate(SourceContext *ctx,const XrXirArtifact *checked) {
    AstNode *entry=ctx->comptime_sites ? ctx->comptime_sites->syntax : NULL;
    if (ctx->comptime_sites) ctx->module=ctx->comptime_sites->module;
    XrXirArtifact *closed=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};uint8_t identity[32]={0};
    if (!source_work_units(ctx,NULL,sizeof(identity))) return false;
    XrXirStatus status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(checked,&closed,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(closed,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(closed,&packet,NULL);
    if (status==XR_XIR_OK) {
        if (!source_work_units(ctx,NULL,packet.length)) status=XR_XIR_BUDGET;
        else xr_sha256(packet.bytes,packet.length,identity);
    }
    /* The supported target profile is explicit. Family admission and canonical
     * receiving use authentic type owners, never host sizeof or pointers. */
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(lowered,NULL);
    if (status!=XR_XIR_OK) source_fail(ctx,entry,status,"comptime thunk pipeline failed complete receiving");
    uint32_t count=0;
    for (SourceComptimeSite *site=ctx->comptime_sites;ctx->diagnostic.status==XR_XIR_OK && site;site=site->next) {
        ctx->module=site->module;
        uint32_t f=UINT32_MAX;
        if (!source_comptime_helper(ctx,lowered,site,&f)) break;
        if (!source_work_units(ctx,site->syntax,sizeof(XrXirCtfeCallRequest)+sizeof(identity)+sizeof(XrXirValue))) break;
        XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,.function=f,.target=target,
            .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},
            .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
        memcpy(request.evaluation.checked_identity,identity,sizeof(identity));
        XrXirValue value={0};XrXirCtfeStatus evaluated=xr_xir_compile_ctfe_calls(&request,&value);
        if (evaluated!=XR_XIR_CTFE_OK) { xr_xir_value_drop(&value);source_comptime_failure(ctx,site->syntax,evaluated);break; }
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        XrXirCtfeTypeShape shape={0};XrXirCtfeStatus shaped=xr_xir_compile_ctfe_type_shape(&ctx->compile,module->types,module->functions[f].result,&shape);
        if (shaped!=XR_XIR_CTFE_OK) { xr_xir_value_drop(&value);source_comptime_failure(ctx,site->syntax,shaped);break; }
        if (!source_work_units(ctx,site->syntax,sizeof(site->result_work))) { xr_xir_value_drop(&value);break; }
        site->result_work=shape.work;
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(module->types,module->functions[f].result);
        site->result_arity=tuple?tuple->parameter_count:(xr_xir_type_is_nullable(module->types,module->functions[f].result) || xr_xir_type_is_array(module->types,module->functions[f].result) ||
            xr_xir_type_is_struct(module->types,module->functions[f].result) || xr_xir_type_is_enum(module->types,module->functions[f].result))?1:0;
        if (!source_comptime_result_value(ctx,site->syntax,&value,site->result_arity,site->result_work)) {
            xr_xir_value_drop(&value);source_fail(ctx,site->syntax,ctx->diagnostic.status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:ctx->diagnostic.status,"comptime returned a noncanonical value");break;
        }
        if (!source_work_units(ctx,site->syntax,sizeof(value)+1)) { xr_xir_value_drop(&value);break; }
        site->value=value;++count;
    }
    if (ctx->diagnostic.status==XR_XIR_OK && count!=ctx->comptime_count)
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"comptime result coverage is incomplete");
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(lowered);
    return ctx->diagnostic.status==XR_XIR_OK;
}
/* This destructor owns declaration/recipe data, never the borrowed input graph
 * or immutable manifest rows held by the original synchronous Source owner. */
static void source_comptime_values_free(SourceContext *ctx) {
    for (SourceComptimeSite *site=ctx->comptime_sites;site;site=site->next) xr_xir_value_drop(&site->value);
}
static void source_comptime_context_free(SourceContext *ctx) {
    source_comptime_values_free(ctx);
    xr_xir_compile_construction_free(ctx->construction);
    while (ctx->memory) { SourceMemory *next=ctx->memory->next;xr_compile_resources_free(ctx->memory);ctx->memory=next; }
}
static void source_comptime_receive(SourceContext *ctx,XrXirSourceResult *output) {
    if (!ctx->comptime_count || ctx->diagnostic.status!=XR_XIR_OK) return;
    if (!source_comptime_evaluate(ctx,output->checked)) { xr_xir_compile_source_result_free(output);source_comptime_values_free(ctx);return; }
    if (!source_work_units(ctx,NULL,sizeof(SourceContext))) { xr_xir_compile_source_result_free(output);source_comptime_values_free(ctx);return; }
    SourceContext received={0};received.compile=ctx->compile;received.graph=ctx->graph;received.linkage_kind=ctx->linkage_kind;
    received.manifests=ctx->manifests;received.prelude_module=ctx->prelude_module;received.core_factory=ctx->core_factory;
    received.remaining_blocks=ctx->remaining_blocks;received.remaining_instructions=ctx->remaining_instructions;
    received.comptime_input=ctx->comptime_sites;
    XrXirSourceResult result={0};source_construct(&received,&result);
    if (received.diagnostic.status==XR_XIR_OK &&
        (received.comptime_count!=ctx->comptime_count || received.comptime_matched!=ctx->comptime_count))
        source_fail(&received,NULL,XR_XIR_BAD_STRUCTURE,"final Source comptime occurrence coverage changed");
    if (received.diagnostic.status==XR_XIR_OK) source_query_publish(&received,&result);
    xr_xir_compile_source_result_free(output);
    if (received.diagnostic.status==XR_XIR_OK) *output=result;
    else { xr_xir_compile_source_result_free(&result);ctx->diagnostic=received.diagnostic;ctx->module=received.module; }
    source_comptime_context_free(&received);source_comptime_values_free(ctx);
}
