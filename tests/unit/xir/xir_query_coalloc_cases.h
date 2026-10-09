/*
 * xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License.
 * Query coallocation has independent storage/work and early-access oracles.
 */
static XrCompileResourceStats query_fees(const XrXirCompileContext *ctx) {
    XrCompileResourceStats fees={0};CHECK(xr_compile_resources_stats(ctx->resources,&fees)==XR_COMPILE_RESOURCE_OK);return fees;
}
static void query_admission_cases(void) {
    CHECK(xr_compile_resources_admit(NULL,1,0)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    for(unsigned axis=0;axis<3;++axis)for(unsigned less=0;less<2;++less){
        XrCompileResourceLimits caps=allocation_limits();
        uint64_t exact=axis==2?66:sizeof(XrCompileResources)+sizeof(CompileAllocation)+64;
        uint64_t *limit=axis==0?&caps.allocated_bytes:axis==1?&caps.live_bytes:&caps.work;*limit=exact-less;
        XrXirCompileContext ctx=allocation_context(caps);XrCompileResourceStats before=query_fees(&ctx);
        size_t start=source_fixture_compile_attempts;
        for(unsigned repeat=0;repeat<4;++repeat){
            CHECK(xr_compile_resources_admit(ctx.resources,64,64)==(less?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OK));
            XrCompileResourceStats after=query_fees(&ctx);CHECK(!memcmp(&before,&after,sizeof(before)) && source_fixture_compile_attempts==start);
        }
        void *out=NULL;CHECK(xr_compile_resources_calloc(ctx.resources,1,64,&out)==(less?XR_COMPILE_RESOURCE_BUDGET:XR_COMPILE_RESOURCE_OK));
        if(!less){CHECK(out && source_fixture_compile_attempts==start+1 && query_fees(&ctx).work==66);xr_compile_resources_free(out);}
        else CHECK(!out && source_fixture_compile_attempts==start);
        allocation_context_close(&ctx);CHECK(!source_fixture_compile_live);
    }
    XrXirCompileContext ctx=allocation_context(allocation_limits());XrCompileResourceStats before=query_fees(&ctx);
    size_t start=source_fixture_compile_attempts;
    CHECK(xr_compile_resources_admit(ctx.resources,0,0)==XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(xr_compile_resources_admit(ctx.resources,SIZE_MAX,0)==XR_COMPILE_RESOURCE_BUDGET);
    CHECK(xr_compile_resources_admit(ctx.resources,1,UINT64_MAX)==XR_COMPILE_RESOURCE_BUDGET);
    uint64_t refs=ctx.resources->references,count=ctx.resources->stats.allocation_count;
    ctx.resources->references=UINT64_MAX;CHECK(xr_compile_resources_admit(ctx.resources,1,0)==XR_COMPILE_RESOURCE_BUDGET);ctx.resources->references=refs;
    ctx.resources->stats.allocation_count=UINT64_MAX;CHECK(xr_compile_resources_admit(ctx.resources,1,0)==XR_COMPILE_RESOURCE_BUDGET);ctx.resources->stats.allocation_count=count;
    XrCompileResourceStats after=query_fees(&ctx);CHECK(!memcmp(&before,&after,sizeof(before)) && source_fixture_compile_attempts==start);
    allocation_context_close(&ctx);CHECK(!source_fixture_compile_live);
    ctx=allocation_context((XrCompileResourceLimits){1048576,1048576,66});
    CHECK(xr_compile_resources_admit(ctx.resources,64,64)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_work(ctx.resources,1)==XR_COMPILE_RESOURCE_OK);
    start=source_fixture_compile_attempts;void *out=NULL;
    CHECK(xr_compile_resources_calloc(ctx.resources,1,64,&out)==XR_COMPILE_RESOURCE_BUDGET && !out && source_fixture_compile_attempts==start);
    allocation_context_close(&ctx);CHECK(!source_fixture_compile_live);
}
static void query_coalloc_boundaries(void) {
    source_fixture_compile_fail_at=SIZE_MAX;query_admission_cases();
    XrXirCompileContext ctx=allocation_context(allocation_limits());
    XrXirSourceSnapshot holder={0};holder.context=ctx;SourceQueryCopy probe={&holder,XR_XIR_OK};
    XrXirSourceDeclaration empty={0};size_t extent=0;
    CHECK(query_declaration_extent(&probe,&empty,&extent) && !extent);
    extent=SIZE_MAX;CHECK(!query_declaration_extent(&probe,&empty,&extent));
    XrXirSourceType parameter={0};empty.parameter_count=1;empty.parameters=&parameter;extent=SIZE_MAX-8;
    CHECK(!query_declaration_extent(&probe,&empty,&extent));
    allocation_context_close(&ctx);CHECK(!source_fixture_compile_live);
    ctx=allocation_context((XrCompileResourceLimits){1048576,1048576,1});holder.context=ctx;
    empty=(XrXirSourceDeclaration){0};empty.name=(const char *)(uintptr_t)1;extent=0;probe.status=XR_XIR_OK;
    CHECK(!query_declaration_extent(&probe,&empty,&extent) && probe.status==XR_XIR_BUDGET);
    allocation_context_close(&ctx);CHECK(!source_fixture_compile_live);
    char name[]="map",signature[]="(i64)->i64";XrXirSourceType parameters[]={{XR_XIR_I64,73,true}};
    XrXirSourceDeclaration declaration={0};declaration.name=name;declaration.signature=signature;
    declaration.parameters=parameters;declaration.parameter_count=1;
    XrXirSourceView view={0};view.declarations=&declaration;view.declaration_count=1;
    uint64_t table=sizeof(declaration)+sizeof(parameters)+sizeof(name)+sizeof(signature);
    uint64_t bytes=sizeof(XrCompileResources)+4*sizeof(CompileAllocation)+2*sizeof(XrXirConstruction)+sizeof(XrXirSourceSnapshot)+sizeof(SourceQueryMemory)+table;
    uint64_t work=11+2*sizeof(XrXirConstruction)+sizeof(XrXirSourceSnapshot)+sizeof(view)+sizeof(SourceQueryMemory)+table+sizeof(declaration)+sizeof(parameters)+3*(sizeof(name)+sizeof(signature));
    XrCompileResourceLimits caps={bytes,bytes,work};XrXirSourceSnapshot *snapshot=NULL;
    size_t start=source_fixture_compile_attempts;
    CHECK(allocation_snapshot_copy(&view,&caps,&snapshot)==XR_XIR_OK);
    size_t sites=source_fixture_compile_attempts-start;CHECK(sites==5);
    CHECK(allocation_last_stats.allocated_bytes==bytes && allocation_last_stats.peak_bytes==bytes && allocation_last_stats.work==work);
    memset(name,'x',sizeof(name)-1);memset(signature,'x',sizeof(signature)-1);parameters[0].generic_owner=99;
    const XrXirSourceDeclaration *copy=xr_xir_compile_source_snapshot_view(snapshot)->declarations;
    CHECK(!strcmp(copy->name,"map") && !strcmp(copy->signature,"(i64)->i64"));
    CHECK(copy->parameters[0].generic_owner==73 && copy->parameters!=parameters);
    xr_xir_compile_source_snapshot_free(snapshot);snapshot=NULL;CHECK(!source_fixture_compile_live);
    allocation_snapshot_boundaries(&view,(XrCompileResourceStats){5,bytes,0,bytes,work});
    for(size_t i=0;i<sites;++i){
        source_fixture_compile_fail_at=source_fixture_compile_attempts+i;
        source_fixture_compile_injected=false;
        CHECK(allocation_snapshot_copy(&view,&caps,&snapshot)==XR_XIR_OUT_OF_MEMORY && !snapshot && !source_fixture_compile_live);
        allocation_pure_point("map_snapshot",i);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    declaration.name=NULL;declaration.signature=NULL;declaration.parameters=NULL;
    caps=allocation_limits();start=source_fixture_compile_attempts;
    CHECK(allocation_snapshot_copy(&view,&caps,&snapshot)==XR_XIR_BAD_STRUCTURE && !snapshot && !source_fixture_compile_live);
    XrXirSourceType unknown={0};declaration.parameters=&unknown;
    CHECK(allocation_snapshot_copy(&view,&caps,&snapshot)==XR_XIR_OK);
    copy=xr_xir_compile_source_snapshot_view(snapshot)->declarations;
    CHECK(!copy->name && !copy->signature && copy->parameters && !copy->parameters[0].known && !copy->parameters[0].generic_owner);
    xr_xir_compile_source_snapshot_free(snapshot);snapshot=NULL;CHECK(!source_fixture_compile_live);
    declaration.parameter_count=0;declaration.parameters=parameters;start=source_fixture_compile_attempts;
    CHECK(allocation_snapshot_copy(&view,&caps,&snapshot)==XR_XIR_OK);
    CHECK(source_fixture_compile_attempts-start==5 && !xr_xir_compile_source_snapshot_view(snapshot)->declarations[0].parameters);
    xr_xir_compile_source_snapshot_free(snapshot);snapshot=NULL;CHECK(!source_fixture_compile_live);
    ctx=allocation_context(allocation_limits());snapshot=(XrXirSourceSnapshot *)(uintptr_t)1;start=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(&ctx,&view,NULL,&snapshot)==XR_XIR_BAD_STRUCTURE && snapshot==(XrXirSourceSnapshot *)(uintptr_t)1 && source_fixture_compile_attempts==start);
    snapshot=NULL;allocation_context_close(&ctx);CHECK(!source_fixture_compile_live);
    XrXirConstraint constraint={0};XrXirSourceDeclaration rows[2]={0};
    rows[0].name="x";rows[0].signature="odd!";rows[0].generic_constraints=&constraint;rows[0].generic_parameter_count=1;
    rows[1].name="y";rows[1].parameters=parameters;rows[1].parameter_count=1;
    view.declarations=rows;view.declaration_count=2;sites=0;
    for(size_t i=0;i<=sites;++i){
        start=source_fixture_compile_attempts;source_fixture_compile_fail_at=i?start+i-1:SIZE_MAX;
        source_fixture_compile_injected=false;
        XrXirStatus status=allocation_snapshot_copy(&view,&caps,&snapshot);
        CHECK(status==(i?XR_XIR_OUT_OF_MEMORY:XR_XIR_OK));
        if(!i){
            sites=source_fixture_compile_attempts-start;CHECK(sites==6);
            copy=xr_xir_compile_source_snapshot_view(snapshot)->declarations;
            CHECK((uintptr_t)copy[1].parameters%_Alignof(XrXirSourceType)==0);
            CHECK(copy[0].generic_constraints!=&constraint && copy[1].parameters!=parameters && copy[1].parameters[0].generic_owner==99);
        }else CHECK(!snapshot);
        xr_xir_compile_source_snapshot_free(snapshot);snapshot=NULL;CHECK(!source_fixture_compile_live);
        if(i)allocation_pure_point("aligned_snapshot",i-1);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
}
static void query_coalloc_early_boundaries(void) {
    for(unsigned role=0;role<5;++role){
        size_t table=2*sizeof(XrXirSourceDeclaration),payload=sizeof(SourceQueryMemory)+table;
        XrCompileResourceLimits caps=allocation_limits();
        if(role==0)caps.allocated_bytes=sizeof(XrCompileResources)+sizeof(CompileAllocation)+payload-1;
        if(role==1)caps.live_bytes=sizeof(XrCompileResources)+sizeof(CompileAllocation)+payload-1;
        if(role==2)caps.work=3;
        if(role==3)caps.allocated_bytes=sizeof(XrCompileResources)+sizeof(CompileAllocation)+payload+1;
        if(role==4)caps.work=6;
        XrXirCompileContext ctx=allocation_context(caps);XrCompileResourceStats before=query_fees(&ctx);
        XrXirSourceSnapshot holder={0};holder.context=ctx;SourceQueryCopy copy={&holder,XR_XIR_OK};
        XrXirSourceView view={0};view.declaration_count=2;view.declarations=(const XrXirSourceDeclaration *)(uintptr_t)1;
        XrXirSourceDeclaration rows[2]={0};rows[0].name="a";rows[1].name=(const char *)(uintptr_t)1;
        if(role>=3)view.declarations=rows;
        size_t start=source_fixture_compile_attempts;query_declarations(&copy,&view);
        CHECK(copy.status==XR_XIR_BUDGET && !holder.memory && !holder.view.declarations && source_fixture_compile_attempts==start);
        XrCompileResourceStats after=query_fees(&ctx);
        CHECK(after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes && after.work==before.work+(role==3?3:0));
        allocation_context_close(&ctx);CHECK(!source_fixture_compile_live);
    }
    XrXirSourceDeclaration rows[2]={0};rows[0].name="x";rows[0].signature="odd!";rows[1].name="y";
    XrXirSourceView view={0};view.declarations=rows;view.declaration_count=2;
    uint64_t table=2*sizeof(XrXirSourceDeclaration),payload=sizeof(SourceQueryMemory)+table+9;
    uint64_t bytes=sizeof(XrCompileResources)+4*sizeof(CompileAllocation)+2*sizeof(XrXirConstruction)+sizeof(XrXirSourceSnapshot)+payload;
    uint64_t work=13+2*sizeof(XrXirConstruction)+sizeof(XrXirSourceSnapshot)+sizeof(view)+payload+table+27;
    XrCompileResourceLimits caps={bytes,bytes,work};XrXirSourceSnapshot *snapshot=NULL;
    CHECK(allocation_snapshot_copy(&view,&caps,&snapshot)==XR_XIR_OK);
    CHECK(allocation_last_stats.allocated_bytes==bytes && allocation_last_stats.work==work);
    const XrXirSourceDeclaration *copy=xr_xir_compile_source_snapshot_view(snapshot)->declarations;
    CHECK(!copy[0].parameters && !copy[1].parameters);
    CHECK(copy[1].name==copy[0].signature+5 && !strcmp(copy[1].name,"y"));
    xr_xir_compile_source_snapshot_free(snapshot);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
