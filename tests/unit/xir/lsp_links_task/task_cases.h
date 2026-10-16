/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_LINKS_TASK_CASES_H
#define LSP_LINKS_TASK_CASES_H
#include "app/lsp/xlsp_hierarchy_request.h"
#include "app/lsp/xlsp_links.h"
static const char links_text[]="/* 😀 */ import \"./lib\";\r\nexport * from \"./disk\"\n";
typedef struct LinksRun {
    const char *uri,*library;
    unsigned scenario;
    size_t source_fault,json_fault;
    const XrCompileResourceLimits *limits;
    bool budget,report;
} LinksRun;
typedef struct LinksMetrics {size_t source,json;bool source_hit,json_hit;XrCompileResourceStats stats;} LinksMetrics;
typedef struct LinksFixture {
    XrLspServer server;
    XrLspTransport *transport;
    XrLspAsync *async;
    Gate gate,after;
    XlspRequestId id;
    XrJsonValue *params;
    unsigned tasks;
} LinksFixture;
static void links_base(LinksFixture *f,const LinksRun *options) {
    memset(f,0,sizeof(*f));f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.transport=f->transport=xlsp_transport_stdio();CHECK(f->transport);
    f->server.doc_table=xlsp_document_store_new(destroy_document);CHECK(f->server.doc_table);
    add_document(&f->server,options->uri,options->scenario==10?"import \"./missing\"\n":links_text,7);
    add_document(&f->server,options->library,"export fn value()->i64{return 4}\n",9);
    // Link resolution does not borrow or mutate the semantic token cache.
    xlsp_document_store_get(f->server.doc_table,options->uri)->sem_token_result_id=UINT32_MAX;
    f->id=identity();XlspRequestId owned={0};CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,"textDocument/documentLink",99));
    f->params=parameters(options->uri);
}
static void links_cleanup(LinksFixture *f) {
    if(f->async){xlsp_async_free(f->async);f->async=NULL;}
    xjson_free(f->params);f->params=NULL;
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))
        CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count);xlsp_request_id_free(&f->id);
    xlsp_document_store_free(f->server.doc_table);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static void links_start(LinksFixture *f) {
    f->server.async=f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);
    XrLspTask *barrier=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(barrier);
    CHECK(xlsp_async_submit(f->async,barrier));f->tasks=1;
    uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
}
static void links_reject(LinksFixture *f,XrXirStatus expected) {
    size_t blocks=runtime_live,bytes=runtime_bytes,json=lsp_json_live,json_bytes=lsp_json_bytes;
    size_t app=task_live,app_bytes=task_bytes;XlspPendingRequests pending=f->server.pending_requests;
    CHECK(xlsp_source_task_submit(&f->server,&f->id,f->params,14)==expected);
    CHECK(!memcmp(&pending,&f->server.pending_requests,sizeof(pending)));
    CHECK(runtime_live==blocks&&runtime_bytes==bytes&&lsp_json_live==json&&lsp_json_bytes==json_bytes);
    CHECK(task_live==app&&task_bytes==app_bytes);
}
static void links_admission(LinksFixture *f,const LinksRun *options) {
    unsigned checks=0;
    xjson_object_set_new(xjson_get_object(f->params,"textDocument"),"uri",xjson_new_string(options->uri));
    links_reject(f,XR_XIR_BAD_STRUCTURE);++checks;xjson_free(f->params);f->params=parameters(options->uri);
    xjson_object_set_new(f->params,"textDocument",xjson_new_object());
    links_reject(f,XR_XIR_BAD_STRUCTURE);++checks;xjson_free(f->params);f->params=parameters(options->uri);
    XrJsonValue *uri=xjson_get(xjson_get_object(f->params,"textDocument"),"uri");
    CHECK(uri&&uri->string_len);uri->as.string[uri->string_len-1]=0;
    links_reject(f,XR_XIR_BAD_STRUCTURE);++checks;xjson_free(f->params);f->params=parameters(options->uri);
    for(size_t i=0;i<3;++i){task_fail=task_attempts+i;links_reject(f,XR_XIR_OUT_OF_MEMORY);
        CHECK(task_attempts==task_fail+1);task_fail=SIZE_MAX;++checks;}
    fprintf(stderr,"LINKS_ADMISSION checks=%u\n",checks);
}
static int links_death(const LinksRun *options) {
    LinksFixture f;links_base(&f,options);XlspHierarchyRequest *request=NULL;
    CHECK(xlsp_hierarchy_request_capture(&f.server,f.params,0,XLSP_SOURCE_DOCUMENT_LINKS,&request)==XR_XIR_OK);
    xjson_free(f.params);f.params=NULL;xlsp_document_store_free(f.server.doc_table);f.server.doc_table=NULL;
    memset(&f.server,0xa5,sizeof(f.server));_Atomic bool cancel=false;XlspSourceRequestResult result={0},again={0};
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&result.json&&!result.token_update);
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&again)==XR_XIR_BAD_STRUCTURE&&!again.json);
    xlsp_hierarchy_request_free(request);size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);
    puts(text);lsp_json_free(text);xjson_free(result.json);xlsp_request_id_free(&f.id);xlsp_transport_free(f.transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
    fprintf(stderr,"LINKS_DEATH server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0\n");return 0;
}
static void links_steer(LinksFixture *f,const LinksRun *options) {
    unsigned scenario=options->scenario;
    if(scenario==1)CHECK(xlsp_request_cancel(&f->server,&f->id));
    if(scenario==3){++f->server.source_revision;xlsp_document_store_free(f->server.doc_table);f->server.doc_table=NULL;}
    if(scenario==4)++f->server.source_revision;
    if(scenario==5)f->server.exit_received=true;
    if(scenario==6)f->server.transport=NULL;
    if(scenario==7)f->server.shutdown_received=true;
    if(scenario==9){
        CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));XlspRequestId owned={0};
        CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
        CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,"replacement",100));
        xlsp_pending_request_find(&f->server.pending_requests,&f->id)->source_task_id=UINT64_MAX-1;
    }
    if(scenario==11){
        XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,options->uri);
        memset(doc->content,'?',doc->length);
    }
    if(scenario==8){
        XrLspTask *after=xlsp_task_new(block,block_complete,&f->after,XLSP_TASK_NORMAL);CHECK(after);
        CHECK(xlsp_async_submit(f->async,after));++f->tasks;
    }
}
static void links_finish(LinksFixture *f,const LinksRun *options) {
    if(options->scenario==2){task_cancel_arm();atomic_store(&f->gate.release,true);task_cancel_wait();
        CHECK(xlsp_request_cancel(&f->server,&f->id));task_cancel_release();}
    else atomic_store(&f->gate.release,true);
    if(options->scenario==8){
        uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load(&f->after.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
        CHECK(xlsp_request_cancel(&f->server,&f->id));atomic_store(&f->after.release,true);
    }
    uint64_t deadline=xr_time_monotonic_ms()+10000;unsigned processed=0;
    while(processed<f->tasks){processed+=(unsigned)xlsp_async_poll(f->async);CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    CHECK(f->gate.completed==1&&(options->scenario!=8||f->after.completed==1));
}
static int links_once(const LinksRun *options,LinksMetrics *metrics) {
    if(options->scenario==13)return links_death(options);
    task_axis_limits=NULL;LinksFixture f;links_base(&f,options);links_start(&f);
    if(options->scenario==12)links_admission(&f,options);
    task_axis_limits=options->limits;task_last_stats=(XrCompileResourceStats){0};size_t begin=runtime_attempts;
    runtime_fail_at=options->source_fault==SIZE_MAX?SIZE_MAX:begin+options->source_fault;
    lsp_json_calls=0;lsp_json_fail=options->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,f.params,14);
    if(options->scenario==14){
        CHECK(status==XR_XIR_BAD_STRUCTURE);atomic_store(&f.gate.release,true);links_cleanup(&f);
        fprintf(stderr,"BEFORE actual SourceTask rejected documentLink mode=14 status=1 physical0/0 JSON0/0 app0/0\n");return 1;
    }
    if(options->budget)CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
    else if(options->source_fault==SIZE_MAX)CHECK(status==XR_XIR_OK);
    else CHECK(status==XR_XIR_OK||status==XR_XIR_OUT_OF_MEMORY);
    xjson_free(f.params);f.params=NULL;
    if(status==XR_XIR_OK){++f.tasks;links_steer(&f,options);}
    else CHECK(!xlsp_pending_request_find(&f.server.pending_requests,&f.id)->source_task_id);
    links_finish(&f,options);
    metrics->source=runtime_attempts-begin;metrics->json=lsp_json_calls;metrics->stats=task_last_stats;
    metrics->source_hit=options->source_fault!=SIZE_MAX&&runtime_attempts>runtime_fail_at;metrics->json_hit=lsp_json_injected;
    task_axis_limits=NULL;runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(options->source_fault!=SIZE_MAX||options->json_fault!=SIZE_MAX)CHECK(metrics->source_hit||metrics->json_hit);
    if(options->scenario==9){XlspPendingRequest *pending=xlsp_pending_request_find(&f.server.pending_requests,&f.id);
        CHECK(pending&&pending->source_task_id==UINT64_MAX-1&&!pending->cancelled);}
    else if(status==XR_XIR_OK)CHECK(!f.server.pending_requests.count);
    XrLspDocument *doc=xlsp_document_store_get(f.server.doc_table,options->uri);
    CHECK(!doc||(!doc->source_tokens&&doc->sem_token_result_id==UINT32_MAX));
    CHECK(f.server.next_request_id==71&&!f.server.source_workspace);unsigned stopped=f.server.exit_received?1u:0u;
    links_cleanup(&f);
    if(options->report)fprintf(stderr,"LINKS source=%zu json=%zu source_hit=%u json_hit=%u exit=%u allocated=%llu peak=%llu work=%llu physical0/0 JSON0/0 app0/0\n",
        metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,stopped,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,(unsigned long long)metrics->stats.work);
    return options->scenario==2?task_cancel_check(14):0;
}
static int links_main(int argc,char **argv) {
    CHECK(argc>=5);LinksRun options={.uri=argv[1],.library=argv[3],.source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};
    LinksMetrics metrics={0};
    if(!strcmp(argv[4],"--resource")){
        CHECK(argc==7);options.source_fault=(size_t)strtoull(argv[5],NULL,10);options.json_fault=(size_t)strtoull(argv[6],NULL,10);
        return links_once(&options,&metrics);
    }
    if(!strcmp(argv[4],"--chunk")){
        CHECK(argc==9);bool source=!strcmp(argv[5],"source");CHECK(source||!strcmp(argv[5],"json"));
        size_t start=(size_t)strtoull(argv[6],NULL,10),count=(size_t)strtoull(argv[7],NULL,10),total=(size_t)strtoull(argv[8],NULL,10);
        CHECK(count&&start<total&&count<=total-start);options.report=false;
        for(size_t i=0;i<count;++i){options.source_fault=source?start+i:SIZE_MAX;options.json_fault=source?SIZE_MAX:start+i;
            CHECK(!links_once(&options,&metrics));}
        fprintf(stderr,"LINKS_CHUNK domain=%s start=%zu count=%zu total=%zu checked=%zu physical0/0 JSON0/0 app0/0\n",
            argv[5],start,count,total,count);return 0;
    }
    if(!strcmp(argv[4],"--axes")){
        CHECK(argc==8);XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[5],NULL,10),
            (uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10)};options.report=false;
        CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
            XrCompileResourceLimits limits=baseline;uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
            CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);options.limits=&limits;options.budget=delta<0;
            CHECK(!links_once(&options,&metrics));}
        fprintf(stderr,"LINKS_AXES checked=9 physical0/0 JSON0/0 app0/0\n");return 0;
    }
    options.scenario=(unsigned)strtoul(argv[4],NULL,10);CHECK(options.scenario<=15);
    CHECK(argc==5||(argc==6&&options.scenario==15));
    if(options.scenario==15)options.json_fault=(size_t)strtoull(argv[5],NULL,10);
    return links_once(&options,&metrics);
}
#endif // LSP_LINKS_TASK_CASES_H
