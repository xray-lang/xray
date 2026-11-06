/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static int resource_once(char **argv,unsigned method,size_t source_fault,size_t json_fault,bool report) {
    CHECK(method<3);hierarchy_axis_active=false;TypeFixture f;fixture(&f,argv,root_text);XrJsonValue *p=NULL;
    if(!method)p=params(argv[1],1,24);
    else{XrJsonValue *prepared=prepare(&f,method==1?3u:1u,method==1?11u:24u);p=item_params(xjson_array_get(prepared,0));xjson_free(prepared);}
    CHECK(!runtime_live&&!runtime_bytes);runtime_attempts=0;runtime_fail_at=source_fault;
    lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;hierarchy_last_stats=(XrCompileResourceStats){0};hierarchy_ledgers=0;
    hierarchy_axis_active=hierarchy_axis_limits!=NULL;
    XrJsonValue *out=method==0?xlsp_handle_prepare_type_hierarchy(&f.server,p):method==1?
        xlsp_handle_type_hierarchy_supertypes(&f.server,p):xlsp_handle_type_hierarchy_subtypes(&f.server,p);
    hierarchy_axis_active=false;XrXirStatus status=(XrXirStatus)f.server.source_failure.status;size_t source=runtime_attempts,json=lsp_json_calls;
    bool source_hit=source_fault!=SIZE_MAX&&source>source_fault,json_hit=lsp_json_injected;
    runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(hierarchy_axis_limits&&hierarchy_axis_budget)CHECK(status==XR_XIR_MEMORY_LIMIT&&!out);
    else if(source_fault==SIZE_MAX&&json_fault==SIZE_MAX){CHECK(status==XR_XIR_OK&&out&&out->type==XR_JSON_ARRAY&&out->as.array.count==(int)method+1);}
    else{CHECK(source_hit||json_hit);if(status!=XR_XIR_OUT_OF_MEMORY||out)fprintf(stderr,"Type resource method%u Source%zu JSON%zu status%u output%u\n",method,source_fault,json_fault,(unsigned)status,out?1u:0u);
        CHECK(status==XR_XIR_OUT_OF_MEMORY&&!out);}
    CHECK(hierarchy_ledgers==1&&!runtime_live&&!runtime_bytes);xjson_free(out);xjson_free(p);done(&f);CHECK(!lsp_json_live&&!lsp_json_bytes);
    if(report)printf("{\"method\":%u,\"status\":%u,\"source\":%zu,\"json\":%zu,\"allocated\":%llu,\"peak\":%llu,\"held\":%llu,\"source_hit\":%s,\"json_hit\":%s,\"ledgers\":%zu,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",
        method,(unsigned)status,source,json,(unsigned long long)hierarchy_last_stats.allocated_bytes,(unsigned long long)hierarchy_last_stats.peak_bytes,
        (unsigned long long)hierarchy_last_stats.held_bytes,source_hit?"true":"false",json_hit?"true":"false",hierarchy_ledgers,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);
    return 0;
}
static int resource_memory(char **argv) {
    unsigned method=(unsigned)strtoul(argv[5],NULL,10);CHECK(method<3);
    uint64_t peak=(uint64_t)strtoull(argv[6],NULL,10);CHECK(peak&&peak<=UINT64_C(8)*1024*1024);
    for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits exact={XR_COMPILE_MEMORY_FINITE,
        delta<0?peak-1:peak+(unsigned)delta,true};hierarchy_axis_limits=&exact;hierarchy_axis_budget=delta<0;
        CHECK(!resource_once(argv,method,SIZE_MAX,SIZE_MAX,false));hierarchy_axis_limits=NULL;hierarchy_axis_budget=false;}
    CHECK(!resource_once(argv,method,SIZE_MAX,SIZE_MAX,false));
    printf("{\"method\":%u,\"axis\":\"held\",\"checked\":3,\"recovered\":true,\"physical_live\":%zu,\"physical_bytes\":%zu,\"json_live\":%zu,\"json_bytes\":%zu}\n",method,runtime_live,runtime_bytes,lsp_json_live,lsp_json_bytes);return 0;
}
static int resource_chunk(int argc,char **argv) {
    CHECK(argc==10);unsigned method=(unsigned)strtoul(argv[5],NULL,10);bool source=!strcmp(argv[6],"source");CHECK(source||!strcmp(argv[6],"json"));
    size_t start=(size_t)strtoull(argv[7],NULL,10),count=(size_t)strtoull(argv[8],NULL,10),total=(size_t)strtoull(argv[9],NULL,10);
    CHECK(method<3&&count&&start<total&&count<=total-start);
    for(size_t i=0;i<count;++i)CHECK(!resource_once(argv,method,source?start+i:SIZE_MAX,source?SIZE_MAX:start+i,false));
    printf("{\"method\":%u,\"domain\":\"%s\",\"start\":%zu,\"count\":%zu,\"total\":%zu,\"checked\":%zu}\n",method,argv[6],start,count,total,count);return 0;
}
