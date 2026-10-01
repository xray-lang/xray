/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_condition_authority_gates.inc.c - Source facts never confer recipes
 */
static void assert_ordinary_memory_origin(void) {
    XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrModuleResolverConfig config={0};XrModuleResolver *resolver=xr_module_resolver_new(&config);CHECK(resolver);
    SourceContext context={0};context.budget=xr_xir_default_budget();context.linkage_kind=XR_XIR_LIBRARY;
    context.graph=xr_module_graph_new(session,resolver);CHECK(context.graph);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"xray-core-assertions-v1",NULL};
    char *error=NULL;
    CHECK(!xr_module_graph_build_source(context.graph,&authority,xir_core_declaration_source,&error));
    CHECK(!xr_module_graph_topological_sort(context.graph));
    XrXirSourceResult result={0};XrXirBudget checking=context.budget;
    source_construct(&context,&checking,&result);
    CHECK(context.diagnostic.status==XR_XIR_BAD_TYPE && !result.checked && !context.assertion && !context.assert_panics);
    for (uint32_t d=0;d<context.query.declaration_count;++d) CHECK(!context.query.declarations[d].native_identity);
    source_core_dispose(&context);xr_module_resolver_free(resolver);xr_compiler_session_delete(session);xr_free(error);
    /* The result variable in these source bytes has no ordinary declaration
     * binder. Reusing the loader's memory coordinate grants no binding role. */
    xr_xir_source_result_free(&result);
    CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
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
    CHECK(xr_xir_checked_write(artifact,NULL,&packet,NULL)==XR_XIR_BAD_STRUCTURE && !packet.bytes);
    CHECK(xr_xir_specialize(artifact,NULL,&closed,NULL)==XR_XIR_BAD_STRUCTURE && !closed);
    modules[0].dependency_count=dependency;
    identities[binding.function].exported=1;
    CHECK(xr_xir_checked_write(artifact,NULL,&packet,NULL)==XR_XIR_BAD_STRUCTURE && !packet.bytes);
    CHECK(xr_xir_specialize(artifact,NULL,&closed,NULL)==XR_XIR_BAD_STRUCTURE && !closed);
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
    CHECK(xr_xir_checked_write(artifact,NULL,&packet,NULL)==XR_XIR_BAD_STRUCTURE && !packet.bytes);
    CHECK(xr_xir_specialize(artifact,NULL,&closed,NULL)==XR_XIR_BAD_STRUCTURE && !closed);
    instructions[index]=saved;
    CHECK(runtime_live==baseline && runtime_bytes==bytes);
    puts("Default private helper, missing real dependency and exported-helper attacks refused by writer/specializer PASS");
}
