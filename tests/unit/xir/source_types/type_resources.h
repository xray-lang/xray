/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static int type_resource_once(const char *path,unsigned mode,XrCompileResourceLimits bounds,size_t fault,XrXirStatus expected,bool report) {
    CHECK(mode<4&&!runtime_live&&!runtime_bytes);runtime_attempts=0;runtime_fail_at=fault;type_ledgers=0;
    XrCompileResourceStats stats={0};XrXirSourceSnapshot *snapshot=NULL;XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=type_program(path,mode,bounds,&stats,&snapshot,&diagnostic);size_t calls=runtime_attempts;
    bool hit=fault!=SIZE_MAX&&calls>fault;runtime_fail_at=SIZE_MAX;
    if(status!=expected)fprintf(stderr,"Type resource mode=%u fault=%zu expected=%u actual=%u: %s\n",mode,fault,(unsigned)expected,(unsigned)status,diagnostic.message);
    CHECK(status==expected&&type_ledgers==1);if(fault!=SIZE_MAX)CHECK(hit);
    if(status==XR_XIR_OK){CHECK(snapshot);inspect(xr_xir_compile_source_snapshot_view(snapshot),xr_xir_compile_source_snapshot_type_facts(snapshot),mode);}
    else CHECK(!snapshot);
    xr_xir_compile_source_snapshot_free(snapshot);CHECK(!runtime_live&&!runtime_bytes);
    if(report)printf("{\"mode\":%u,\"status\":%u,\"source\":%zu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,\"fault_hit\":%s,\"ledgers\":%zu,\"physical_live\":%zu,\"physical_bytes\":%zu}\n",
        mode,(unsigned)status,calls,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work,
        hit?"true":"false",type_ledgers,runtime_live,runtime_bytes);
    return 0;
}
static int type_resource_command(int argc,char **argv) {
    unsigned mode=(unsigned)strtoul(argv[2],NULL,10);CHECK(mode<4);
    if(argc==8){CHECK(!strcmp(argv[3],"--resource"));size_t fault=(size_t)strtoull(argv[4],NULL,10);
        XrCompileResourceLimits bounds={(uint64_t)strtoull(argv[5],NULL,10),(uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10)};
        return type_resource_once(argv[1],mode,bounds,fault,fault==SIZE_MAX?XR_XIR_OK:XR_XIR_OUT_OF_MEMORY,true);}
    CHECK(argc==7);
    if(!strcmp(argv[3],"--chunk")){size_t start=(size_t)strtoull(argv[4],NULL,10),count=(size_t)strtoull(argv[5],NULL,10),total=(size_t)strtoull(argv[6],NULL,10);
        CHECK(count&&start<total&&count<=total-start);
        for(size_t i=0;i<count;++i)CHECK(!type_resource_once(argv[1],mode,type_limits,start+i,XR_XIR_OUT_OF_MEMORY,false));
        printf("{\"mode\":%u,\"start\":%zu,\"count\":%zu,\"total\":%zu,\"checked\":%zu}\n",mode,start,count,total,count);return 0;}
    CHECK(!strcmp(argv[3],"--axes"));XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[4],NULL,10),(uint64_t)strtoull(argv[5],NULL,10),(uint64_t)strtoull(argv[6],NULL,10)};
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits exact=baseline;
        uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(!type_resource_once(argv[1],mode,exact,SIZE_MAX,delta<0?XR_XIR_BUDGET:XR_XIR_OK,false));}
    printf("{\"mode\":%u,\"axes\":3,\"checked\":9,\"physical_live\":%zu,\"physical_bytes\":%zu}\n",mode,runtime_live,runtime_bytes);return 0;
}
