/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_views_growth_cases.h - Real Checked dependency expansion crosses graph capacity
 */
#ifndef XIR_LIBRARY_VIEWS_GROWTH_CASES_H
#define XIR_LIBRARY_VIEWS_GROWTH_CASES_H
static void library_views_growth_graph(XrXirLibraryCatalog *catalog,XrModuleIdentityAuthority authority) {
    XrCompilerSession *session=library_session_new(library_context);
    XrModuleResolverConfig config={NULL,NULL,catalog};XrModuleResolver *resolver=library_resolver_new(library_context,&config);
    XrModuleGraph *graph=NULL;CHECK(xr_compile_module_graph_new(library_context->resources,session,resolver,&graph)==XR_MODULE_OK);
    CHECK(graph->spec_capacity==16&&graph->spec_count==0);
    char *error=NULL;CHECK(xr_compile_module_graph_build(graph,XR_SOURCE_FIXTURES "/views_chain_root.xr",&authority,&error)==XR_MODULE_OK&&!error);
    CHECK(graph->spec_count==18&&graph->spec_capacity>16);
    unsigned checked=0,edges=0;
    for(int i=0;i<graph->spec_count;++i){const XrModuleSpec *spec=&graph->specs[i];
        if(spec->representation==XR_MODULE_CHECKED_LIBRARY){++checked;CHECK(!spec->ast&&spec->resource&&spec->resource->checked);}
        edges+=(unsigned)spec->dep_count;
        for(int e=0;e<spec->dep_count;++e)CHECK(spec->dep_indices[e]>=0&&spec->dep_indices[e]<graph->spec_count);
    }
    CHECK(checked==17&&edges==17);
    CHECK(xr_compile_module_graph_topological_sort(graph)==XR_MODULE_OK&&!graph->has_cycle&&graph->topo_count==18);
    xr_compile_module_graph_free(graph);xr_compile_module_resolver_free(resolver);xr_compile_session_free(session);
}
static void library_views_growth_case(void) {
    char storage[17][32];const char *names[17];
    for(unsigned i=0;i<17;++i){CHECK(snprintf(storage[i],sizeof(storage[i]),"views_chain_%02u.xr",i)>0);names[i]=storage[i];}
    for(unsigned i=0;i<17;++i){char text[256];
        if(i==16)CHECK(snprintf(text,sizeof(text),"export fn answer()->i64{return 41;}\n")>0);
        else CHECK(snprintf(text,sizeof(text),"import \"./views_chain_%02u\" as part;export fn answer()->i64{return part.answer();}\n",i+1)>0);
        conflict_text(names[i],text);
    }
    conflict_text("views_chain_root.xr","import \"./views_chain_00\" as chain;export fn result()->i64{return chain.answer();}\n");
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirLibraryModuleInput bindings[17]={0};size_t count=0;
    XrXirCheckedPacket packet=library_views_packet(XR_SOURCE_FIXTURES "/views_chain_00.xr",authority,names,17,bindings,&count);
    CHECK(count==17);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},bindings,count};xr_sha256(input.packet,input.length,input.sha256);
    for(unsigned i=0;i<17;++i){char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_FIXTURES,names[i])>0);CHECK(!remove(path));}
    LibrarySourceFixture fixture={&input,1,XR_SOURCE_FIXTURES "/views_chain_root.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("Views17 actual graph growth",library_source_operation,&fixture);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    size_t resources_count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&resources_count);
    CHECK(resources&&resources_count==17);
    unsigned edges=0;const void *artifact=resources[0].checked;
    for(size_t i=0;i<resources_count;++i){CHECK(resources[i].checked==artifact&&resources[i].checked_module==i);edges+=resources[i].dependency_count;}
    CHECK(edges==16);
    xr_xir_compile_checked_packet_free(&packet);memset(bindings,0,sizeof(bindings));memset(storage,0,sizeof(storage));memset(&input,0,sizeof(input));
    library_views_growth_graph(catalog,authority);
    XrCompilerSession *consumer=library_session_new(library_context);
    XrXirSourceRequest request={consumer,XR_SOURCE_FIXTURES "/views_chain_root.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"views17 consumer %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirModule *checked=xr_xir_compile_artifact_module(result.checked);
    CHECK(checked->declarations->module_count==18&&checked->function_count==37);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(consumer);xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_source_run(owned);CHECK(!runtime_live&&!runtime_bytes);
    puts("views17 18modules37functions fixed41 twoInstances producerdeath graphGrowth PASS");
}
#endif // XIR_LIBRARY_VIEWS_GROWTH_CASES_H
