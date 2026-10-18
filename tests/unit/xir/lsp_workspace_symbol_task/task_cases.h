/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_WORKSPACE_TASK_CASES_H
#define LSP_WORKSPACE_TASK_CASES_H
#include "app/lsp/xlsp_workspace_symbols.h"
#include "app/lsp/xlsp_source_index_store.h"
static const char workspace_root_text[]="/* \xf0\x9f\x98\x80 */ export fn first()->i64{return 1}\nfn second(){}\nstruct Thing{}\n";
static const char workspace_library_text[]="export fn shared(){}\nconst hidden:i64=1\n";
XR_FUNC XrXirStatus before_xlsp_source_index_store_visit(XlspSourceIndexStore *,XlspIndexVisitor,void *);
XR_FUNC XrLspDocument *xlsp_document_get(XrLspServer *server,const char *uri) {
    return server?xlsp_document_store_get(server->doc_table,uri):NULL;
}
#define XLSP_WORKSPACE_SYMBOL_LIMIT 100
typedef struct {XrLspServer *server;XrJsonValue *symbols;const char *query;size_t query_length;int added;} XlspWorkspaceSymbolCtx;
#include "workspace_handlers.inc.c"
#include "before_handlers.inc.c"
typedef struct WorkspaceOptions {
    const char *uri,*library;
    unsigned scenario,method;
    size_t source_fault,json_fault;
    const XrCompileResourceLimits *limits;
    bool budget,report;
} WorkspaceOptions;
typedef struct WorkspaceMetrics {size_t source,json;bool source_hit,json_hit;XrCompileResourceStats stats;} WorkspaceMetrics;
typedef struct WorkspaceFixture {
    XrLspServer server;
    XrLspAsync *async;
    XrLspTransport *transport;
    Gate gate,after;
    XlspRequestId id;
    XrJsonValue *params;
    unsigned method;
} WorkspaceFixture;
static unsigned workspace_mode(const WorkspaceFixture *f) {(void)f;return 22u;}
static const char *workspace_method(const WorkspaceFixture *f) {(void)f;return "workspace/symbol";}
static void workspace_add_index(WorkspaceFixture *f,const char *uri,const char *text,int version) {
    XlspSourceDocument document={uri,text,strlen(uri),strlen(text),version};XlspSourceIndex *index=NULL;
    CHECK(xlsp_source_index_build(f->server.index_resources,&document,&index)==XR_XIR_OK);
    CHECK(xlsp_source_index_store_replace(f->server.source_index,&index)==XR_XIR_OK&&!index);
}
static void workspace_fixture(WorkspaceFixture *f,const WorkspaceOptions *options) {
    memset(f,0,sizeof(*f));f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.transport=f->transport=xlsp_transport_stdio();CHECK(f->transport);
    f->server.doc_table=xlsp_document_store_new(destroy_document);CHECK(f->server.doc_table);
    f->server.config.ignore_pattern_count=-1;f->server.workspace_folder_count=-1;
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    f->server.index_resources=sdk_ledger(&limits);CHECK(xlsp_source_index_store_new(f->server.index_resources,&f->server.source_index)==XR_XIR_OK);
    add_document(&f->server,options->uri,"\xff",7);
    xlsp_document_store_get(f->server.doc_table,options->uri)->sem_token_result_id=UINT32_MAX;
    if(options->scenario!=10) {
        workspace_add_index(f,options->uri,workspace_root_text,7);workspace_add_index(f,options->library,workspace_library_text,0);
        char stale[4096];int n=snprintf(stale,sizeof(stale),"%s-stale.xr",options->uri);CHECK(n>0&&(size_t)n<sizeof(stale));
        workspace_add_index(f,stale,"fn retired(){}\n",1);add_document(&f->server,stale,"\xff",2);
    }
    if(options->scenario==2||options->scenario==11) {
        char text[16000],uri[4096],prefix[61];memset(prefix,'a',60);prefix[60]=0;size_t used=0;
        for(unsigned i=0;i<110;++i){int n=snprintf(text+used,sizeof(text)-used,"fn %s%03u(){}\n",prefix,i);CHECK(n>0&&(size_t)n<sizeof(text)-used);used+=(size_t)n;}
        int n=snprintf(uri,sizeof(uri),"%s-many.xr",options->uri);CHECK(n>0&&(size_t)n<sizeof(uri));workspace_add_index(f,uri,text,0);
    }
    f->params=xjson_new_object();CHECK(f->params);
    const char *query=options->scenario==9?"\xf0\x9f\x98\x80":options->scenario==2?"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaax":"";
    CHECK(index_json_member(f->params,"query",xjson_new_string(query)));
    f->id=identity();XlspRequestId owned={0};CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,workspace_method(f),99));
}
static void workspace_close(WorkspaceFixture *f) {
    if(f->async)xlsp_async_free(f->async);
    xlsp_source_workspace_free(f->server.source_workspace);xjson_free(f->params);
    xlsp_source_index_store_free(f->server.source_index);xr_compile_resources_release(f->server.index_resources);
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count&&f->server.next_request_id==71);
    xlsp_request_id_free(&f->id);xlsp_document_store_free(f->server.doc_table);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static void workspace_start(WorkspaceFixture *f) {
    f->server.async=f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);
    XrLspTask *task=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(task);
    CHECK(xlsp_async_submit(f->async,task));uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
}
static void workspace_death(WorkspaceFixture *f) {
    XlspWorkspaceSymbolRequest *request=NULL;CHECK(xlsp_workspace_symbols_capture(&f->server,f->params,&request)==XR_XIR_OK);
    CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    xlsp_document_store_free(f->server.doc_table);xjson_free(f->params);f->params=NULL;
    xlsp_source_index_store_free(f->server.source_index);xr_compile_resources_release(f->server.index_resources);
    memset(&f->server,0xa5,sizeof(f->server));_Atomic bool cancel=false;XlspSourceRequestResult result={0},again={0};
    CHECK(xlsp_workspace_symbols_run(request,&cancel,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&result.json&&!result.token_update);
    CHECK(xlsp_workspace_symbols_run(request,&cancel,&again)==XR_XIR_BAD_STRUCTURE&&!again.json);
    xlsp_workspace_symbols_free(request);CHECK(!runtime_live&&!runtime_bytes);
    size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result.json);xlsp_request_id_free(&f->id);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
    fprintf(stderr,"WORKSPACE_DEATH server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0\n");
}
static void workspace_reject(WorkspaceFixture *f,XrJsonValue *params) {
    XlspPendingRequests pending=f->server.pending_requests;XlspWorkspaceSymbolRequest *request=NULL;
    size_t blocks=runtime_live,bytes=runtime_bytes;XrCompileResourceStats before=sdk_stats(f->server.index_resources);
    CHECK(xlsp_workspace_symbols_capture(&f->server,params,&request)==XR_XIR_BAD_STRUCTURE&&!request);
    CHECK(xlsp_source_task_submit(&f->server,&f->id,params,22)==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&pending,&f->server.pending_requests,sizeof(pending))&&runtime_live==blocks&&runtime_bytes==bytes);
    XrCompileResourceStats after=sdk_stats(f->server.index_resources);CHECK(before.allocated_bytes==after.allocated_bytes&&before.live_bytes==after.live_bytes&&before.peak_bytes==after.peak_bytes&&before.work==after.work);
}
static void workspace_admission(WorkspaceFixture *f) {
    XrJsonValue *duplicate=xjson_clone(f->params);CHECK(duplicate);CHECK(index_json_member(duplicate,"query",xjson_new_string("different")));
    workspace_reject(f,duplicate);xjson_free(duplicate);
    const char *values[]={"null","1","true","[]","{}","\"\\u0000\""};
    for(unsigned i=0;i<6;++i){XrJsonValue *params=xjson_clone(f->params),*value=xjson_parse(values[i],strlen(values[i]));CHECK(params&&value);
        xjson_object_set(params,"query",value);workspace_reject(f,params);xjson_free(params);}
    XrJsonValue *params=xjson_new_object();CHECK(params);workspace_reject(f,params);xjson_free(params);
    params=xjson_new_array();CHECK(params);workspace_reject(f,params);xjson_free(params);
    params=xjson_clone(f->params);CHECK(params);xjson_object_set(params,"query",xjson_new_string_n("\xff",1));workspace_reject(f,params);xjson_free(params);
    XlspSourceIndexStore *store=f->server.source_index;f->server.source_index=NULL;workspace_reject(f,f->params);f->server.source_index=store;
    XrLspDocument *open=NULL;
    for(int i=0;i<f->server.doc_table->bucket_count&&!open;++i)for(XrLspDocBucket *bucket=f->server.doc_table->buckets[i];bucket;bucket=bucket->next)
        if(bucket->doc->version==7){open=bucket->doc;break;}
    CHECK(open);open->server=NULL;workspace_reject(f,f->params);open->server=&f->server;
    f->server.index_failure=XR_XIR_BUDGET;XlspWorkspaceSymbolRequest *request=NULL;XlspPendingRequests pending=f->server.pending_requests;
    CHECK(xlsp_workspace_symbols_capture(&f->server,f->params,&request)==XR_XIR_BUDGET&&!request);
    CHECK(xlsp_source_task_submit(&f->server,&f->id,f->params,22)==XR_XIR_BUDGET);
    CHECK(!memcmp(&pending,&f->server.pending_requests,sizeof(pending)));f->server.index_failure=XR_XIR_OK;
    fprintf(stderr,"WORKSPACE_ADMISSION duplicate=1 invalid_shapes=9 original_index_ledger_unchanged=1\n");
}
static void workspace_cache(WorkspaceFixture *f,const char *uri,bool before) {
    (void)uri;XrCompileResourceStats initial=sdk_stats(f->server.index_resources);
    if(before)CHECK(index_json_member(f->params,"query",xjson_new_string("absent")));
    XrJsonValue *result=before?before_xlsp_handle_workspace_symbol(&f->server,f->params):xlsp_handle_workspace_symbol(&f->server,f->params);CHECK(result);
    if(!before) {
        XrJsonValue *again=xlsp_handle_workspace_symbol(&f->server,f->params);CHECK(again);
        size_t first_length=0,next_length=0;char *first=xjson_stringify(result,&first_length),*next=xjson_stringify(again,&next_length);
        CHECK(first&&next&&first_length==next_length&&!memcmp(first,next,first_length));
        lsp_json_free(first);lsp_json_free(next);xjson_free(again);
    }
    XrCompileResourceStats after=sdk_stats(f->server.index_resources);
    CHECK(initial.allocated_bytes==after.allocated_bytes&&initial.live_bytes==after.live_bytes);
    if(before)CHECK(after.work>initial.work);else CHECK(after.work==initial.work);
    size_t length=0;char *text=xjson_stringify(result,&length);CHECK(text&&length);puts(text);lsp_json_free(text);xjson_free(result);workspace_close(f);
    fprintf(stderr,"WORKSPACE_CACHE before=%u index_ledger_work_delta=%llu store_and_producer_death=1 physical0/0 JSON0/0 app0/0\n",before?1u:0u,(unsigned long long)(after.work-initial.work));
}
static int workspace_once(const WorkspaceOptions *options,WorkspaceMetrics *metrics) {
    WorkspaceFixture f;workspace_fixture(&f,options);
    if(options->scenario==14){workspace_start(&f);workspace_admission(&f);atomic_store(&f.gate.release,true);workspace_close(&f);return 0;}
    if(options->scenario==15){workspace_death(&f);return 0;}
    if(options->scenario==12||options->scenario==13){workspace_cache(&f,options->uri,options->scenario==13);return 0;}
    workspace_start(&f);size_t begin=runtime_attempts;task_resource_created_count=0;task_last_created_resources=NULL;
    task_axis_limits=options->limits;runtime_fail_at=options->source_fault==SIZE_MAX?SIZE_MAX:begin+options->source_fault;
    lsp_json_calls=0;lsp_json_fail=options->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,f.params,workspace_mode(&f));
    bool fault=options->source_fault!=SIZE_MAX||options->json_fault!=SIZE_MAX;
    if(options->budget)CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
    else if(fault)CHECK(status==XR_XIR_OK||status==XR_XIR_OUT_OF_MEMORY);else CHECK(status==XR_XIR_OK);
    XrCompileResources *ledger=task_last_created_resources;
    if(options->scenario==18)xjson_object_set(f.params,"query",xjson_new_string("absent"));
    xjson_free(f.params);f.params=NULL;xlsp_document_store_free(f.server.doc_table);f.server.doc_table=NULL;
    xlsp_source_index_store_free(f.server.source_index);f.server.source_index=NULL;
    xr_compile_resources_release(f.server.index_resources);f.server.index_resources=NULL;
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
    unsigned stopped=f.server.exit_received?1u:0u;workspace_close(&f);
    if(options->report)fprintf(stderr,"WORKSPACE source=%zu json=%zu source_hit=%u json_hit=%u exit=%u allocated=%llu peak=%llu work=%llu created=%zu producer_dead=1 physical0/0 JSON0/0 app0/0\n",
        metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,stopped,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,(unsigned long long)metrics->stats.work,task_resource_created_count);
    return options->scenario==2?task_cancel_check(22):0;
}
static bool workspace_resource_scenario(unsigned scenario) {return scenario==0||scenario==9||scenario==10||scenario==11;}
static int workspace_main(int argc,char **argv) {
    CHECK(source_request_mode("workspace/symbol")==22&&source_request_mode("textDocument/formatting")==20);
    CHECK(source_request_mode("textDocument/inlayHint")==-1&&source_request_mode("textDocument/signatureHelp")==15);
    CHECK(argc>=6);WorkspaceOptions options={.uri=argv[1],.library=argv[3],.method=(unsigned)strtoul(argv[4],NULL,10),.source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};
    CHECK(options.method==0);WorkspaceMetrics metrics={0};
    if(!strcmp(argv[5],"--resource")){CHECK(argc==9);options.source_fault=(size_t)strtoull(argv[6],NULL,10);
        options.json_fault=(size_t)strtoull(argv[7],NULL,10);options.scenario=(unsigned)strtoul(argv[8],NULL,10);
        CHECK(workspace_resource_scenario(options.scenario));return workspace_once(&options,&metrics);}
    if(!strcmp(argv[5],"--chunk")) {
        CHECK(argc==11);bool source=!strcmp(argv[6],"source");CHECK(source||!strcmp(argv[6],"json"));
        size_t start=(size_t)strtoull(argv[7],NULL,10),count=(size_t)strtoull(argv[8],NULL,10),total=(size_t)strtoull(argv[9],NULL,10);
        CHECK(count&&start<total&&count<=total-start);options.report=false;options.scenario=(unsigned)strtoul(argv[10],NULL,10);
        CHECK(workspace_resource_scenario(options.scenario));
        for(size_t i=0;i<count;++i){options.source_fault=source?start+i:SIZE_MAX;options.json_fault=source?SIZE_MAX:start+i;
            CHECK(!workspace_once(&options,&metrics));}
        fprintf(stderr,"WORKSPACE_CHUNK domain=%s start=%zu count=%zu total=%zu checked=%zu physical0/0 JSON0/0 app0/0\n",
            argv[6],start,count,total,count);return 0;
    }
    if(!strcmp(argv[5],"--axes")) {
        CHECK(argc==10);options.report=false;options.scenario=(unsigned)strtoul(argv[9],NULL,10);
        CHECK(workspace_resource_scenario(options.scenario));
        XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10),(uint64_t)strtoull(argv[8],NULL,10)};
        CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits cut=baseline;
            uint64_t *bound=axis==0?&cut.allocated_bytes:axis==1?&cut.live_bytes:&cut.work;CHECK(*bound&&*bound<INT64_MAX);
            *bound=(uint64_t)((int64_t)*bound+delta);options.limits=&cut;options.budget=delta<0;CHECK(!workspace_once(&options,&metrics));}
        fprintf(stderr,"WORKSPACE_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0\n");return 0;
    }
    CHECK(argc==6);options.scenario=(unsigned)strtoul(argv[5],NULL,10);CHECK(options.scenario<=18);
    return workspace_once(&options,&metrics);
}
#endif // LSP_WORKSPACE_TASK_CASES_H
