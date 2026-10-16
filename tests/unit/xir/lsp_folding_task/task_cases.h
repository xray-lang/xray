/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_FOLDING_TASK_CASES_H
#define LSP_FOLDING_TASK_CASES_H
#include "app/lsp/xlsp_hierarchy_request.h"
#include "app/lsp/xlsp_folding.h"
#include "app/lsp/xlsp_source_syntax.h"
#include "folding_server_mode.inc.c"
XR_FUNC XrJsonValue *before_xlsp_handle_folding_range(XrLspServer *,XrJsonValue *);
static const char folding_text[]="fn main(){\n var slot=1\n if(slot>0){\n slot+=1\n }\n}\n";
typedef struct FoldingOptions {
    const char *uri,*library;
    unsigned scenario;
    size_t source_fault,json_fault;
    const XrCompileResourceLimits *limits;
    bool budget,report;
} FoldingOptions;
typedef struct FoldingMetrics {size_t source,json;bool source_hit,json_hit;XrCompileResourceStats stats;} FoldingMetrics;
typedef struct FoldingFixture {
    XrLspServer server;
    XrLspAsync *async;
    XrLspTransport *transport;
    Gate gate,after;
    XlspRequestId id;
    XrJsonValue *params;
} FoldingFixture;
static void folding_fixture(FoldingFixture *f,const FoldingOptions *options) {
    memset(f,0,sizeof(*f));f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.transport=f->transport=xlsp_transport_stdio();CHECK(f->transport);
    f->server.doc_table=xlsp_document_store_new(destroy_document);CHECK(f->server.doc_table);
    f->server.config.ignore_pattern_count=-1;
    const char *text=options->scenario==9?"fn main(){\n var slot=1\n if(slot>0){\n slot+=1\n }\n}\nfn broken(\n":
        options->scenario==10?"":folding_text;
    char *long_text=NULL;
    if(options->scenario==2||options->scenario==11) {
        long_text=sdk_fixture_malloc(8300);CHECK(long_text);
        memcpy(long_text,"/*",2);memset(long_text+2,'a',8192);memcpy(long_text+8194,"*/\n",3);
        strcpy(long_text+8197,folding_text);text=long_text;
    }
    add_document(&f->server,options->uri,text,1);sdk_fixture_free(long_text);
    add_document(&f->server,options->library,"\xff",7);
    xlsp_document_store_get(f->server.doc_table,options->uri)->sem_token_result_id=UINT32_MAX;
    f->params=xjson_new_object();XrJsonValue *document=xjson_new_object();CHECK(f->params&&document);
    CHECK(index_json_member(document,"uri",xjson_new_string(options->uri)));
    CHECK(index_json_member(f->params,"textDocument",document));
    f->id=identity();XlspRequestId owned={0};CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,"textDocument/foldingRange",99));
}
static void folding_close(FoldingFixture *f) {
    if(f->async)xlsp_async_free(f->async);
    xlsp_source_workspace_free(f->server.source_workspace);xjson_free(f->params);
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count&&f->server.next_request_id==71);
    xlsp_request_id_free(&f->id);xlsp_document_store_free(f->server.doc_table);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static void folding_start(FoldingFixture *f) {
    f->server.async=f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);
    XrLspTask *task=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(task);
    CHECK(xlsp_async_submit(f->async,task));uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
}
static void folding_death(FoldingFixture *f) {
    XlspHierarchyRequest *request=NULL;CHECK(xlsp_hierarchy_request_capture(&f->server,f->params,0,XLSP_SOURCE_FOLDING,&request)==XR_XIR_OK);
    CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    xlsp_document_store_free(f->server.doc_table);xjson_free(f->params);f->params=NULL;
    memset(&f->server,0xa5,sizeof(f->server));_Atomic bool cancel=false;XlspSourceRequestResult result={0},again={0};
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&result.json&&!result.token_update);
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&again)==XR_XIR_BAD_STRUCTURE&&!again.json);
    xlsp_hierarchy_request_free(request);CHECK(!runtime_live&&!runtime_bytes);
    size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result.json);xlsp_request_id_free(&f->id);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
    fprintf(stderr,"FOLDING_DEATH server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0\n");
}
static void folding_admission(FoldingFixture *f) {
    const char *keys[]={"textDocument","uri"};
    for(unsigned i=0;i<2;++i) {
        XrJsonValue *params=xjson_clone(f->params);CHECK(params);
        XrJsonValue *object=i?xjson_get_object(params,"textDocument"):params;
        CHECK(index_json_member(object,keys[i],xjson_clone(xjson_get(object,keys[i]))));
        XlspPendingRequests pending=f->server.pending_requests;XlspHierarchyRequest *request=NULL;
        CHECK(xlsp_hierarchy_request_capture(&f->server,params,0,XLSP_SOURCE_FOLDING,&request)==XR_XIR_BAD_STRUCTURE&&!request);
        CHECK(xlsp_source_task_submit(&f->server,&f->id,params,17)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&pending,&f->server.pending_requests,sizeof(pending))&&!runtime_live&&!runtime_bytes);
        xjson_free(params);
    }
    const char *uri=xjson_get_string(xjson_get_object(f->params,"textDocument"),"uri");
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,uri);CHECK(doc);doc->server=NULL;
    XlspHierarchyRequest *request=NULL;
    CHECK(xlsp_hierarchy_request_capture(&f->server,f->params,0,XLSP_SOURCE_FOLDING,&request)==XR_XIR_BAD_STRUCTURE&&!request);
    CHECK(xlsp_source_task_submit(&f->server,&f->id,f->params,17)==XR_XIR_BAD_STRUCTURE);doc->server=&f->server;
    CHECK(!runtime_live&&!runtime_bytes);
    fprintf(stderr,"FOLDING_ADMISSION duplicate_known=2 capture_and_async=4 owner=2 physical0/0\n");
}
static void folding_cache(FoldingFixture *f,const char *uri,bool before) {
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,uri);CHECK(doc);
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;
    CHECK(task_resource_new_impl(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XlspSourceDocument input={doc->uri,doc->content,strlen(doc->uri),doc->length,doc->version};
    CHECK(xlsp_source_syntax_build(session,&input,&doc->syntax_snapshot)==XR_PARSE_OK);
    doc->ast=(AstNode *)xlsp_source_syntax_ast(doc->syntax_snapshot);
    uintptr_t syntax=(uintptr_t)doc->syntax_snapshot,ast=(uintptr_t)doc->ast;
    xr_compile_session_free(session);xr_compile_resources_release(resources);
    XrJsonValue *result=before?before_xlsp_handle_folding_range(&f->server,f->params):
        xlsp_handle_folding_range(&f->server,f->params);CHECK(result&&f->server.source_failure.status==XR_XIR_OK);
    CHECK((uintptr_t)doc->ast==ast);
    if(before)CHECK((uintptr_t)doc->syntax_snapshot!=syntax&&(uintptr_t)xlsp_source_syntax_ast(doc->syntax_snapshot)!=ast);
    else CHECK((uintptr_t)doc->syntax_snapshot==syntax&&xlsp_source_syntax_ast(doc->syntax_snapshot)==doc->ast);
    size_t length=0;char *text=xjson_stringify(result,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result);doc->ast=NULL;
    xlsp_source_syntax_free(doc->syntax_snapshot);doc->syntax_snapshot=NULL;folding_close(f);
    fprintf(stderr,"FOLDING_CACHE before=%u cache_%s=1 physical0/0 JSON0/0 app0/0\n",before?1u:0u,before?"replaced_AST_stale":"and_AST_preserved");
}
static int folding_once(const FoldingOptions *options,FoldingMetrics *metrics) {
    FoldingFixture f;folding_fixture(&f,options);
    if(options->scenario==14){folding_start(&f);folding_admission(&f);atomic_store(&f.gate.release,true);folding_close(&f);return 0;}
    if(options->scenario==15){folding_death(&f);return 0;}
    if(options->scenario==12||options->scenario==13){folding_cache(&f,options->uri,options->scenario==13);return 0;}
    folding_start(&f);size_t begin=runtime_attempts;task_resource_created_count=0;task_last_created_resources=NULL;
    task_axis_limits=options->limits;runtime_fail_at=options->source_fault==SIZE_MAX?SIZE_MAX:begin+options->source_fault;
    lsp_json_calls=0;lsp_json_fail=options->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,f.params,17);
    bool fault=options->source_fault!=SIZE_MAX||options->json_fault!=SIZE_MAX;
    if(options->budget)CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
    else if(fault)CHECK(status==XR_XIR_OK||status==XR_XIR_OUT_OF_MEMORY);else CHECK(status==XR_XIR_OK);
    XrCompileResources *ledger=task_last_created_resources;
    xjson_free(f.params);f.params=NULL;xlsp_document_store_free(f.server.doc_table);f.server.doc_table=NULL;
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
    unsigned stopped=f.server.exit_received?1u:0u;folding_close(&f);
    if(options->report)fprintf(stderr,"FOLDING source=%zu json=%zu source_hit=%u json_hit=%u exit=%u allocated=%llu peak=%llu work=%llu created=%zu producer_dead=1 physical0/0 JSON0/0 app0/0\n",
        metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,stopped,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,(unsigned long long)metrics->stats.work,task_resource_created_count);
    return options->scenario==2?task_cancel_check(17):0;
}
static bool folding_resource_scenario(unsigned scenario) {return scenario==0||scenario==9||scenario==10||scenario==11;}
static int folding_main(int argc,char **argv) {
    CHECK(source_request_mode("textDocument/foldingRange")==17);
    CHECK(source_request_mode("textDocument/inlayHint")==-1&&source_request_mode("textDocument/signatureHelp")==15);
    CHECK(argc>=5);FoldingOptions options={.uri=argv[1],.library=argv[3],.source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};
    FoldingMetrics metrics={0};
    if(!strcmp(argv[4],"--resource")){CHECK(argc==8);options.source_fault=(size_t)strtoull(argv[5],NULL,10);
        options.json_fault=(size_t)strtoull(argv[6],NULL,10);options.scenario=(unsigned)strtoul(argv[7],NULL,10);
        CHECK(folding_resource_scenario(options.scenario));return folding_once(&options,&metrics);}
    if(!strcmp(argv[4],"--chunk")) {
        CHECK(argc==10);bool source=!strcmp(argv[5],"source");CHECK(source||!strcmp(argv[5],"json"));
        size_t start=(size_t)strtoull(argv[6],NULL,10),count=(size_t)strtoull(argv[7],NULL,10),total=(size_t)strtoull(argv[8],NULL,10);
        CHECK(count&&start<total&&count<=total-start);options.report=false;options.scenario=(unsigned)strtoul(argv[9],NULL,10);
        CHECK(folding_resource_scenario(options.scenario));
        for(size_t i=0;i<count;++i){options.source_fault=source?start+i:SIZE_MAX;options.json_fault=source?SIZE_MAX:start+i;
            CHECK(!folding_once(&options,&metrics));}
        fprintf(stderr,"FOLDING_CHUNK domain=%s start=%zu count=%zu total=%zu checked=%zu physical0/0 JSON0/0 app0/0\n",
            argv[5],start,count,total,count);return 0;
    }
    if(!strcmp(argv[4],"--axes")) {
        CHECK(argc==9);options.report=false;options.scenario=(unsigned)strtoul(argv[8],NULL,10);
        CHECK(folding_resource_scenario(options.scenario));
        XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[5],NULL,10),(uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10)};
        CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits cut=baseline;
            uint64_t *bound=axis==0?&cut.allocated_bytes:axis==1?&cut.live_bytes:&cut.work;CHECK(*bound&&*bound<INT64_MAX);
            *bound=(uint64_t)((int64_t)*bound+delta);options.limits=&cut;options.budget=delta<0;CHECK(!folding_once(&options,&metrics));}
        fprintf(stderr,"FOLDING_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0\n");return 0;
    }
    CHECK(argc==5);options.scenario=(unsigned)strtoul(argv[4],NULL,10);CHECK(options.scenario<=15);
    return folding_once(&options,&metrics);
}
#endif // LSP_FOLDING_TASK_CASES_H
