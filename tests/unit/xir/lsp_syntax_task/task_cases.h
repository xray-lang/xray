/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_SYNTAX_TASK_CASES_H
#define LSP_SYNTAX_TASK_CASES_H
#include "app/lsp/xlsp_hierarchy_request.h"
#include "app/lsp/xlsp_source_syntax.h"
#include "app/lsp/xlsp_source_selection.h"
#include "app/lsp/xlsp_source_open.h"
static const char syntax_selection_text[]="A\xf0\x9f\x98\x80Word\r\n \tfoo_2\n";
static const char syntax_symbol_text[]=
    "/* \xf0\x9f\x98\x80 */ fn before()->i64 {\n return 1\n}\n"
    "struct Box {\n const value:i64\n answer()->i64 { return this.value }\n}\n"
    "enum Tone {\n Red,\n Blue\n}\n";
static bool syntax_before_limits_seen;
static XrCompileResourceStatus syntax_before_resources(const XrCompileResourceLimits *limits,XrCompileResources **out) {
    CHECK(limits->allocated_bytes==UINT64_C(1)<<30&&limits->live_bytes==UINT64_C(256)<<20&&limits->work==UINT64_C(8)<<30);
    syntax_before_limits_seen=true;return task_resource_new_impl(limits,out);
}
XR_FUNC XrXirStatus before_xlsp_source_selection_json(XrCompileResources *,const char *,size_t,const XrJsonValue *,XrJsonValue **);
/* The selected handlers only need the real store lookup, not its document owner. */
XR_FUNC XrLspDocument *xlsp_document_get(XrLspServer *server,const char *uri) {
    return server?xlsp_document_store_get(server->doc_table,uri):NULL;
}
#include "syntax_handlers.inc.c"
#include "before_handlers.inc.c"
typedef struct SyntaxOptions {
    const char *uri,*library;
    unsigned scenario,method;
    size_t source_fault,json_fault;
    const XrCompileResourceLimits *limits;
    bool budget,report;
} SyntaxOptions;
typedef struct SyntaxMetrics {size_t source,json;bool source_hit,json_hit;XrCompileResourceStats stats;} SyntaxMetrics;
typedef struct SyntaxFixture {
    XrLspServer server;
    XrLspAsync *async;
    XrLspTransport *transport;
    Gate gate,after;
    XlspRequestId id;
    XrJsonValue *params;
    unsigned method;
} SyntaxFixture;
static unsigned syntax_mode(const SyntaxFixture *f) {return f->method?19u:18u;}
static XlspSourceScopeQueryKind syntax_kind(const SyntaxFixture *f) {return f->method?XLSP_SOURCE_DOCUMENT_SYMBOLS:XLSP_SOURCE_SELECTION;}
static const char *syntax_method(const SyntaxFixture *f) {return f->method?"textDocument/documentSymbol":"textDocument/selectionRange";}
static void syntax_fixture(SyntaxFixture *f,const SyntaxOptions *options) {
    memset(f,0,sizeof(*f));f->method=options->method;f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.transport=f->transport=xlsp_transport_stdio();CHECK(f->transport);
    f->server.doc_table=xlsp_document_store_new(destroy_document);CHECK(f->server.doc_table);
    f->server.config.ignore_pattern_count=-1;
    const char *text=options->scenario==10?"":options->method?syntax_symbol_text:syntax_selection_text;
    char *owned_text=NULL;
    if(options->scenario==9&&options->method) {
        size_t length=strlen(text);owned_text=sdk_fixture_malloc(length+3);CHECK(owned_text);
        memcpy(owned_text,text,length);memcpy(owned_text+length,"$\n",3);text=owned_text;
    }
    if(options->scenario==2||options->scenario==11) {
        size_t length=strlen(text);owned_text=sdk_fixture_malloc(length+8198);CHECK(owned_text);
        memcpy(owned_text,"/*",2);memset(owned_text+2,'a',8192);memcpy(owned_text+8194,"*/\n",3);
        memcpy(owned_text+8197,text,length+1);text=owned_text;
    }
    add_document(&f->server,options->uri,text,1);sdk_fixture_free(owned_text);
    add_document(&f->server,options->library,"\xff",7);
    xlsp_document_store_get(f->server.doc_table,options->uri)->sem_token_result_id=UINT32_MAX;
    f->params=xjson_new_object();XrJsonValue *document=xjson_new_object();CHECK(f->params&&document);
    CHECK(index_json_member(document,"uri",xjson_new_string(options->uri)));
    CHECK(index_json_member(f->params,"textDocument",document));
    if(!options->method) {
        bool shifted=options->scenario==2||options->scenario==11;
        const char *points=options->scenario==10?"[{\"line\":0,\"character\":0}]":shifted?
            "[{\"line\":1,\"character\":4},{\"line\":2,\"character\":4},{\"line\":3,\"character\":0}]":
            "[{\"line\":0,\"character\":4},{\"line\":1,\"character\":4},{\"line\":2,\"character\":0}]";
        XrJsonValue *positions=xjson_parse(points,strlen(points));CHECK(positions);
        CHECK(index_json_member(f->params,"positions",positions));
        if(options->scenario==16)xjson_get(positions->as.array.items[0],"character")->as.integer=2;
        if(options->scenario==17)xjson_get(positions->as.array.items[2],"line")->as.integer=99;
    }
    f->id=identity();XlspRequestId owned={0};CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,syntax_method(f),99));
}
static void syntax_close(SyntaxFixture *f) {
    if(f->async)xlsp_async_free(f->async);
    xlsp_source_workspace_free(f->server.source_workspace);xjson_free(f->params);
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count&&f->server.next_request_id==71);
    xlsp_request_id_free(&f->id);xlsp_document_store_free(f->server.doc_table);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static void syntax_start(SyntaxFixture *f) {
    f->server.async=f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);
    XrLspTask *task=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(task);
    CHECK(xlsp_async_submit(f->async,task));uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
}
static void syntax_death(SyntaxFixture *f) {
    XlspHierarchyRequest *request=NULL;CHECK(xlsp_hierarchy_request_capture(&f->server,f->params,0,syntax_kind(f),&request)==XR_XIR_OK);
    CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    xlsp_document_store_free(f->server.doc_table);xjson_free(f->params);f->params=NULL;
    memset(&f->server,0xa5,sizeof(f->server));_Atomic bool cancel=false;XlspSourceRequestResult result={0},again={0};
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&result.json&&!result.token_update);
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&again)==XR_XIR_BAD_STRUCTURE&&!again.json);
    xlsp_hierarchy_request_free(request);CHECK(!runtime_live&&!runtime_bytes);
    size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result.json);xlsp_request_id_free(&f->id);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
    fprintf(stderr,"SYNTAX_DEATH server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0\n");
}
static void syntax_admission(SyntaxFixture *f) {
    const char *keys[]={"textDocument","uri","positions","line","character"};unsigned count=f->method?2u:5u;
    for(unsigned i=0;i<count;++i) {
        XrJsonValue *params=xjson_clone(f->params);CHECK(params);
        XrJsonValue *object=i==0||i==2?params:i==1?xjson_get_object(params,"textDocument"):
            xjson_get(params,"positions")->as.array.items[0];
        CHECK(index_json_member(object,keys[i],xjson_clone(xjson_get(object,keys[i]))));
        XlspPendingRequests pending=f->server.pending_requests;XlspHierarchyRequest *request=NULL;
        CHECK(xlsp_hierarchy_request_capture(&f->server,params,0,syntax_kind(f),&request)==XR_XIR_BAD_STRUCTURE&&!request);
        CHECK(xlsp_source_task_submit(&f->server,&f->id,params,syntax_mode(f))==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&pending,&f->server.pending_requests,sizeof(pending))&&!runtime_live&&!runtime_bytes);
        if(i>=3) {
            const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
            XrCompileResources *resources=sdk_ledger(&limits);XrJsonValue *output=NULL;
            CHECK(xlsp_source_selection_json(resources,syntax_selection_text,sizeof(syntax_selection_text)-1,
                xjson_get(params,"positions"),&output)==XR_XIR_BAD_STRUCTURE&&!output);
            xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
        }
        xjson_free(params);
    }
    const char *uri=xjson_get_string(xjson_get_object(f->params,"textDocument"),"uri");
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,uri);CHECK(doc);doc->server=NULL;
    XlspHierarchyRequest *request=NULL;
    CHECK(xlsp_hierarchy_request_capture(&f->server,f->params,0,syntax_kind(f),&request)==XR_XIR_BAD_STRUCTURE&&!request);
    CHECK(xlsp_source_task_submit(&f->server,&f->id,f->params,syntax_mode(f))==XR_XIR_BAD_STRUCTURE);doc->server=&f->server;
    if(!f->method)for(unsigned i=0;i<8;++i) {
        XrJsonValue *params=xjson_clone(f->params);CHECK(params);XrJsonValue *positions=xjson_get(params,"positions");
        XrJsonValue *point=positions->as.array.items[0],*value=xjson_get(point,i<3?"line":"character");
        if(i<6){if(i%3==0)value->as.integer=-1;else if(i%3==1)value->as.integer=(int64_t)INT32_MAX+1;
            else{value->is_integer=false;value->as.number=1.5;}}
        else if(i==6) {XrJsonValue *object=xjson_new_object();CHECK(object);xjson_object_set(params,"positions",object);}
        else {XrJsonValue *null=xjson_new_null();CHECK(null);xjson_free(point);positions->as.array.items[0]=null;}
        CHECK(xlsp_hierarchy_request_capture(&f->server,params,0,syntax_kind(f),&request)==XR_XIR_BAD_STRUCTURE&&!request);
        CHECK(xlsp_source_task_submit(&f->server,&f->id,params,syntax_mode(f))==XR_XIR_BAD_STRUCTURE);
        xjson_free(params);
    }
    CHECK(!runtime_live&&!runtime_bytes);
    fprintf(stderr,"SYNTAX_ADMISSION method=%u duplicate=%u invalid_coordinates=%u physical0/0\n",f->method,count,f->method?0u:8u);
}
static void syntax_document_cache_before(SyntaxFixture *f,XrLspDocument *doc) {
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *resources=sdk_ledger(&limits);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XlspSourceDocument input={doc->uri,doc->content,strlen(doc->uri),doc->length,doc->version};
    CHECK(xlsp_source_open_prepare(&context,&input,1,0,&doc->source_open)==XR_XIR_OK&&doc->source_open);
    unsigned query_status=(unsigned)xlsp_source_open_query_status(doc->source_open);
    doc->syntax_snapshot=xlsp_source_open_take_syntax(doc->source_open);CHECK(doc->syntax_snapshot);
    doc->ast=(AstNode *)xlsp_source_syntax_ast(doc->syntax_snapshot);
    XrCompileResourceStats before=sdk_stats(resources);
    XrJsonValue *result=before_xlsp_handle_td_document_symbol(&f->server,f->params);CHECK(result);
    XrCompileResourceStats after=sdk_stats(resources);
    CHECK(after.allocated_bytes>before.allocated_bytes&&after.work>before.work&&after.live_bytes==before.live_bytes);
    size_t length=0;char *text=xjson_stringify(result,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result);doc->ast=NULL;
    xlsp_source_syntax_free(doc->syntax_snapshot);doc->syntax_snapshot=NULL;
    xlsp_source_open_free(doc->source_open);doc->source_open=NULL;xr_compile_resources_release(resources);syntax_close(f);
    fprintf(stderr,"SYNTAX_BEFORE method=1 real_SourceOpen=1 query_status=%u cached_ledger_debited=1 allocated_delta=%llu work_delta=%llu physical0/0 JSON0/0 app0/0\n",
        query_status,(unsigned long long)(after.allocated_bytes-before.allocated_bytes),(unsigned long long)(after.work-before.work));
}
static void syntax_cache(SyntaxFixture *f,const char *uri,bool before) {
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,uri);CHECK(doc);
    if(before) {
        if(f->method) {
            syntax_document_cache_before(f,doc);return;
        }
        XrJsonValue *point=xjson_get(f->params,"positions")->as.array.items[0];
        CHECK(index_json_member(point,"line",xjson_new_number(999)));
        XrJsonValue *result=before_xlsp_handle_selection_range(&f->server,f->params);
        CHECK(result&&syntax_before_limits_seen&&f->server.source_failure.status==XR_XIR_OK);
        size_t length=0;char *text=xjson_stringify(result,&length);CHECK(text&&length);puts(text);
        lsp_json_free(text);xjson_free(result);syntax_close(f);
        fprintf(stderr,"SYNTAX_BEFORE method=0 duplicate_coordinate_accepted=1 original_CLI_limits=1 physical0/0 JSON0/0 app0/0\n");return;
    }
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;
    CHECK(task_resource_new_impl(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    const char cached[]="fn cached(){}\n";XlspSourceDocument input={doc->uri,cached,strlen(doc->uri),sizeof(cached)-1,0};
    CHECK(xlsp_source_syntax_build(session,&input,&doc->syntax_snapshot)==XR_PARSE_OK);
    doc->ast=(AstNode *)xlsp_source_syntax_ast(doc->syntax_snapshot);uintptr_t owner=(uintptr_t)doc->syntax_snapshot,ast=(uintptr_t)doc->ast;
    xr_compile_session_free(session);XrCompileResourceStats old=sdk_stats(resources);
    XrJsonValue *result=f->method?xlsp_handle_td_document_symbol(&f->server,f->params):xlsp_handle_selection_range(&f->server,f->params);
    CHECK(result&&f->server.source_failure.status==XR_XIR_OK&&(uintptr_t)doc->ast==ast&&(uintptr_t)doc->syntax_snapshot==owner);
    XrCompileResourceStats after=sdk_stats(resources);
    CHECK(old.allocated_bytes==after.allocated_bytes&&old.live_bytes==after.live_bytes&&old.work==after.work);
    size_t length=0;char *text=xjson_stringify(result,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result);doc->ast=NULL;xlsp_source_syntax_free(doc->syntax_snapshot);doc->syntax_snapshot=NULL;
    xr_compile_resources_release(resources);syntax_close(f);
    fprintf(stderr,"SYNTAX_CACHE method=%u cache_AST_and_original_ledger_preserved=1 physical0/0 JSON0/0 app0/0\n",f->method);
}
static int syntax_once(const SyntaxOptions *options,SyntaxMetrics *metrics) {
    SyntaxFixture f;syntax_fixture(&f,options);
    if(options->scenario==14){syntax_start(&f);syntax_admission(&f);atomic_store(&f.gate.release,true);syntax_close(&f);return 0;}
    if(options->scenario==15){syntax_death(&f);return 0;}
    if(options->scenario==12||options->scenario==13){syntax_cache(&f,options->uri,options->scenario==13);return 0;}
    syntax_start(&f);size_t begin=runtime_attempts;task_resource_created_count=0;task_last_created_resources=NULL;
    task_axis_limits=options->limits;runtime_fail_at=options->source_fault==SIZE_MAX?SIZE_MAX:begin+options->source_fault;
    lsp_json_calls=0;lsp_json_fail=options->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,f.params,syntax_mode(&f));
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
    unsigned stopped=f.server.exit_received?1u:0u;syntax_close(&f);
    if(options->report)fprintf(stderr,"SYNTAX source=%zu json=%zu source_hit=%u json_hit=%u exit=%u allocated=%llu peak=%llu work=%llu created=%zu producer_dead=1 physical0/0 JSON0/0 app0/0\n",
        metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,stopped,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,(unsigned long long)metrics->stats.work,task_resource_created_count);
    return options->scenario==2?task_cancel_check(18u+options->method):0;
}
static bool syntax_resource_scenario(unsigned scenario) {return scenario==0||scenario==9||scenario==10||scenario==11;}
static int syntax_main(int argc,char **argv) {
    CHECK(source_request_mode("textDocument/foldingRange")==17&&source_request_mode("textDocument/selectionRange")==18&&source_request_mode("textDocument/documentSymbol")==19);
    CHECK(source_request_mode("textDocument/inlayHint")==-1&&source_request_mode("textDocument/signatureHelp")==15);
    CHECK(argc>=6);SyntaxOptions options={.uri=argv[1],.library=argv[3],.method=(unsigned)strtoul(argv[4],NULL,10),.source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};
    CHECK(options.method<=1);SyntaxMetrics metrics={0};
    if(!strcmp(argv[5],"--resource")){CHECK(argc==9);options.source_fault=(size_t)strtoull(argv[6],NULL,10);
        options.json_fault=(size_t)strtoull(argv[7],NULL,10);options.scenario=(unsigned)strtoul(argv[8],NULL,10);
        CHECK(syntax_resource_scenario(options.scenario));return syntax_once(&options,&metrics);}
    if(!strcmp(argv[5],"--chunk")) {
        CHECK(argc==11);bool source=!strcmp(argv[6],"source");CHECK(source||!strcmp(argv[6],"json"));
        size_t start=(size_t)strtoull(argv[7],NULL,10),count=(size_t)strtoull(argv[8],NULL,10),total=(size_t)strtoull(argv[9],NULL,10);
        CHECK(count&&start<total&&count<=total-start);options.report=false;options.scenario=(unsigned)strtoul(argv[10],NULL,10);
        CHECK(syntax_resource_scenario(options.scenario));
        for(size_t i=0;i<count;++i){options.source_fault=source?start+i:SIZE_MAX;options.json_fault=source?SIZE_MAX:start+i;
            CHECK(!syntax_once(&options,&metrics));}
        fprintf(stderr,"SYNTAX_CHUNK domain=%s start=%zu count=%zu total=%zu checked=%zu physical0/0 JSON0/0 app0/0\n",
            argv[6],start,count,total,count);return 0;
    }
    if(!strcmp(argv[5],"--axes")) {
        CHECK(argc==10);options.report=false;options.scenario=(unsigned)strtoul(argv[9],NULL,10);
        CHECK(syntax_resource_scenario(options.scenario));
        XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10),(uint64_t)strtoull(argv[8],NULL,10)};
        CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits cut=baseline;
            uint64_t *bound=axis==0?&cut.allocated_bytes:axis==1?&cut.live_bytes:&cut.work;CHECK(*bound&&*bound<INT64_MAX);
            *bound=(uint64_t)((int64_t)*bound+delta);options.limits=&cut;options.budget=delta<0;CHECK(!syntax_once(&options,&metrics));}
        fprintf(stderr,"SYNTAX_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0\n");return 0;
    }
    CHECK(argc==6);options.scenario=(unsigned)strtoul(argv[5],NULL,10);CHECK(options.scenario<=17);
    return syntax_once(&options,&metrics);
}
#endif // LSP_SYNTAX_TASK_CASES_H
