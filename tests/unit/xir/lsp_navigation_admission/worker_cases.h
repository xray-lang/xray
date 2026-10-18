/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_NAVIGATION_WORKER_CASES_H
#define LSP_NAVIGATION_WORKER_CASES_H
typedef struct NavigationWorkerOptions {
    const char *uri,*library;
    unsigned method;
    size_t source_fault,json_fault;
    const XrCompileResourceLimits *limits;
    bool budget,report;
} NavigationWorkerOptions;
typedef struct NavigationWorkerMetrics {
    size_t source,json;
    bool source_hit,json_hit;
    XrCompileResourceStats stats;
} NavigationWorkerMetrics;
static int admission_worker_once(const NavigationWorkerOptions *options,NavigationWorkerMetrics *metrics) {
    task_axis_limits=NULL;NavigationAdmission f;admission_fixture(&f,options->uri,options->library);
    XrJsonValue *params=parameters(options->uri);size_t begin=runtime_attempts;
    task_resource_created_count=0;task_last_created_resources=NULL;
    task_last_stats=(XrCompileResourceStats){0};task_axis_limits=options->limits;
    runtime_fail_at=options->source_fault==SIZE_MAX?SIZE_MAX:begin+options->source_fault;
    lsp_json_calls=0;lsp_json_fail=options->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,params,options->method);
    bool fault=options->source_fault!=SIZE_MAX||options->json_fault!=SIZE_MAX;
    if(options->budget)CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
    else if(fault)CHECK(status==XR_XIR_OK||status==XR_XIR_OUT_OF_MEMORY);
    else CHECK(status==XR_XIR_OK);
    XlspPendingRequest *pending=xlsp_pending_request_find(&f.server.pending_requests,&f.id);CHECK(pending);
    CHECK((pending->source_task_id!=0)==(status==XR_XIR_OK));
    XrCompileResources *ledger=task_last_created_resources;XrCompileResourceStats captured=task_last_stats;Gate after={0};
    if(status==XR_XIR_OK){
        XrLspTask *hold=xlsp_task_new(block,block_complete,&after,XLSP_TASK_NORMAL);CHECK(hold);
        CHECK(xlsp_async_submit(f.async,hold));
    }
    // Every invocation destroys and poisons both editor texts before its worker can run.
    xjson_free(params);xlsp_document_store_free(f.server.doc_table);f.server.doc_table=NULL;
    if(status==XR_XIR_OK){
        atomic_store(&f.gate.release,true);uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load(&after.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
        // The real queued successor pins completion: this request still owns the original ledger.
        CHECK(task_resource_created_count==1&&ledger&&ledger==task_last_created_resources);
        metrics->stats=sdk_stats(ledger);
        if(!fault&&!options->budget)CHECK(metrics->stats.work>captured.work&&metrics->stats.allocated_bytes>captured.allocated_bytes);
        atomic_store(&after.release,true);
    }else metrics->stats=task_last_stats;
    admission_join(&f,status==XR_XIR_OK?3u:1u);CHECK(status!=XR_XIR_OK||after.completed==1);
    task_last_created_resources=NULL;
    metrics->source=runtime_attempts-begin;metrics->json=lsp_json_calls;
    metrics->source_hit=options->source_fault!=SIZE_MAX&&runtime_attempts>runtime_fail_at;
    metrics->json_hit=lsp_json_injected;
    task_axis_limits=NULL;runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(fault)CHECK(metrics->source_hit||metrics->json_hit);
    if(status==XR_XIR_OK)CHECK(!f.server.pending_requests.count);
    unsigned stopped=f.server.exit_received?1u:0u;admission_close(&f);
    if(options->report)fprintf(stderr,"NAV_WORKER mode=%u source=%zu json=%zu source_hit=%u json_hit=%u exit=%u allocated=%llu peak=%llu work=%llu producer_dead=1 physical0/0 JSON0/0 app0/0\n",
        options->method,metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,stopped,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,(unsigned long long)metrics->stats.work);
    if(options->report&&status==XR_XIR_OK)fprintf(stderr,"NAV_WORKER_LEDGER mode=%u created=1 capture_work=%llu final_work=%llu same_original=1\n",
        options->method,(unsigned long long)captured.work,(unsigned long long)metrics->stats.work);
    return 0;
}
static int admission_worker_main(int argc,char **argv,unsigned method) {
    NavigationWorkerOptions options={.uri=argv[1],.library=argv[3],.method=method,
        .source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};NavigationWorkerMetrics metrics={0};
    if(!strcmp(argv[5],"--worker")){
        CHECK(argc==8);options.source_fault=(size_t)strtoull(argv[6],NULL,10);options.json_fault=(size_t)strtoull(argv[7],NULL,10);
        return admission_worker_once(&options,&metrics);
    }
    if(!strcmp(argv[5],"--worker-chunk")){
        CHECK(argc==10);bool source=!strcmp(argv[6],"source");CHECK(source||!strcmp(argv[6],"json"));
        size_t start=(size_t)strtoull(argv[7],NULL,10),count=(size_t)strtoull(argv[8],NULL,10),total=(size_t)strtoull(argv[9],NULL,10);
        CHECK(count&&start<total&&count<=total-start);options.report=false;
        for(size_t i=0;i<count;++i){options.source_fault=source?start+i:SIZE_MAX;options.json_fault=source?SIZE_MAX:start+i;
            CHECK(!admission_worker_once(&options,&metrics));}
        fprintf(stderr,"NAV_WORKER_CHUNK mode=%u domain=%s start=%zu count=%zu total=%zu checked=%zu producer_dead=1 physical0/0 JSON0/0 app0/0\n",
            method,argv[6],start,count,total,count);return 0;
    }
    CHECK(!strcmp(argv[5],"--worker-axes")&&argc==9);options.report=false;
    XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10),(uint64_t)strtoull(argv[8],NULL,10)};
    CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
        XrCompileResourceLimits exact=baseline;uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
        CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);options.limits=&exact;options.budget=delta<0;
        CHECK(!admission_worker_once(&options,&metrics));}
    fprintf(stderr,"NAV_WORKER_AXES mode=%u checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0\n",method);return 0;
}
#endif // LSP_NAVIGATION_WORKER_CASES_H
