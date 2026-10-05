/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_map_cases.h - Nonzero initializer identity and scratch boundaries
 *
 * KEY CONCEPT:
 *   A real Checked Library fixture precedes private map assertions. Map facts
 *   are not substituted for the ordinary checker or source integration gate.
 */
#ifndef LIBRARY_INITIALIZER_MAP_CASES_H
#define LIBRARY_INITIALIZER_MAP_CASES_H
/* Include after the real source.c amalgamation and counted runtime definitions.
 * source xr_malloc must use the same xr_test_library_source_calloc hook as xr_calloc.
 * The caller supplies the actual canonical identity for library.xr. */
static XrXirArtifact *library_initializer_middle(const char *canonical) {
    XrXirInstruction secret_ops[] = {
        {XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},41,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction init_ops[] = {{XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirInstruction answer_ops[] = {
        {XR_XIR_CALL,XR_XIR_I64,{0},{0},0,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock two={0,2,0,0},one={0,1,0,0};
    XrXirFunction functions[]={
        {"secret",6,NULL,0,XR_XIR_I64,&two,1,secret_ops,2,NULL,0},
        {"$init",5,NULL,0,XR_XIR_UNIT,&one,1,init_ops,1,NULL,0},
        {"answer",6,NULL,0,XR_XIR_I64,&two,1,answer_ops,2,NULL,0}};
    XrXirFunctionIdentity identities[3]={{0},{0},{.exported=1}};
    XrXirSourceModule module={canonical,(uint32_t)strlen(canonical),NULL,0,1};
    XrXirDeclarations declarations={.modules=&module,.module_count=1,.functions=identities,
        .root_module=UINT32_MAX,.entry_function=UINT32_MAX};
    XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,
        .declarations=&declarations,.linkage_kind=XR_XIR_LIBRARY};
    XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_check(library_context,&built,&checked,NULL)==XR_XIR_OK&&checked);
    return checked;
}
static XrXirStatus library_map_operation(const XrXirCompileContext *context,void *opaque) {
    const XrXirModule *library=opaque;XrModuleGraph graph={0};graph.spec_count=2;SourceContext ctx={0};ctx.graph=&graph;ctx.function_count=7;ctx.compile=*context;
    SourceLibraryMap map={0};bool ok=source_library_map(&ctx,library,1,4,&map);
    if(!ok){CHECK(!map.functions);return ctx.diagnostic.status;}
    CHECK(map.functions[0]==4&&map.functions[1]==1&&map.functions[2]==5&&map.next_function==6);
    XrXirInstruction mapped={0};CHECK(source_library_instruction(&ctx,&map,&library->functions[2].instructions[0],&mapped));CHECK(mapped.op==XR_XIR_CALL&&mapped.immediate==4);
    XrXirInstruction untouched=mapped,bad=library->functions[2].instructions[0];bad.immediate=3;
    CHECK(!source_library_instruction(&ctx,&map,&bad,&mapped));CHECK(ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE&&!memcmp(&mapped,&untouched,sizeof(mapped)));
    xr_compile_resources_free(map.functions);return XR_XIR_OK;
}
static void library_map_boundary_cases(const XrXirArtifact *artifact) {
    const XrXirModule *library=xr_xir_compile_artifact_module(artifact);CHECK(library&&library->function_count==3&&library->declarations->modules[0].initializer==1);
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);CHECK(library_map_operation(&owner.context,(void *)library)==XR_XIR_OK);
    XrCompileResourceStats required=library_compile_stats(&owner.context);library_compile_owner_drop(&owner);
    for(unsigned mode=0;mode<3;++mode){XrCompileResourceLimits caps=library_compile_limits;if(mode==0)caps.allocated_bytes=required.allocated_bytes-1;if(mode==1)caps.work=1;
        CHECK(library_compile_owner_new(&owner,&caps)==XR_XIR_OK);if(mode==2)source_program_compile_fail_at=source_program_compile_attempts;
        XrModuleGraph graph={0};graph.spec_count=2;SourceContext ctx={0};ctx.graph=&graph;ctx.function_count=7;ctx.compile=owner.context;SourceLibraryMap map={0};
        CHECK(!source_library_map(&ctx,library,1,4,&map));source_program_compile_fail_at=SIZE_MAX;
        CHECK(!map.functions&&ctx.diagnostic.status==(mode==2?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET));library_compile_owner_drop(&owner);
    }
    /* Full normal map and bad operand are independent of fault prefixes. */
}
static XrXirArtifact *library_initializer_map_cases(const char *canonical) {
    XrXirArtifact *artifact=library_initializer_middle(canonical);
    library_map_boundary_cases(artifact);
    return artifact; /* Caller owns this genuine Checked fixture for catalog/Source41. */
}
#endif // LIBRARY_INITIALIZER_MAP_CASES_H
