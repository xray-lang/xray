/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_body_arrays.inc.c - Independent Array contexts and emission identities
 *
 * KEY CONCEPT:
 *   Two equal instructions remain distinct producers. A receiving use cannot
 *   substitute another result, scope, type pool or physical recipe anchor.
 */
typedef struct ArrayInternProbe {XrXirType result;XrXirStatus status;} ArrayInternProbe;
static XrXirStatus array_intern_probe(void *opaque,const XrXirTypeNode *node,XrXirType *out) {
    ArrayInternProbe *probe=opaque;CHECK(node->kind==XR_XIR_TYPE_ARRAY&&node->element==XR_XIR_I64);
    *out=probe->result;return probe->status;
}
static void array_receivers(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats frame={0},done={0};CHECK(xr_compile_resources_stats(resources,&frame)==XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode nodes[]={{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_STRING}};
    XrXirTypes types={.nodes=nodes,.count=2},foreign=types;
    XrXirInstruction instructions[]={
        {.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)256},
        {.op=XR_XIR_ARRAY_NEW,.type=(XrXirType)256},
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={2,0}}};
    XrXirBlock block={.count=4};XrXirFunction function={.name="arrays",.name_length=6,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER,
        .instructions=instructions,.instruction_count=4,.blocks=&block,.block_count=1};
    XrXirFunction *functions=&function;XrXirFunctionIdentity identity={0},*identities=&identity;uint32_t function_count=1,module_count=1;
    XrHirBodyDeclarations declarations={.functions=&functions,.identities=&identities,.function_count=&function_count,.module_count=&module_count};
    XrHirBody *body=NULL,*other=NULL;CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK&&xr_hir_body_new(resources,&types,&other)==XR_XIR_OK);
    CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK);
    XrHirBodyOrigin origin={0};XrHirBodyScope scope={0};CHECK(xr_hir_body_scope(body,(XrHirBodyScope){0},origin,NULL,&scope)==XR_XIR_OK);
    XrHirArrayShape shape={0},sentinel={other,77};CHECK(xr_hir_body_array_receive(body,&types,(XrXirType)256,NULL,&shape,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_array_receive(body,&foreign,(XrXirType)256,NULL,&sentinel,NULL)==XR_XIR_BAD_STRUCTURE&&sentinel.owner==other&&sentinel.index==77);
    XrHirArrayPrimitiveInput input={.types=&types,.scope=scope,.origin=origin,.shape=shape,.operation=XR_XIR_ARRAY_NEW};
    XrHirArrayAction first={0},second={0};CHECK(xr_hir_body_array_primitive(body,&input,NULL,&first,NULL)==XR_XIR_OK);
    input.origin.node=2;CHECK(xr_hir_body_array_primitive(body,&input,NULL,&second,NULL)==XR_XIR_OK);
    XrHirBodyAction consumed={0};CHECK(xr_hir_body_consume(body,first.use,&consumed)==XR_XIR_OK);
    CHECK(xr_hir_body_consume(body,first.use,&consumed)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_body_consume(body,second.use,&consumed)==XR_XIR_OK);
    CHECK(xr_hir_body_emit(body,first.use,(XrXirType)256,20)==XR_XIR_OK);
    CHECK(xr_hir_body_emit(body,first.use,(XrXirType)256,21)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_body_emit(body,second.use,(XrXirType)256,21)==XR_XIR_OK);
    uint32_t map[22];for(uint32_t p=0;p<22;++p)map[p]=UINT32_MAX;map[20]=0;map[21]=1;
    XrHirBodyProjection projection={.function=0,.instruction=1,.value=1,.source_value=21,.type=(XrXirType)256,
        .operation=XR_XIR_ARRAY_NEW,.has_instruction=true,.has_value=true,.source_value_map=map,.source_value_count=22};
    CHECK(xr_hir_body_project(body,first.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    projection.source_value=20;CHECK(xr_hir_body_project(body,first.use,&projection,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    projection.instruction=0;projection.value=0;CHECK(xr_hir_body_project(body,first.use,&projection,NULL,NULL)==XR_XIR_OK);
    projection.instruction=1;projection.value=1;projection.source_value=21;CHECK(xr_hir_body_project(body,second.use,&projection,NULL,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_array_primitive(body,&input,NULL,&second,NULL)==XR_XIR_BAD_STRUCTURE);
    xr_hir_body_end(body);XrHirArrayInfo info={0};CHECK(!xr_hir_body_array_info(body,shape,&info));
    CHECK(xr_hir_array_view(&types,(XrXirType)256,NULL,&info,NULL)==XR_XIR_OK);
    xr_hir_body_free(body);xr_hir_body_free(other);body=NULL;
    CHECK(xr_compile_resources_stats(resources,&done)==XR_COMPILE_RESOURCE_OK&&done.held_bytes==frame.held_bytes);

    CHECK(xr_hir_body_new(resources,&types,&body)==XR_XIR_OK);CHECK(xr_hir_body_declarations(body,&declarations)==XR_XIR_OK);
    CHECK(xr_hir_body_scope(body,(XrHirBodyScope){0},origin,NULL,&scope)==XR_XIR_OK);
    XrHirArrayLiteralInput literal={.types=&types,.scope=scope,.origin=origin,.count=2};XrHirArrayLiteral context={0};
    CHECK(xr_hir_body_array_literal(body,&literal,NULL,&context,NULL)==XR_XIR_OK);XrHirArrayExpected expected={0};
    CHECK(xr_hir_body_array_expected(body,context,&expected)&&!expected.present);
    CHECK(xr_hir_body_array_append(body,context,0,(XrHirArrayValue){XR_XIR_I64,10},NULL,NULL)==XR_XIR_OK);
    CHECK(xr_hir_body_array_expected(body,context,&expected)&&expected.present&&expected.type==XR_XIR_I64&&expected.inferred);
    CHECK(xr_hir_body_array_append(body,context,1,(XrHirArrayValue){XR_XIR_BOOL,11},NULL,NULL)==XR_XIR_BAD_TYPE);
    CHECK(xr_hir_body_array_append(body,context,0,(XrHirArrayValue){XR_XIR_I64,11},NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_hir_body_finish(body,NULL)==XR_XIR_BAD_STRUCTURE);
    xr_hir_body_end(body);xr_hir_body_free(body);
    CHECK(xr_compile_resources_stats(resources,&done)==XR_COMPILE_RESOURCE_OK&&done.held_bytes==frame.held_bytes);
    XrXirCompileContext compile={resources,xr_xir_compile_default_limits()};
    XrXirModule module={.types=&types,.functions=&function,.function_count=1};
    ArrayInternProbe probe={(XrXirType)257,XR_XIR_OK};XrHirArrayPool pool={&compile,&types,&module,NULL,&probe,array_intern_probe};
    XrHirArrayTypeInput type={.scope=XR_HIR_ARRAY_FUNCTION_SIGNATURE,.element=XR_XIR_I64,.function=0};XrXirType received=(XrXirType)77;
    CHECK(xr_hir_array_type(&pool,&type,&received,NULL)==XR_XIR_BAD_TYPE&&received==(XrXirType)77);
    probe.result=XR_XIR_I64;CHECK(xr_hir_array_type(&pool,&type,&received,NULL)==XR_XIR_BAD_TYPE&&received==(XrXirType)77);
    probe.result=(XrXirType)700;CHECK(xr_hir_array_type(&pool,&type,&received,NULL)==XR_XIR_BAD_TYPE&&received==(XrXirType)77);
    probe.result=(XrXirType)256;probe.status=XR_XIR_CANCELLED;
    CHECK(xr_hir_array_type(&pool,&type,&received,NULL)==XR_XIR_CANCELLED&&received==(XrXirType)77);
    probe.status=XR_XIR_OK;CHECK(xr_hir_array_type(&pool,&type,&received,NULL)==XR_XIR_OK&&received==(XrXirType)256);
    CHECK(xr_compile_resources_stats(resources,&done)==XR_COMPILE_RESOURCE_OK&&done.held_bytes==frame.held_bytes);
    xr_compile_resources_release(resources);CHECK(!physical_live&&!physical_bytes);
    puts("Owned Array ordered context, pool identity, distinct NEW result/anchor and cleanup PASS");
}
