/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_PARAMETER_TASK_CASES_H
#define LSP_PARAMETER_TASK_CASES_H
#include "app/lsp/xlsp_hierarchy_request.h"
static const char parameter_text[]="import { mix as chosen } from \"./lib\"\nfn main(){\n var slot=1\n chosen(ref slot,2)\n}\n/* 😀 */\n";
static const char parameter_library[]="export fn mix(value:ref i64,extra:i64){value+=extra}\n";

typedef struct ParameterOptions {
    const char *uri,*library;
    unsigned scenario;
    size_t source_fault,json_fault;
    const XrCompileResourceLimits *limits;
    bool budget,report;
} ParameterOptions;
typedef struct ParameterMetrics {size_t source,json;bool source_hit,json_hit;XrCompileResourceStats stats;} ParameterMetrics;
typedef struct ParameterFixture {
    XrLspServer server;
    XrLspAsync *async;
    XrLspTransport *transport;
    Gate gate,after;
    XlspRequestId id;
    XrJsonValue *params;
} ParameterFixture;
static XrJsonValue *parameter_integer(int64_t value) {
    XrJsonValue *out=xjson_parse("0",1);CHECK(out);out->as.integer=value;return out;
}
static void parameter_fixture(ParameterFixture *f,const ParameterOptions *options) {
    memset(f,0,sizeof(*f));f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.transport=f->transport=xlsp_transport_stdio();CHECK(f->transport);
    f->server.doc_table=xlsp_document_store_new(destroy_document);CHECK(f->server.doc_table);
    f->server.config.inlay_hints_parameter_names=options->scenario!=9&&options->scenario!=12;
    add_document(&f->server,options->uri,parameter_text,1);add_document(&f->server,options->library,parameter_library,7);
    xlsp_document_store_get(f->server.doc_table,options->uri)->sem_token_result_id=UINT32_MAX;
    f->params=parameters(options->uri);
    XrJsonValue *range=xjson_new_object(),*start=xjson_new_object(),*end=xjson_new_object();CHECK(range&&start&&end);
    CHECK(index_json_member(start,"line",parameter_integer(options->scenario==10?4:3)));
    CHECK(index_json_member(start,"character",parameter_integer(0)));
    CHECK(index_json_member(end,"line",parameter_integer(options->scenario==10?4:3)));
    CHECK(index_json_member(end,"character",parameter_integer(options->scenario==10?0:19)));
    CHECK(index_json_member(range,"start",start)&&index_json_member(range,"end",end));
    CHECK(index_json_member(f->params,"range",range));
    if(options->scenario==11||options->scenario==12) {
        xjson_get(start,"line")->as.integer=5;xjson_get(start,"character")->as.integer=4;
        xjson_get(end,"line")->as.integer=5;xjson_get(end,"character")->as.integer=4;
    }
    f->id=identity();XlspRequestId owned={0};CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,"textDocument/inlayHint",99));
}
static void parameter_close(ParameterFixture *f) {
    if(f->async)xlsp_async_free(f->async);
    xlsp_source_workspace_free(f->server.source_workspace);xjson_free(f->params);
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count&&f->server.next_request_id==71);
    xlsp_request_id_free(&f->id);xlsp_document_store_free(f->server.doc_table);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static void parameter_start(ParameterFixture *f) {
    f->server.async=f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);
    XrLspTask *task=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(task);
    CHECK(xlsp_async_submit(f->async,task));uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
}
static void parameter_death(ParameterFixture *f) {
    XlspHierarchyRequest *request=NULL;CHECK(xlsp_hierarchy_request_capture(&f->server,f->params,0,XLSP_SOURCE_PARAMETER_HINTS,&request)==XR_XIR_OK);
    CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    xlsp_document_store_free(f->server.doc_table);xjson_free(f->params);f->params=NULL;
    memset(&f->server,0xa5,sizeof(f->server));_Atomic bool cancel=false;XlspSourceRequestResult result={0},again={0};
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&result.json&&!result.token_update);
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&again)==XR_XIR_BAD_STRUCTURE&&!again.json);
    xlsp_hierarchy_request_free(request);CHECK(!runtime_live&&!runtime_bytes);
    size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result.json);xlsp_request_id_free(&f->id);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
    fprintf(stderr,"PARAMETER_DEATH server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0\n");
}
static void parameter_admission(ParameterFixture *f) {
    const char *keys[]={"textDocument","range","uri","start","end","line","character","line","character"};
    for(unsigned i=0;i<9;++i) {
        XrJsonValue *params=xjson_clone(f->params);CHECK(params);
        XrJsonValue *range=xjson_get_object(params,"range");
        XrJsonValue *object=i<2?params:i==2?xjson_get_object(params,"textDocument"):i<5?range:
            xjson_get_object(range,i<7?"start":"end");
        CHECK(index_json_member(object,keys[i],xjson_clone(xjson_get(object,keys[i]))));
        XlspPendingRequests pending=f->server.pending_requests;XlspHierarchyRequest *request=NULL;
        CHECK(xlsp_hierarchy_request_capture(&f->server,params,0,XLSP_SOURCE_PARAMETER_HINTS,&request)==XR_XIR_BAD_STRUCTURE&&!request);
        CHECK(xlsp_source_task_submit(&f->server,&f->id,params,16)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&pending,&f->server.pending_requests,sizeof(pending))&&!runtime_live&&!runtime_bytes);
        xjson_free(params);
    }
    const char *uri=xjson_get_string(xjson_get_object(f->params,"textDocument"),"uri");
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,uri);CHECK(doc);doc->server=NULL;
    XlspHierarchyRequest *request=NULL;
    CHECK(xlsp_hierarchy_request_capture(&f->server,f->params,0,XLSP_SOURCE_PARAMETER_HINTS,&request)==XR_XIR_BAD_STRUCTURE&&!request);
    CHECK(xlsp_source_task_submit(&f->server,&f->id,f->params,16)==XR_XIR_BAD_STRUCTURE);doc->server=&f->server;
    for(unsigned i=0;i<4;++i) {
        XrJsonValue *params=xjson_clone(f->params);CHECK(params);
        XrJsonValue *start=xjson_get_object(xjson_get_object(params,"range"),"start"),*line=xjson_get(start,"line");
        if(i==0)line->as.integer=-1;
        if(i==1)line->as.integer=(int64_t)INT32_MAX+1;
        if(i==2){line->is_integer=false;line->as.number=3.5;}
        if(i==3)line->as.integer=4;
        CHECK(xlsp_hierarchy_request_capture(&f->server,params,0,XLSP_SOURCE_PARAMETER_HINTS,&request)==XR_XIR_BAD_STRUCTURE&&!request);
        CHECK(xlsp_source_task_submit(&f->server,&f->id,params,16)==XR_XIR_BAD_STRUCTURE);xjson_free(params);
    }
    CHECK(!runtime_live&&!runtime_bytes);
    fprintf(stderr,"PARAMETER_ADMISSION duplicate_known=9 capture_and_async=18 owner_and_range=10 physical0/0\n");
}
static int parameter_once(const ParameterOptions *options,ParameterMetrics *metrics) {
    ParameterFixture f;parameter_fixture(&f,options);
    if(options->scenario==14){parameter_start(&f);parameter_admission(&f);atomic_store(&f.gate.release,true);parameter_close(&f);return 0;}
    if(options->scenario==15){parameter_death(&f);return 0;}
    parameter_start(&f);size_t begin=runtime_attempts;task_resource_created_count=0;task_last_created_resources=NULL;
    task_axis_limits=options->limits;runtime_fail_at=options->source_fault==SIZE_MAX?SIZE_MAX:begin+options->source_fault;
    lsp_json_calls=0;lsp_json_fail=options->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,f.params,16);
    if(options->scenario==13){CHECK(status==XR_XIR_BAD_STRUCTURE);atomic_store(&f.gate.release,true);parameter_close(&f);
        fprintf(stderr,"PARAMETER_BEFORE mode16_rejected=1 physical0/0\n");return 0;}
    bool fault=options->source_fault!=SIZE_MAX||options->json_fault!=SIZE_MAX;
    if(options->budget)CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
    else if(fault)CHECK(status==XR_XIR_OK||status==XR_XIR_OUT_OF_MEMORY);else CHECK(status==XR_XIR_OK);
    XrCompileResources *ledger=task_last_created_resources;
    xjson_free(f.params);f.params=NULL;xlsp_document_store_free(f.server.doc_table);f.server.doc_table=NULL;
    f.server.config.inlay_hints_parameter_names=options->scenario==9;
    if(status==XR_XIR_OK) {
        XrLspTask *after=xlsp_task_new(block,block_complete,&f.after,XLSP_TASK_NORMAL);CHECK(after);
        CHECK(xlsp_async_submit(f.async,after));
        if(options->scenario==1)CHECK(xlsp_request_cancel(&f.server,&f.id));
        if(options->scenario==3)++f.server.source_revision;
        if(options->scenario==4)f.server.exit_received=true;
        if(options->scenario==5)f.server.transport=NULL;
        if(options->scenario==6) {
            CHECK(xlsp_pending_request_remove(&f.server.pending_requests,&f.id));XlspRequestId owned={0};
            CHECK(xlsp_request_id_clone(&f.id,&owned)==XLSP_PROTOCOL_OK);
            CHECK(xlsp_pending_request_add(&f.server.pending_requests,owned,"replacement",100));
            xlsp_pending_request_find(&f.server.pending_requests,&f.id)->source_task_id=UINT64_MAX-1;
        }
        if(options->scenario==7)f.server.shutdown_received=true;
        if(options->scenario==2)task_cancel_arm();
        atomic_store(&f.gate.release,true);
        if(options->scenario==2){task_cancel_wait();CHECK(xlsp_request_cancel(&f.server,&f.id));task_cancel_release();}
        uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load(&f.after.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
        CHECK(task_resource_created_count==1&&ledger&&ledger==task_last_created_resources);metrics->stats=sdk_stats(ledger);
        if(options->scenario==8)CHECK(xlsp_request_cancel(&f.server,&f.id));
        atomic_store(&f.after.release,true);
    } else {metrics->stats=task_last_stats;atomic_store(&f.gate.release,true);}
    unsigned completed=0;uint64_t deadline=xr_time_monotonic_ms()+10000;
    while(completed<(status==XR_XIR_OK?3u:1u)){completed+=(unsigned)xlsp_async_poll(f.async);
        CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    metrics->source=runtime_attempts-begin;metrics->json=lsp_json_calls;
    metrics->source_hit=options->source_fault!=SIZE_MAX&&runtime_attempts>runtime_fail_at;metrics->json_hit=lsp_json_injected;
    task_last_created_resources=NULL;task_axis_limits=NULL;runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(fault)CHECK(metrics->source_hit||metrics->json_hit);
    if(options->scenario==6)CHECK(xlsp_pending_request_find(&f.server.pending_requests,&f.id)->source_task_id==UINT64_MAX-1);
    else if(status==XR_XIR_OK)CHECK(!f.server.pending_requests.count);
    unsigned stopped=f.server.exit_received?1u:0u;parameter_close(&f);
    if(options->report)fprintf(stderr,"PARAMETER source=%zu json=%zu source_hit=%u json_hit=%u exit=%u allocated=%llu peak=%llu work=%llu created=1 producer_dead=1 physical0/0 JSON0/0 app0/0\n",
        metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,stopped,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,(unsigned long long)metrics->stats.work);
    return options->scenario==2?task_cancel_check(16):0;
}
static bool parameter_resource_scenario(unsigned scenario) {return scenario==0||scenario==9||scenario==10;}
static int parameter_main(int argc,char **argv) {
    CHECK(argc>=5);ParameterOptions options={.uri=argv[1],.library=argv[3],.source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};
    ParameterMetrics metrics={0};
    if(!strcmp(argv[4],"--resource")){CHECK(argc==8);options.source_fault=(size_t)strtoull(argv[5],NULL,10);
        options.json_fault=(size_t)strtoull(argv[6],NULL,10);options.scenario=(unsigned)strtoul(argv[7],NULL,10);
        CHECK(parameter_resource_scenario(options.scenario));return parameter_once(&options,&metrics);}
    if(!strcmp(argv[4],"--chunk")) {
        CHECK(argc==10);bool source=!strcmp(argv[5],"source");CHECK(source||!strcmp(argv[5],"json"));
        size_t start=(size_t)strtoull(argv[6],NULL,10),count=(size_t)strtoull(argv[7],NULL,10),total=(size_t)strtoull(argv[8],NULL,10);
        CHECK(count&&start<total&&count<=total-start);options.report=false;options.scenario=(unsigned)strtoul(argv[9],NULL,10);
        CHECK(parameter_resource_scenario(options.scenario));
        for(size_t i=0;i<count;++i){options.source_fault=source?start+i:SIZE_MAX;options.json_fault=source?SIZE_MAX:start+i;
            CHECK(!parameter_once(&options,&metrics));}
        fprintf(stderr,"PARAMETER_CHUNK domain=%s start=%zu count=%zu total=%zu checked=%zu physical0/0 JSON0/0 app0/0\n",
            argv[5],start,count,total,count);return 0;
    }
    if(!strcmp(argv[4],"--axes")) {
        CHECK(argc==9);options.report=false;options.scenario=(unsigned)strtoul(argv[8],NULL,10);
        CHECK(parameter_resource_scenario(options.scenario));
        XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[5],NULL,10),(uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10)};
        CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits cut=baseline;
            uint64_t *bound=axis==0?&cut.allocated_bytes:axis==1?&cut.live_bytes:&cut.work;CHECK(*bound&&*bound<INT64_MAX);
            *bound=(uint64_t)((int64_t)*bound+delta);options.limits=&cut;options.budget=delta<0;CHECK(!parameter_once(&options,&metrics));}
        fprintf(stderr,"PARAMETER_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0\n");return 0;
    }
    CHECK(argc==5);options.scenario=(unsigned)strtoul(argv[4],NULL,10);CHECK(options.scenario<=15);
    return parameter_once(&options,&metrics);
}
#endif // LSP_PARAMETER_TASK_CASES_H
