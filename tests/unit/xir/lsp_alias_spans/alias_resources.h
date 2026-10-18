/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void alias_reset(size_t source_fault,size_t json_fault) {
    alias_ledgers=0;runtime_attempts=0;runtime_fail_at=source_fault;lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;
}
static void alias_resources(unsigned method,unsigned mode,bool full) {
    alias_reset(SIZE_MAX,SIZE_MAX);XrCompileResourceStats needed={0};XrXirStatus status=alias_once(method,mode,limits,&needed);
    if(status!=XR_XIR_OK)fprintf(stderr,"Alias method%u mode%u baseline status%u\n",method,mode,(unsigned)status);CHECK(status==XR_XIR_OK);
    size_t source=runtime_attempts,json=lsp_json_calls;
    if(full){
        for(size_t prefix=0;prefix<source;++prefix){alias_reset(prefix,SIZE_MAX);status=alias_once(method,mode,limits,NULL);
            if(status!=XR_XIR_OUT_OF_MEMORY)fprintf(stderr,"Alias method%u mode%u Source%zu status%u\n",method,mode,prefix,(unsigned)status);
            CHECK(runtime_attempts>prefix&&status==XR_XIR_OUT_OF_MEMORY);}
        for(size_t prefix=0;prefix<json;++prefix){alias_reset(SIZE_MAX,prefix);status=alias_once(method,mode,limits,NULL);
            CHECK(lsp_json_injected&&status==XR_XIR_OUT_OF_MEMORY);}
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){alias_reset(SIZE_MAX,SIZE_MAX);
            XrCompileResourceLimits exact={needed.allocated_bytes,needed.peak_bytes,needed.work};
            uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);
            *bound=(uint64_t)((int64_t)*bound+delta);CHECK(alias_once(method,mode,exact,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}
    }
    alias_reset(SIZE_MAX,SIZE_MAX);
    printf("{\"method\":%u,\"mode\":%u,\"source\":%zu,\"json\":%zu,\"full\":%s,\"boundaries\":%u,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,\"ledgers\":1,\"physical_live\":0,\"physical_bytes\":0,\"json_live\":0,\"json_bytes\":0}\n",
        method,mode,source,json,full?"true":"false",full?9u:0u,(unsigned long long)needed.allocated_bytes,(unsigned long long)needed.peak_bytes,(unsigned long long)needed.work);
}
