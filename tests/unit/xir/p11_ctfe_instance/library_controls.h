/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * library_controls.h - Opaque literal type names grant no field authority
 */
static void library_comptime_controls(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=NULL,*received=NULL;
    CHECK(library_comptime_operation(&context,"fresh",&source)==XR_XIR_OK);
    uint32_t provider=source->construction->comptime.bindings[0].provider;bool literal=false;
    CHECK(xir_literal_type_names(&context,&source->module,source->construction,provider,&literal)==XR_XIR_OK && literal);
    XrXirComptimeTable unbound=source->construction->comptime;
    CHECK(unbound.binding_count==2);unbound.bindings+=1;--unbound.binding_count;
    XrXirConstructionSpec spec={source->construction->rows,source->construction->count,source->construction->materializations,
        source->construction->materialization_count,&unbound};XrXirConstruction *removed=NULL;
    CHECK(xr_xir_compile_construction_new_v3(&context,source->module.types,&spec,&removed)==XR_XIR_OK);
    literal=true;CHECK(xir_literal_type_names(&context,&source->module,removed,provider,&literal)==XR_XIR_OK && !literal);
    CHECK(xr_xir_compile_recheck_v2(&context,&source->module,removed,&received,NULL)==XR_XIR_BAD_TYPE && !received);
    xr_xir_compile_construction_free(removed);
    XrXirStatus status=XR_XIR_OK;
    XrXirFunction *functions=xir_compile_copy(&context,source->module.functions,(size_t)source->module.function_count*sizeof(*functions),&status);
    XrXirFunctionIdentity *identities=xir_compile_copy(&context,source->module.declarations->functions,
        (size_t)source->module.function_count*sizeof(*identities),&status);CHECK(functions && identities && status==XR_XIR_OK);
    XrXirDeclarations declarations=*source->module.declarations;declarations.functions=identities;
    XrXirModule current=source->module;current.functions=functions;current.declarations=&declarations;
    identities[provider].exported=1;literal=true;
    CHECK(xir_literal_type_names(&context,&current,source->construction,provider,&literal)==XR_XIR_OK && !literal);
    CHECK(xr_xir_compile_recheck_v2(&context,&current,source->construction,&received,NULL)!=XR_XIR_OK && !received);
    identities[provider].exported=0;
    XrXirType nominal=XR_XIR_UNIT,array=source->module.functions[provider].result;
    for (uint32_t t=0;t<source->module.types->count;++t) {
        const XrXirTypeNode *node=&source->module.types->nodes[t];
        if (node->kind==XR_XIR_TYPE_NOMINAL) nominal=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
    }
    CHECK(nominal!=XR_XIR_UNIT && xr_xir_array_element(source->module.types,array)==nominal);
    XrXirInstruction ops[]={
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},7,{0}},
        {XR_XIR_STRUCT_NEW,nominal,{0,1},{0},0,{0}},
        {XR_XIR_ARRAY_NEW,array,{1,1},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{2},{0},0,{0}}};
    uint32_t operands[]={0,1};XrXirBlock block={0,4,0,0};
    functions[provider].instructions=ops;functions[provider].instruction_count=4;
    functions[provider].blocks=&block;functions[provider].block_count=1;
    functions[provider].operands=operands;functions[provider].operand_count=2;literal=false;
    CHECK(xir_literal_type_names(&context,&current,source->construction,provider,&literal)==XR_XIR_OK && literal);
    CHECK(xr_xir_compile_recheck_v2(&context,&current,source->construction,&received,NULL)==XR_XIR_BAD_TYPE && !received);
    ops[1].op=XR_XIR_STRUCT_GET;literal=true;
    CHECK(xir_literal_type_names(&context,&current,source->construction,provider,&literal)==XR_XIR_OK && !literal);
    xr_compile_resources_free(identities);xr_compile_resources_free(functions);
    CHECK(xr_xir_compile_artifact_verify(source,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Library opaque names: private data recipe only, exported/field-read refuse, literal construction still needs actual owner, physical0\n");
}

static void library_comptime_site_controls(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=NULL,*checked=NULL;
    CHECK(pending_source(&context,"array",2,&source)==XR_XIR_OK);
    XirComptimePending *pending=NULL;CHECK(xr_xir_compile_comptime_pending(source,&pending,NULL)==XR_XIR_OK && pending);
    CHECK(xr_xir_compile_comptime_pending_specialize(pending,&checked,NULL)==XR_XIR_OK);
    const XrXirProvenance *proof=checked->module.provenance;const XirEvaluationRoot *root=&proof->evaluation;
    XrXirComptimeSite site={proof->origins[root->caller].function,root->instruction,proof->origins[root->callee].function};
    XirCtfeInstanceBatchRequest evaluation=instance_batch_request(checked,&root->callee,1);
    XirCtfeInstanceBatch *batch=NULL;CHECK(xr_xir_compile_ctfe_instance_batch(&evaluation,&batch)==XR_XIR_CTFE_OK);
    XirLiteralRequest request={&context,batch,1};XirLiteralDefinitions output={0};XrCompileResourceStats before=stats(&context);
    CHECK(xr_xir_compile_instance_literals_at(&request,NULL,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    XrXirComptimeSite wrong=site;++wrong.instruction;
    CHECK(xr_xir_compile_instance_literals_at(&request,&wrong,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    wrong=site;wrong.helper=wrong.caller;
    CHECK(xr_xir_compile_instance_literals_at(&request,&wrong,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!output.definition && !output.bindings && !output.count && stats(&context).live_bytes==before.live_bytes);
    CHECK(xr_xir_compile_instance_literals_at(&request,&site,&output,NULL)==XR_XIR_OK);
    const XrXirComptimeTable *table=&output.definition->construction->comptime;
    CHECK(table->site_count==1 && table->binding_count==1 && table->bindings[0].provider==output.bindings[0].provider);
    CHECK(xr_xir_compile_artifact_verify(output.definition,NULL)==XR_XIR_OK);
    xr_xir_compile_instance_literals_free(&output);xr_xir_compile_ctfe_instance_batch_free(batch);
    xr_xir_compile_artifact_free(checked);xr_xir_compile_comptime_pending_free(pending);xr_xir_compile_artifact_free(source);
    checked=instance_source(&context,"array");uint32_t function=instance_named(checked,"array",0);
    evaluation=instance_batch_request(checked,&function,1);batch=NULL;
    CHECK(xr_xir_compile_ctfe_instance_batch(&evaluation,&batch)==XR_XIR_CTFE_OK);request.instances=batch;
    before=stats(&context);CHECK(xr_xir_compile_instance_literals_at(&request,&site,&output,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(!output.definition && !output.bindings && !output.count && stats(&context).live_bytes==before.live_bytes);
    xr_xir_compile_ctfe_instance_batch_free(batch);xr_xir_compile_artifact_free(checked);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Library site receiving: genuine exact pending root, no missing/wrong site or ordinary INSTANCE authority, atomic prefixes physical0\n");
}
