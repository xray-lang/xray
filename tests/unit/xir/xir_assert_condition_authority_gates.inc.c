/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_condition_authority_gates.inc.c - Source facts never confer recipes
 */
static void assert_ordinary_memory_origin(void) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(assert_compile_context->resources,&session)==XR_COMPILER_SESSION_OK);CHECK(session);
    XrModuleResolverConfig config={0};XrModuleResolver *resolver=NULL;CHECK(xr_compile_module_resolver_new(assert_compile_context->resources,&config,&resolver)==XR_MODULE_OK);CHECK(resolver);
    SourceContext context={0};context.compile=*assert_compile_context;context.remaining_blocks=context.compile.limits.blocks;context.remaining_instructions=context.compile.limits.instructions;context.linkage_kind=XR_XIR_LIBRARY;
    CHECK(xr_compile_module_graph_new(assert_compile_context->resources,session,resolver,&context.graph)==XR_MODULE_OK);CHECK(context.graph);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"xray-core-assertions-v1",NULL};
    char *error=NULL;
    CHECK(!xr_compile_module_graph_build_source(context.graph,&authority,xir_core_declaration_source,&error));
    CHECK(!xr_compile_module_graph_topological_sort(context.graph));
    XrXirSourceResult result={0};
    source_construct(&context,&result);
    CHECK(context.diagnostic.status==XR_XIR_BAD_TYPE && !result.checked && !context.assertion && !context.assert_panics);
    for (uint32_t d=0;d<context.query.declaration_count;++d) CHECK(!context.query.declarations[d].native_identity);
    source_core_dispose(&context);xr_compile_module_resolver_free(resolver);xr_compile_session_free(session);xr_compile_resources_free(error);
    /* The result variable in these source bytes has no ordinary declaration
     * binder. Reusing the loader's memory coordinate grants no binding role. */
    xr_xir_compile_source_result_free(&result);
    CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes() && !runtime_live && !runtime_bytes);
    puts("Same exact Source bytes/memory identity reject unbound result role and grant no native identity PASS");
}
static void assert_permissions(XrXirSourceResult *result) {
    XrXirArtifact *artifact=result->checked;
    XrXirModule *module=&artifact->module;
    XrXirDeclarations *declarations=(XrXirDeclarations *)module->declarations;
    XrXirSourceModule *modules=(XrXirSourceModule *)declarations->modules;
    XrXirFunctionIdentity *identities=(XrXirFunctionIdentity *)declarations->functions;
    XrXirDefaultBinding binding=module->defaults->records[0];
    size_t baseline=runtime_live,bytes=runtime_bytes;
    uint32_t dependency=modules[0].dependency_count;
    modules[0].dependency_count=0;
    XrXirCheckedPacket packet={0};XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_BAD_STRUCTURE && !packet.bytes);
    CHECK(xr_xir_compile_specialize(artifact,&closed,NULL)==XR_XIR_BAD_STRUCTURE && !closed);
    modules[0].dependency_count=dependency;
    identities[binding.function].exported=1;
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_BAD_STRUCTURE && !packet.bytes);
    CHECK(xr_xir_compile_specialize(artifact,&closed,NULL)==XR_XIR_BAD_STRUCTURE && !closed);
    identities[binding.function].exported=0;
    uint32_t function=assert_find(module,"success");
    XrXirInstruction *instructions=(XrXirInstruction *)module->functions[function].instructions;
    uint32_t index=UINT32_MAX;
    for (uint32_t i=0;i<module->functions[function].instruction_count;++i)
        if (instructions[i].op==XR_XIR_CALL) {CHECK(index==UINT32_MAX);index=i;}
    CHECK(index!=UINT32_MAX);
    XrXirInstruction saved=instructions[index];
    instructions[index].immediate=binding.function;instructions[index].type=XR_XIR_STRING;
    instructions[index].args[1]=0;
    CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_BAD_STRUCTURE && !packet.bytes);
    CHECK(xr_xir_compile_specialize(artifact,&closed,NULL)==XR_XIR_BAD_STRUCTURE && !closed);
    instructions[index]=saved;
    CHECK(runtime_live==baseline && runtime_bytes==bytes);
    puts("Default private helper, missing real dependency and exported-helper attacks refused by writer/specializer PASS");
}
