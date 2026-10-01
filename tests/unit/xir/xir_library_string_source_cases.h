/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_string_source_cases.h - Literal inventory budget and remap boundaries
 *
 * KEY CONCEPT:
 *   Declaration collection never emits expressions or borrows library bytes.
 */
static void library_string_source_boundaries(void) {
    size_t live=source_live,bytes=source_bytes;
    for(unsigned mode=0;mode<6;++mode){
        SourceContext ctx={0};ctx.budget=xr_xir_default_budget();
        ctx.budget.metadata_bytes=sizeof(SourceMemory)+16*sizeof(XrXirLiteral);
        ctx.budget.work=1;uint64_t before=ctx.budget.metadata_bytes;uint32_t id=99;
        if(mode==1)--ctx.budget.metadata_bytes;
        if(mode==2)ctx.budget.work=0;
        if(mode==3)source_fail_at=source_attempts;
        if(mode==4)ctx.literal_count=UINT32_MAX;
        bool ok=source_literal_append(&ctx,NULL,mode==5?NULL:"",mode==5?1:0,&id);
        source_fail_at=SIZE_MAX;
        CHECK(ok==(mode==0));
        if(ok){CHECK(id==0&&ctx.literal_count==1&&!ctx.budget.metadata_bytes&&!ctx.budget.work);
            CHECK(ctx.literals[0].length==0&&!ctx.functions);}
        else {CHECK(id==99&&ctx.literal_count==(mode==4?UINT32_MAX:0));
            CHECK(ctx.diagnostic.status==(mode==3?XR_XIR_OUT_OF_MEMORY:mode==5?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET));
            CHECK(ctx.budget.metadata_bytes==before-(mode==1?1:0));}
        while(ctx.memory)source_release_private(ctx.memory+1);
        CHECK(source_live==live&&source_bytes==bytes);
    }
    SourceContext ctx={0};ctx.budget=xr_xir_default_budget();
    SourceLibraryMap map={0};map.literal_begin=100;map.literal_count=1;
    XrXirInstruction op={XR_XIR_CONST_STRING,XR_XIR_STRING,{0},{0},1,{0}},output={0};
    CHECK(!source_library_instruction(&ctx,&map,&op,&output));
    CHECK(ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE&&!strcmp(ctx.diagnostic.message,"library literal identity is invalid"));
    CHECK(source_live==live&&source_bytes==bytes);
}

