/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_source_budget_cases.h - Shared Source metadata remaining budget
 *
 * KEY CONCEPT:
 *   Arena owners and imported verification consume the same remaining quota.
 */
static void library_source_metadata_cases(const XrXirLibraryCatalog *catalog) {
    size_t live=source_live,bytes=source_bytes,rlive=runtime_live,rbytes=runtime_bytes;
    const XrModuleResourceBinding *resource=xr_xir_library_catalog_resource(catalog);
    const XrXirModule *module=xr_xir_artifact_module(resource->checked);
    XrXirBudget budget=xr_xir_default_budget(),before=budget;
    CHECK(xr_xir_verify_remaining(module,&budget,NULL)==XR_XIR_OK);
    uint64_t fee=before.metadata_bytes-budget.metadata_bytes;
    CHECK(fee>sizeof(SourceMemory));
    for(unsigned tight=0;tight<2;++tight){
        XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
        XrXirBudget quota=xr_xir_default_budget();if(tight)quota.metadata_bytes=fee+1;
        XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&resource->authority,&quota,NULL,NULL,XR_XIR_PROGRAM,catalog};
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        source_peak=source_bytes;
        XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
        CHECK(source_peak-bytes<=quota.metadata_bytes);
        if(tight){CHECK(status==XR_XIR_BUDGET&&!result.checked&&!result.snapshot);
            CHECK(!strcmp(diagnostic.message,"Checked library verification failed"));}
        else CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
        xr_xir_source_result_free(&result);xr_compiler_session_delete(session);
        CHECK(source_live==live&&source_bytes==bytes&&runtime_live==rlive&&runtime_bytes==rbytes);
    }
    XrModuleSpec spec={0};spec.resource=resource;
    XrModuleGraph graph={0};graph.specs=&spec;graph.spec_count=1;
    SourceContext ctx={0};ctx.graph=&graph;ctx.budget=xr_xir_default_budget();
    ctx.budget.metadata_bytes=fee+1;
    CHECK(source_alloc(&ctx,1,1));
    CHECK(ctx.budget.metadata_bytes==fee-sizeof(SourceMemory));
    CHECK(!source_library_module(&ctx,0));
    CHECK(ctx.diagnostic.status==XR_XIR_BUDGET);
    CHECK(!strcmp(ctx.diagnostic.message,"Checked library verification failed"));
    CHECK(ctx.budget.metadata_bytes<=fee-sizeof(SourceMemory));
    while(ctx.memory){SourceMemory *next=ctx.memory->next;source_release_private(ctx.memory+1);ctx.memory=next;}
    CHECK(source_live==live&&source_bytes==bytes&&runtime_live==rlive&&runtime_bytes==rbytes);
    for(unsigned mode=0;mode<4;++mode){
        memset(&ctx,0,sizeof(ctx));ctx.budget=xr_xir_default_budget();
        uint64_t exact=sizeof(SourceMemory)+7;ctx.budget.metadata_bytes=exact;
        if(mode==1)--ctx.budget.metadata_bytes;
        if(mode==2)source_fail_at=source_attempts;
        uint64_t available=ctx.budget.metadata_bytes;
        void *owned=source_alloc(&ctx,mode==3?SIZE_MAX:7,1);
        source_fail_at=SIZE_MAX;
        if(!mode){
            CHECK(owned&&!ctx.budget.metadata_bytes);
            source_release_private(owned);CHECK(!ctx.memory&&!ctx.budget.metadata_bytes);
            CHECK(!source_alloc(&ctx,1,1)&&ctx.diagnostic.status==XR_XIR_BUDGET);
        }else{
            CHECK(!owned&&!ctx.memory&&ctx.budget.metadata_bytes==available);
            CHECK(ctx.diagnostic.status==(mode==2?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET));
        }
        CHECK(source_live==live&&source_bytes==bytes&&runtime_live==rlive&&runtime_bytes==rbytes);
    }
    printf("Source metadata shared verification fee=%llu exact/minus1/OOM/overflow/privatefree PASS\n",(unsigned long long)fee);
}
