/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_EDIT_TASK_CASES_H
#define LSP_EDIT_TASK_CASES_H
#include "app/lsp/xlsp_hierarchy_request.h"
#include "app/lsp/xlsp_source_syntax.h"
#include "app/lsp/xlsp_source_format.h"
#include "app/lsp/xlsp_source_on_type.h"
#include "app/lsp/xlsp_source_open.h"
static const char edit_format_text[]="fn value()->string{return \"\xf0\x9f\x98\x80\"}";
static const char edit_indent_text[]="/* \xf0\x9f\x98\x80 } */fn run(){\r\n";
static bool edit_before_limits_seen;
static XrCompileResourceStatus edit_before_resources(const XrCompileResourceLimits *limits,XrCompileResources **out) {
    CHECK(limits->allocated_bytes==UINT64_C(1)<<30&&limits->live_bytes==UINT64_C(256)<<20&&limits->work==UINT64_C(8)<<30);
    edit_before_limits_seen=true;return task_resource_new_impl(limits,out);
}
/* The selected handlers only need the real store lookup, not its document owner. */
XR_FUNC XrLspDocument *xlsp_document_get(XrLspServer *server,const char *uri) {
    return server?xlsp_document_store_get(server->doc_table,uri):NULL;
}
#include "edit_handlers.inc.c"
#include "before_handlers.inc.c"
typedef struct EditOptions {
    const char *uri,*library;
    unsigned scenario,method;
    size_t source_fault,json_fault;
    const XrCompileResourceLimits *limits;
    bool budget,report;
} EditOptions;
typedef struct EditMetrics {size_t source,json;bool source_hit,json_hit;XrCompileResourceStats stats;} EditMetrics;
typedef struct EditFixture {
    XrLspServer server;
    XrLspAsync *async;
    XrLspTransport *transport;
    Gate gate,after;
    XlspRequestId id;
    XrJsonValue *params;
    unsigned method;
} EditFixture;
static unsigned edit_mode(const EditFixture *f) {return f->method?21u:20u;}
static XlspSourceScopeQueryKind edit_kind(const EditFixture *f) {return f->method?XLSP_SOURCE_ON_TYPE:XLSP_SOURCE_FORMATTING;}
static const char *edit_method(const EditFixture *f) {return f->method?"textDocument/onTypeFormatting":"textDocument/formatting";}
static void edit_fixture(EditFixture *f,const EditOptions *options) {
    memset(f,0,sizeof(*f));f->method=options->method;f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.transport=f->transport=xlsp_transport_stdio();CHECK(f->transport);
    f->server.doc_table=xlsp_document_store_new(destroy_document);CHECK(f->server.doc_table);
    f->server.config.ignore_pattern_count=-1;f->server.config.format_max_line_length=80;
    const char *text=options->scenario==10?"":f->method?edit_indent_text:edit_format_text;
    if(options->scenario==9)text=f->method?"fn run(){/*\n":"fn broken(";
    if(options->scenario==11)text=f->method?"fn run(){\nif(true){\n       }":"fn value()->i64{\r\nreturn 1\r\n}\r\n";
    char *owned_text=NULL;
    if(options->scenario==2) {
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
    const char settings[]="{\"tabSize\":4,\"insertSpaces\":true}";
    XrJsonValue *config=xjson_parse(settings,sizeof(settings)-1);CHECK(config);CHECK(index_json_member(f->params,"options",config));
    if(f->method) {
        unsigned line=options->scenario==10?0u:options->scenario==2||options->scenario==11?2u:1u;
        unsigned character=options->scenario==11?8u:0u;
        if(options->scenario==16){line=0;character=4;}
        if(options->scenario==17)line=99;
        char coordinates[96];int count=snprintf(coordinates,sizeof(coordinates),"{\"line\":%u,\"character\":%u}",line,character);
        CHECK(count>0&&(size_t)count<sizeof(coordinates));
        XrJsonValue *position=xjson_parse(coordinates,(size_t)count);CHECK(position);
        CHECK(index_json_member(f->params,"position",position));
        CHECK(index_json_member(f->params,"ch",xjson_new_string(options->scenario==11?"}":"\n")));
    }
    f->id=identity();XlspRequestId owned={0};CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,edit_method(f),99));
}
static void edit_close(EditFixture *f) {
    if(f->async)xlsp_async_free(f->async);
    xlsp_source_workspace_free(f->server.source_workspace);xjson_free(f->params);
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count&&f->server.next_request_id==71);
    xlsp_request_id_free(&f->id);xlsp_document_store_free(f->server.doc_table);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static void edit_start(EditFixture *f) {
    f->server.async=f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);
    XrLspTask *task=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(task);
    CHECK(xlsp_async_submit(f->async,task));uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
}
static void edit_death(EditFixture *f) {
    XlspHierarchyRequest *request=NULL;CHECK(xlsp_hierarchy_request_capture(&f->server,f->params,0,edit_kind(f),&request)==XR_XIR_OK);
    CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    xlsp_document_store_free(f->server.doc_table);xjson_free(f->params);f->params=NULL;
    memset(&f->server,0xa5,sizeof(f->server));_Atomic bool cancel=false;XlspSourceRequestResult result={0},again={0};
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&result.json&&!result.token_update);
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&again)==XR_XIR_BAD_STRUCTURE&&!again.json);
    xlsp_hierarchy_request_free(request);CHECK(!runtime_live&&!runtime_bytes);
    size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result.json);xlsp_request_id_free(&f->id);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
    fprintf(stderr,"EDIT_DEATH server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0\n");
}
static void edit_reject(EditFixture *f,XrJsonValue *params) {
    XlspPendingRequests pending=f->server.pending_requests;XlspHierarchyRequest *request=NULL;
    CHECK(xlsp_hierarchy_request_capture(&f->server,params,0,edit_kind(f),&request)==XR_XIR_BAD_STRUCTURE&&!request);
    CHECK(xlsp_source_task_submit(&f->server,&f->id,params,edit_mode(f))==XR_XIR_BAD_STRUCTURE);
    CHECK(!memcmp(&pending,&f->server.pending_requests,sizeof(pending))&&!runtime_live&&!runtime_bytes);
}
static void edit_admission(EditFixture *f) {
    const char *keys[]={"textDocument","uri","options","tabSize","insertSpaces","position","line","character","ch"};
    unsigned count=f->method?9u:5u;
    for(unsigned i=0;i<count;++i) {
        XrJsonValue *params=xjson_clone(f->params);CHECK(params);
        XrJsonValue *object=i==0||i==2||i==5||i==8?params:i==1?xjson_get_object(params,"textDocument"):
            i<5?xjson_get_object(params,"options"):xjson_get_object(params,"position");
        CHECK(index_json_member(object,keys[i],xjson_clone(xjson_get(object,keys[i]))));
        edit_reject(f,params);xjson_free(params);
    }
    const struct {const char *object,*key,*value;} invalid[]={
        {"options","tabSize","0"},{"options","tabSize","-1"},{"options","tabSize","2147483648"},
        {"options","tabSize","4.5"},{"options","insertSpaces","\"true\""},{NULL,"options","[]"},
        {"textDocument","uri","123"},{"textDocument","uri","\"file:\\u0000bad\""},{NULL,"textDocument","[]"},
        {"position","line","-1"},{"position","line","2147483648"},{"position","line","1.5"},
        {"position","character","-1"},{"position","character","2147483648"},{"position","character","false"},
        {NULL,"position","[]"},{NULL,"ch","\"}}\""},{NULL,"ch","\"x\""},{NULL,"ch","\"\""}
    };
    unsigned bad=f->method?(unsigned)(sizeof(invalid)/sizeof(*invalid)):9u;
    for(unsigned i=0;i<bad;++i) {
        XrJsonValue *params=xjson_clone(f->params),*value=xjson_parse(invalid[i].value,strlen(invalid[i].value));CHECK(params&&value);
        XrJsonValue *object=invalid[i].object?xjson_get_object(params,invalid[i].object):params;CHECK(object);
        xjson_object_set(object,invalid[i].key,value);CHECK(xjson_get(object,invalid[i].key)==value);
        edit_reject(f,params);xjson_free(params);
    }
    const char *uri=xjson_get_string(xjson_get_object(f->params,"textDocument"),"uri");
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,uri);CHECK(doc);doc->server=NULL;
    edit_reject(f,f->params);doc->server=&f->server;
    if(!f->method){f->server.config.format_max_line_length=-1;edit_reject(f,f->params);f->server.config.format_max_line_length=80;}
    fprintf(stderr,"EDIT_ADMISSION method=%u duplicate=%u invalid_shapes=%u wrong_owner=1 negative_format_config=%u physical0/0\n",f->method,count,bad,f->method?0u:1u);
}
static void edit_cache(EditFixture *f,const char *uri,bool before) {
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,uri);CHECK(doc);
    if(before) {
        CHECK(index_json_member(xjson_get_object(f->params,"options"),"tabSize",xjson_new_number(999)));
        XrJsonValue *result=f->method?before_xlsp_on_type_handle(&f->server,f->params):before_xlsp_format_handle(&f->server,f->params);
        CHECK(result&&edit_before_limits_seen);
        size_t length=0;char *text=xjson_stringify(result,&length);CHECK(text&&length);puts(text);
        lsp_json_free(text);xjson_free(result);edit_close(f);
        fprintf(stderr,"EDIT_BEFORE method=%u duplicate_options_accepted=1 original_CLI_limits=1 physical0/0 JSON0/0 app0/0\n",f->method);return;
    }
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;
    CHECK(task_resource_new_impl(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    const char cached[]="fn cached(){}\n";XlspSourceDocument input={doc->uri,cached,strlen(doc->uri),sizeof(cached)-1,0};
    CHECK(xlsp_source_syntax_build(session,&input,&doc->syntax_snapshot)==XR_PARSE_OK);
    doc->ast=(AstNode *)xlsp_source_syntax_ast(doc->syntax_snapshot);uintptr_t owner=(uintptr_t)doc->syntax_snapshot,ast=(uintptr_t)doc->ast;
    xr_compile_session_free(session);XrCompileResourceStats old=sdk_stats(resources);
    XrJsonValue *result=f->method?xlsp_on_type_handle(&f->server,f->params):xlsp_format_handle(&f->server,f->params);
    CHECK(result&&f->server.source_failure.status==XR_XIR_OK&&(uintptr_t)doc->ast==ast&&(uintptr_t)doc->syntax_snapshot==owner);
    XrCompileResourceStats after=sdk_stats(resources);
    CHECK(old.allocated_bytes==after.allocated_bytes&&old.live_bytes==after.live_bytes&&old.work==after.work);
    size_t length=0;char *text=xjson_stringify(result,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result);doc->ast=NULL;xlsp_source_syntax_free(doc->syntax_snapshot);doc->syntax_snapshot=NULL;
    xr_compile_resources_release(resources);edit_close(f);
    fprintf(stderr,"EDIT_CACHE method=%u cache_AST_and_original_ledger_preserved=1 physical0/0 JSON0/0 app0/0\n",f->method);
}
static int edit_once(const EditOptions *options,EditMetrics *metrics) {
    EditFixture f;edit_fixture(&f,options);
    if(options->scenario==14){edit_start(&f);edit_admission(&f);atomic_store(&f.gate.release,true);edit_close(&f);return 0;}
    if(options->scenario==15){edit_death(&f);return 0;}
    if(options->scenario==12||options->scenario==13){edit_cache(&f,options->uri,options->scenario==13);return 0;}
    edit_start(&f);size_t begin=runtime_attempts;task_resource_created_count=0;task_last_created_resources=NULL;
    task_axis_limits=options->limits;runtime_fail_at=options->source_fault==SIZE_MAX?SIZE_MAX:begin+options->source_fault;
    lsp_json_calls=0;lsp_json_fail=options->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,f.params,edit_mode(&f));
    bool fault=options->source_fault!=SIZE_MAX||options->json_fault!=SIZE_MAX;
    if(options->budget)CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
    else if(fault)CHECK(status==XR_XIR_OK||status==XR_XIR_OUT_OF_MEMORY);else CHECK(status==XR_XIR_OK);
    XrCompileResources *ledger=task_last_created_resources;
    if(options->scenario==18) {
        f.server.config.format_max_line_length=-1;f.server.config.format_wrap_long_lines=true;
        XrJsonValue *config=xjson_get_object(f.params,"options");
        xjson_get(config,"tabSize")->as.integer=1;xjson_get(config,"insertSpaces")->as.boolean=false;
    }
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
    unsigned stopped=f.server.exit_received?1u:0u;edit_close(&f);
    if(options->report)fprintf(stderr,"EDIT source=%zu json=%zu source_hit=%u json_hit=%u exit=%u allocated=%llu peak=%llu work=%llu created=%zu producer_dead=1 physical0/0 JSON0/0 app0/0\n",
        metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,stopped,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,(unsigned long long)metrics->stats.work,task_resource_created_count);
    return options->scenario==2?task_cancel_check(20u+options->method):0;
}
static bool edit_resource_scenario(unsigned scenario) {return scenario==0||scenario==9||scenario==10||scenario==11;}
static int edit_main(int argc,char **argv) {
    CHECK(source_request_mode("textDocument/foldingRange")==17&&source_request_mode("textDocument/formatting")==20&&source_request_mode("textDocument/onTypeFormatting")==21);
    CHECK(source_request_mode("textDocument/inlayHint")==-1&&source_request_mode("textDocument/signatureHelp")==15);
    CHECK(argc>=6);EditOptions options={.uri=argv[1],.library=argv[3],.method=(unsigned)strtoul(argv[4],NULL,10),.source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};
    CHECK(options.method<=1);EditMetrics metrics={0};
    if(!strcmp(argv[5],"--resource")){CHECK(argc==9);options.source_fault=(size_t)strtoull(argv[6],NULL,10);
        options.json_fault=(size_t)strtoull(argv[7],NULL,10);options.scenario=(unsigned)strtoul(argv[8],NULL,10);
        CHECK(edit_resource_scenario(options.scenario));return edit_once(&options,&metrics);}
    if(!strcmp(argv[5],"--chunk")) {
        CHECK(argc==11);bool source=!strcmp(argv[6],"source");CHECK(source||!strcmp(argv[6],"json"));
        size_t start=(size_t)strtoull(argv[7],NULL,10),count=(size_t)strtoull(argv[8],NULL,10),total=(size_t)strtoull(argv[9],NULL,10);
        CHECK(count&&start<total&&count<=total-start);options.report=false;options.scenario=(unsigned)strtoul(argv[10],NULL,10);
        CHECK(edit_resource_scenario(options.scenario));
        for(size_t i=0;i<count;++i){options.source_fault=source?start+i:SIZE_MAX;options.json_fault=source?SIZE_MAX:start+i;
            CHECK(!edit_once(&options,&metrics));}
        fprintf(stderr,"EDIT_CHUNK domain=%s start=%zu count=%zu total=%zu checked=%zu physical0/0 JSON0/0 app0/0\n",
            argv[6],start,count,total,count);return 0;
    }
    if(!strcmp(argv[5],"--axes")) {
        CHECK(argc==10);options.report=false;options.scenario=(unsigned)strtoul(argv[9],NULL,10);
        CHECK(edit_resource_scenario(options.scenario));
        XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10),(uint64_t)strtoull(argv[8],NULL,10)};
        CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits cut=baseline;
            uint64_t *bound=axis==0?&cut.allocated_bytes:axis==1?&cut.live_bytes:&cut.work;CHECK(*bound&&*bound<INT64_MAX);
            *bound=(uint64_t)((int64_t)*bound+delta);options.limits=&cut;options.budget=delta<0;CHECK(!edit_once(&options,&metrics));}
        fprintf(stderr,"EDIT_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0\n");return 0;
    }
    CHECK(argc==6);options.scenario=(unsigned)strtoul(argv[5],NULL,10);CHECK(options.scenario<=18);
    return edit_once(&options,&metrics);
}
#endif // LSP_EDIT_TASK_CASES_H
