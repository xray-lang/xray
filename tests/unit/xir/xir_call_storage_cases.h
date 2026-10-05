/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
static void source_call_storage_cases(void) {
    for(unsigned mode=0;mode<9;++mode){
        size_t payload=sizeof(SourceMemory)+sizeof(XrXirType)+sizeof(SourceValue);
        uint64_t bytes=sizeof(XrCompileResources)+sizeof(CompileAllocation)+payload;
        XrCompileResourceLimits caps={bytes,bytes,UINT64_C(128000000)};
        if(mode==1)--caps.allocated_bytes;if(mode==2)++caps.allocated_bytes;
        if(mode==7)--caps.live_bytes;if(mode==8)caps.work=payload+1;
        SourceContext ctx={0};CHECK(allocation_private_context(&ctx,caps));
        size_t start=source_fixture_compile_attempts;
        source_fixture_compile_fail_at=mode==3?start:SIZE_MAX;
        source_fixture_compile_injected=false;
        SourceCallStorage output={(XrXirType *)(uintptr_t)1,(SourceValue *)(uintptr_t)1};
        size_t types=mode==4?SIZE_MAX:mode==6?0:1;
        uint64_t values=mode==5?UINT64_MAX:mode==6?0:1;
        bool success=source_call_storage(&ctx,types,values,&output);
        CHECK(success==(mode==0 || mode==2 || mode==6));
        if(success && mode!=6){
            CHECK(output.types && output.values && ctx.memory && !ctx.memory->next);
            CHECK((unsigned char *)output.values==(unsigned char *)output.types+sizeof(XrXirType));
            CHECK(source_fixture_compile_attempts==start+1);
            XrCompileResourceStats fees={0};CHECK(xr_compile_resources_stats(ctx.compile.resources,&fees)==XR_COMPILE_RESOURCE_OK);
            CHECK(fees.allocated_bytes==bytes && fees.peak_bytes==bytes && fees.work==payload+2);
        }else{
            CHECK(!output.types && !output.values && !ctx.memory);
            if(!success)CHECK(ctx.diagnostic.status==(mode==3?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET));
        }
        allocation_private_close(&ctx);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if(mode==3)allocation_pure_point("call_storage",0);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
}
