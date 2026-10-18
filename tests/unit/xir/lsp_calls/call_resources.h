/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static int resource_once(char **argv,unsigned method,size_t source_fault,size_t json_fault,bool report) {
    CHECK(method<3);call_axis_active=false;CallFixture f;call_fixture(&f,argv,root_text);XrJsonValue *params=NULL;
    if(!method)params=prepare_params(argv[1],2,8);
    else {XrJsonValue *prepared=call_prepared(&f,method==1?2u:1u,method==1?8u:3u);params=item_params(xjson_array_get(prepared,0));xjson_free(prepared);}
    CHECK(!runtime_live&&!runtime_bytes);runtime_attempts=0;runtime_fail_at=source_fault;
    lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;call_last_stats=(XrCompileResourceStats){0};call_ledgers=0;
    call_axis_active=call_axis_limits!=NULL;
    XrJsonValue *result=method==0?xlsp_handle_prepare_call_hierarchy(&f.server,params):method==1?
        xlsp_handle_call_hierarchy_incoming(&f.server,params):xlsp_handle_call_hierarchy_outgoing(&f.server,params);
    XrXirStatus status=(XrXirStatus)f.server.source_failure.status;size_t source=runtime_attempts,json=lsp_json_calls;
    call_axis_active=false;
    bool source_hit=source_fault!=SIZE_MAX&&source>source_fault,json_hit=lsp_json_injected;
    runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(call_axis_limits&&call_axis_budget){CHECK(status==XR_XIR_BUDGET&&!result);}
    else if(source_fault==SIZE_MAX&&json_fault==SIZE_MAX){CHECK(status==XR_XIR_OK&&result&&result->type==XR_JSON_ARRAY);
        CHECK(result->as.array.count==(method==1?2:1));}
    else {CHECK(source_hit||json_hit);
        if(status!=XR_XIR_OUT_OF_MEMORY||result)fprintf(stderr,"Call resource method=%u Source=%zu JSON=%zu status=%u result=%u\n",method,source_fault,json_fault,(unsigned)status,result?1u:0u);
        CHECK(status==XR_XIR_OUT_OF_MEMORY&&!result);}
    CHECK(call_ledgers==1&&!runtime_live&&!runtime_bytes);
    xjson_free(result);xjson_free(params);call_fixture_done(&f);CHECK(!lsp_json_live&&!lsp_json_bytes);
    if(report)printf("{\"method\":%u,\"status\":%u,\"source\":%zu,\"json\":%zu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,\"source_hit\":%s,\"json_hit\":%s,\"ledgers\":%zu,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",
        method,(unsigned)status,source,json,(unsigned long long)call_last_stats.allocated_bytes,(unsigned long long)call_last_stats.peak_bytes,
        (unsigned long long)call_last_stats.work,source_hit?"true":"false",json_hit?"true":"false",call_ledgers,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);
    return 0;
}
static int resource_axes(char **argv) {
    unsigned method=(unsigned)strtoul(argv[5],NULL,10);CHECK(method<3);
    XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10),(uint64_t)strtoull(argv[8],NULL,10)};
    CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits exact=baseline;
        uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);
        *bound=(uint64_t)((int64_t)*bound+delta);call_axis_limits=&exact;call_axis_budget=delta<0;
        CHECK(!resource_once(argv,method,SIZE_MAX,SIZE_MAX,false));call_axis_limits=NULL;call_axis_budget=false;}
    printf("{\"method\":%u,\"axes\":3,\"checked\":9,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",method,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);return 0;
}
static int resource_chunk(int argc,char **argv) {
    CHECK(argc==10);unsigned method=(unsigned)strtoul(argv[5],NULL,10);bool source=!strcmp(argv[6],"source");CHECK(source||!strcmp(argv[6],"json"));
    size_t start=(size_t)strtoull(argv[7],NULL,10),count=(size_t)strtoull(argv[8],NULL,10),total=(size_t)strtoull(argv[9],NULL,10);
    CHECK(method<3&&count&&start<total&&count<=total-start);
    for(size_t i=0;i<count;++i)CHECK(!resource_once(argv,method,source?start+i:SIZE_MAX,source?SIZE_MAX:start+i,false));
    printf("{\"method\":%u,\"domain\":\"%s\",\"start\":%zu,\"count\":%zu,\"total\":%zu,\"checked\":%zu}\n",method,argv[6],start,count,total,count);return 0;
}