static void library_string_growth_boundaries(void) {
    size_t live=source_live,bytes=source_bytes;
    for(unsigned mode=0;mode<6;++mode){
        SourceContext ctx={0};ctx.budget=xr_xir_default_budget();XrXirLiteral original[16];
        for(unsigned i=0;i<16;++i)original[i]=(XrXirLiteral){"owned",5};
        ctx.literals=original;ctx.literal_count=16;ctx.literal_capacity=16;
        ctx.budget.work=17;ctx.budget.metadata_bytes=sizeof(SourceMemory)+32*sizeof(XrXirLiteral);
        if(mode==1)--ctx.budget.work;if(mode==2)--ctx.budget.metadata_bytes;
        if(mode==4)ctx.literal_count=ctx.literal_capacity=UINT32_MAX/2+1;
        uint32_t id=99;uint64_t metadata=ctx.budget.metadata_bytes;
        bool ok=source_literal_append(&ctx,NULL,"",mode==3?(size_t)UINT32_MAX+1:0,mode==5?NULL:&id);
        CHECK(ok==(mode==0));
        if(ok){CHECK(id==16&&ctx.literal_count==17&&ctx.literal_capacity==32&&!ctx.budget.work&&!ctx.budget.metadata_bytes);
            CHECK(ctx.literals!=original&&!memcmp(ctx.literals,original,sizeof(original)));}
        else {CHECK(id==99&&ctx.literals==original&&ctx.budget.metadata_bytes==metadata);
            CHECK(ctx.literal_count==(mode==4?UINT32_MAX/2+1:16));
            CHECK(ctx.diagnostic.status==(mode==5?XR_XIR_BAD_STRUCTURE:XR_XIR_BUDGET));}
        while(ctx.memory)source_release_private(ctx.memory+1);
        CHECK(source_live==live&&source_bytes==bytes);
    }
    puts("Library STRING inventory growth17/exact/minus1/overflow PASS");
}
static void library_string_source_budgets(const XrXirSourceRequest *request) {
    size_t live=source_live,bytes=source_bytes,rlive=runtime_live,rbytes=runtime_bytes;
    for(unsigned dimension=0;dimension<2;++dimension){
        XrXirBudget full=xr_xir_default_budget();uint64_t lo=0,hi=dimension?full.metadata_bytes:full.work;unsigned rounds=0;
        while(lo<hi){CHECK(++rounds<=64);uint64_t mid=lo+(hi-lo)/2;XrXirBudget budget=full;
            if(dimension)budget.metadata_bytes=mid;else budget.work=mid;
            XrXirSourceRequest limited=*request;limited.budget=&budget;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
            source_peak=source_bytes;
            XrXirStatus status=xr_xir_source_check(&limited,&result,&diagnostic);
            CHECK(source_peak>=bytes&&source_peak-bytes<=budget.metadata_bytes);
            CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
            if(status==XR_XIR_OK){CHECK(result.checked&&result.snapshot);hi=mid;}else{CHECK(!result.checked&&!result.snapshot);lo=mid+1;}
            xr_xir_source_result_free(&result);CHECK(source_live==live&&source_bytes==bytes&&runtime_live==rlive&&runtime_bytes==rbytes);
        }
        CHECK(lo);for(unsigned less=0;less<2;++less){XrXirBudget budget=full;
            if(dimension)budget.metadata_bytes=lo-less;else budget.work=lo-less;
            XrXirSourceRequest limited=*request;limited.budget=&budget;XrXirSourceResult result={0};
            source_peak=source_bytes;
            CHECK(xr_xir_source_check(&limited,&result,NULL)==(less?XR_XIR_BUDGET:XR_XIR_OK));
            CHECK(source_peak>=bytes&&source_peak-bytes<=budget.metadata_bytes);
            printf("Source peak dimension%u less%u actual%zu quota%llu\n",dimension,less,source_peak-bytes,(unsigned long long)budget.metadata_bytes);
            CHECK(less?(!result.checked&&!result.snapshot):(result.checked&&result.snapshot));xr_xir_source_result_free(&result);
            CHECK(source_live==live&&source_bytes==bytes&&runtime_live==rlive&&runtime_bytes==rbytes);}
        printf("Library STRING Source dimension%u exact%llu/minus1 PASS\n",dimension,(unsigned long long)lo);
    }
}
static void library_string_source_negatives(const XrXirSourceRequest *request) {
    const char *names[]={"private.xr","nul_escape.xr","nul_unicode.xr"};
    size_t live=source_live,bytes=source_bytes,rlive=runtime_live,rbytes=runtime_bytes;
    for(unsigned i=0;i<3;++i){char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s",XR_SOURCE_FIXTURES,names[i])>0);
        XrXirSourceRequest negative=*request;negative.entry_path=path;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(&negative,&result,&diagnostic);
        fprintf(stderr,"negative %s status%u reason%s\n",names[i],status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_STRUCTURE&&!result.checked);
        if(result.snapshot)CHECK(!xr_xir_source_snapshot_view(result.snapshot)->complete);
        if(!i)CHECK(!strcmp(diagnostic.message,"import requires an exported declaration"));
        else {char expected[1200];CHECK(snprintf(expected,sizeof(expected),"failed to parse module: %s",path)>0);
#if XR_OS_WINDOWS
            for(char *cursor=expected;*cursor;++cursor)if(*cursor=='/')*cursor='\\';
#endif
            CHECK(!strcmp(diagnostic.message,expected));}
        xr_xir_source_result_free(&result);CHECK(source_live==live&&source_bytes==bytes&&runtime_live==rlive&&runtime_bytes==rbytes);
    }
}
