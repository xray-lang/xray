/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_core_copy_cases.inc.c - Receiving core identity survives table growth
 */
static void assert_core_view_copies(void) {
    size_t baseline=assert_compile_extra_blocks(),bytes=assert_compile_extra_bytes();
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(assert_compile_context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleResolverConfig config={0};XrModuleResolver *resolver=NULL;
    CHECK(xr_compile_module_resolver_new(assert_compile_context->resources,&config,&resolver)==XR_MODULE_OK);
    SourceContext context={0};context.compile=*assert_compile_context;context.linkage_kind=XR_XIR_LIBRARY;
    context.remaining_blocks=context.compile.limits.blocks;context.remaining_instructions=context.compile.limits.instructions;
    CHECK(xr_compile_module_graph_new(context.compile.resources,session,resolver,&context.graph)==XR_MODULE_OK);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"core-view-copy",NULL};
    const char *source="export fn copied(v:Slice<u8>)->i64{assert(true);return len(v)}";
    char *error=NULL;
    CHECK(xr_compile_module_graph_build_source(context.graph,&authority,source,&error)==XR_MODULE_OK);
    CHECK(xr_compile_module_graph_topological_sort(context.graph)==XR_MODULE_OK);
    CHECK(source_manifests_load(&context));
    XrXirSourceResult result={0};source_construct(&context,&result);
    if (context.diagnostic.status!=XR_XIR_OK) fprintf(stderr,"core view copy %u: %s\n",
        context.diagnostic.status,context.diagnostic.message);
    CHECK(context.diagnostic.status==XR_XIR_OK && result.checked);
    uint32_t selected=UINT32_MAX,copies=0;
    for (uint32_t f=0;f<context.function_count;++f) {
        if (!context.bodies[f].core_copy) continue;
        ++copies;
        if (context.functions[f].parameter_count) selected=f;
    }
    CHECK(copies && selected!=UINT32_MAX && context.function_capacity<context.compile.limits.functions);
    const void *old_functions=context.functions,*old_bodies=context.bodies;
    CHECK(source_materialization_storage(&context,context.function_capacity+1));
    CHECK(context.functions!=old_functions && context.bodies!=old_bodies);
    XrXirDeclarations declarations;XrXirModule module=source_module_view(&context,&declarations);
    for (uint32_t f=0;f<context.function_count;++f) {
        if (!context.bodies[f].core_copy) continue;
        bool preserved=false;
        CHECK(source_view_checked_copy(&context,&module,f,&preserved)==XR_XIR_OK && preserved);
    }
    SourceFunction *body=context.bodies+selected;
    const SourceCoreCopy *proof=body->core_copy;
    bool preserved=true;uint32_t original_module=body->module;
    body->module=0;
    CHECK(source_view_checked_copy(&context,&module,selected,&preserved)==XR_XIR_BAD_STRUCTURE && !preserved);
    body->module=original_module;body->core_copy=NULL;preserved=true;
    CHECK(source_view_checked_copy(&context,&module,selected,&preserved)==XR_XIR_BAD_STRUCTURE && !preserved);
    body->core_copy=proof;
    XrXirType *parameters=body->parameters;body->parameters=NULL;preserved=true;
    CHECK(source_view_checked_copy(&context,&module,selected,&preserved)==XR_XIR_BAD_STRUCTURE && !preserved);
    body->parameters=parameters;
    uint32_t blocks=context.functions[selected].block_count;
    ++context.functions[selected].block_count;preserved=true;
    CHECK(source_view_checked_copy(&context,&module,selected,&preserved)==XR_XIR_BAD_STRUCTURE && !preserved);
    context.functions[selected].block_count=blocks;
    SourceCoreCopy *owned=(SourceCoreCopy *)proof;XrCompileResources *resources=owned->resources;
    owned->resources=NULL;preserved=true;
    CHECK(source_view_checked_copy(&context,&module,selected,&preserved)==XR_XIR_BAD_STRUCTURE && !preserved);
    owned->resources=resources;
    CHECK(source_view_checked_copy(&context,&module,selected,&preserved)==XR_XIR_OK && preserved);
    xr_xir_compile_source_result_free(&result);source_core_dispose(&context);
    xr_compile_module_resolver_free(resolver);xr_compile_session_free(session);xr_compile_resources_free(error);
    CHECK(assert_compile_extra_blocks()==baseline && assert_compile_extra_bytes()==bytes);
    puts("Real core byte-view copy survives materialization table replacement; identity, missing proof, parameters, shape and owner attacks reject");
}
