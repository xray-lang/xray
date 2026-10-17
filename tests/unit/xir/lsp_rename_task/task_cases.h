/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * task_cases.h - Real rename task admission, lifetime and terminal ownership
 *
 * KEY CONCEPT:
 *   Workers consume captured source and configuration after editor death.
 */
#ifndef LSP_RENAME_TASK_CASES_H
#define LSP_RENAME_TASK_CASES_H
#include "app/lsp/xlsp_hierarchy_request.h"
#include "app/lsp/xlsp_source_syntax.h"
#include "toolchain/xcompiler_session.h"
#include "rename_handlers.inc.c"
typedef struct RenameTaskOptions {
    const char *uri,*library,*closed,*root;
    unsigned method,scenario;
    size_t source_fault,json_fault;
    const XrCompileResourceLimits *limits;
    bool budget,report;
} RenameTaskOptions;
typedef struct RenameTaskMetrics {
    size_t source,json;bool source_hit,json_hit;XrCompileResourceStats stats;
} RenameTaskMetrics;
typedef struct RenameTaskFixture {
    XrLspServer server;XrLspAsync *async;XrLspTransport *transport;
    Gate gate,after;XlspRequestId id;XrJsonValue *params;
    XlspIgnorePattern ignore;char ignored[32];unsigned method;
} RenameTaskFixture;
static unsigned rename_task_mode(const RenameTaskFixture *f){return f->method?24u:23u;}
static XlspSourceScopeQueryKind rename_task_kind(const RenameTaskFixture *f){
    return (XlspSourceScopeQueryKind)(f->method?14u:13u);
}
static void rename_task_fixture(RenameTaskFixture *f,const RenameTaskOptions *o) {
    memset(f,0,sizeof(*f));f->method=o->method;f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.transport=f->transport=xlsp_transport_stdio();CHECK(f->transport);
    f->server.doc_table=xlsp_document_store_new(destroy_document);CHECK(f->server.doc_table);
    strcpy(f->ignored,"ignored.xr");f->ignore=(XlspIgnorePattern){.pattern=f->ignored,.is_glob=true};
    f->server.config.ignore_patterns=&f->ignore;f->server.config.ignore_pattern_count=1;f->server.config.ignore_pattern_capacity=1;
    f->server.workspace_folder_count=1;f->server.workspace_folders[0].path=(char *)o->root;
    const char *text="import {target as alias} from \"./lib\"\nfn run()->i64{return alias(1)}\n";
    if(o->scenario==14)text="import \"./lib\"\nfn run()->i64{return lib.target(1)}\n";
    if(o->scenario==15)text="/* \xf0\x9f\x98\x80 */ import {target as alias} from \"./lib\"\r\nfn run()->i64{return alias(1)}\r\n";
    add_document(&f->server,o->uri,text,7);
    add_document(&f->server,o->library,"export fn target(value:i64)->i64{return value}\n",9);
    xlsp_document_store_get(f->server.doc_table,o->uri)->sem_token_result_id=UINT32_MAX;
    xlsp_document_store_get(f->server.doc_table,o->library)->sem_token_result_id=UINT32_MAX;
    f->params=xjson_new_object();XrJsonValue *doc=xjson_new_object();CHECK(f->params&&doc);
    CHECK(index_json_member(doc,"uri",xjson_new_string(o->scenario==13?o->library:o->uri)));
    CHECK(index_json_member(f->params,"textDocument",doc));
    const char *point=o->scenario==13?"{\"line\":0,\"character\":12}":
        o->scenario==17?"{\"line\":99,\"character\":0}":"{\"line\":1,\"character\":22}";
    CHECK(index_json_member(f->params,"position",xjson_parse(point,strlen(point))));
    if(!o->method)CHECK(index_json_member(f->params,"newName",xjson_new_string(
        o->scenario==15?"changed\xf0\x9f\x98\x80":o->scenario==16?"run":"changed")));
    f->id=identity();XlspRequestId owned={0};CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,
        o->method?"textDocument/prepareRename":"textDocument/rename",99));
}
static void rename_task_start(RenameTaskFixture *f) {
    f->server.async=f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);
    XrLspTask *task=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(task);
    CHECK(xlsp_async_submit(f->async,task));uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
}
static void rename_task_close(RenameTaskFixture *f) {
    if(f->async)xlsp_async_free(f->async);
    xjson_free(f->params);
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count&&f->server.next_request_id==71);
    xlsp_request_id_free(&f->id);xlsp_document_store_free(f->server.doc_table);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static void rename_task_sync(RenameTaskFixture *f,bool cached) {
    XrCompileResources *resources=NULL;XrCompileResourceStats before={0};
    XrLspDocument *doc=NULL;uintptr_t syntax=0,ast=0;
    if(cached) {
        const char *uri=xjson_get_string(xjson_get_object(f->params,"textDocument"),"uri");
        doc=xlsp_document_store_get(f->server.doc_table,uri);CHECK(doc);
        const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
        CHECK(task_resource_new_impl(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
        const char text[]="fn unrelated(){}\n";XlspSourceDocument input={uri,text,strlen(uri),sizeof(text)-1,0};
        CHECK(xlsp_source_syntax_build(session,&input,&doc->syntax_snapshot)==XR_PARSE_OK);
        doc->ast=(AstNode *)xlsp_source_syntax_ast(doc->syntax_snapshot);
        syntax=(uintptr_t)doc->syntax_snapshot;ast=(uintptr_t)doc->ast;
        xr_compile_session_free(session);before=sdk_stats(resources);
    }
    XrJsonValue *result=f->method?xlsp_handle_td_prepare_rename(&f->server,f->params):xlsp_handle_td_rename(&f->server,f->params);
    CHECK(result&&!f->server.source_failure.stage&&f->server.source_failure.status==XR_XIR_OK);
    if(cached) {
        XrCompileResourceStats after=sdk_stats(resources);
        CHECK((uintptr_t)doc->syntax_snapshot==syntax&&(uintptr_t)doc->ast==ast);
        CHECK(before.allocated_bytes==after.allocated_bytes&&before.live_bytes==after.live_bytes&&before.work==after.work);
        doc->ast=NULL;xlsp_source_syntax_free(doc->syntax_snapshot);doc->syntax_snapshot=NULL;
        xr_compile_resources_release(resources);
    }
    xlsp_document_store_free(f->server.doc_table);f->server.doc_table=NULL;
    xjson_free(f->params);f->params=NULL;CHECK(!runtime_live&&!runtime_bytes);
    size_t length=0;char *text=xjson_stringify(result,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result);rename_task_close(f);
    fprintf(stderr,"RENAME_SYNC captured_owner_dead=1 cached_AST_ledger_preserved=%u physical0/0 JSON0/0 app0/0\n",cached?1u:0u);
}
static void rename_task_death(RenameTaskFixture *f) {
    XlspHierarchyRequest *request=NULL;
    CHECK(xlsp_hierarchy_request_capture(&f->server,f->params,0,rename_task_kind(f),&request)==XR_XIR_OK&&request);
    xjson_free(f->params);f->params=NULL;xlsp_document_store_free(f->server.doc_table);f->server.doc_table=NULL;
    XrLspServer saved=f->server;memset(&f->server,0xa5,sizeof(f->server));
    XlspSourceRequestResult result={0},again={0};
    CHECK(xlsp_hierarchy_request_run(request,NULL,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&!result.stage&&result.json);
    size_t calls=runtime_attempts;
    CHECK(xlsp_hierarchy_request_run(request,NULL,&again)==XR_XIR_BAD_STRUCTURE&&!again.json&&calls==runtime_attempts);
    xlsp_hierarchy_request_free(request);CHECK(!runtime_live&&!runtime_bytes);
    size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result.json);f->server=saved;rename_task_close(f);
    fprintf(stderr,"RENAME_DEATH server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0\n");
}
#include "admission_cases.h"
static int rename_task_once(const RenameTaskOptions *o,RenameTaskMetrics *metrics) {
    RenameTaskFixture f;rename_task_fixture(&f,o);
    if(o->scenario==9||o->scenario==11){rename_task_sync(&f,o->scenario==11);return 0;}
    if(o->scenario==12){rename_task_death(&f);return 0;}
    rename_task_start(&f);
    if(o->scenario==10){rename_task_admission(&f);atomic_store(&f.gate.release,true);rename_task_close(&f);return 0;}
    size_t begin=runtime_attempts;task_resource_created_count=0;task_last_created_resources=NULL;
    task_axis_limits=o->limits;runtime_fail_at=o->source_fault==SIZE_MAX?SIZE_MAX:begin+o->source_fault;
    lsp_json_calls=0;lsp_json_fail=o->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,f.params,rename_task_mode(&f));
    bool fault=o->source_fault!=SIZE_MAX||o->json_fault!=SIZE_MAX;
    if(o->budget)CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
    else if(fault)CHECK(status==XR_XIR_OK||status==XR_XIR_OUT_OF_MEMORY);else CHECK(status==XR_XIR_OK);
    XrCompileResources *ledger=task_last_created_resources;
    if(o->scenario==18) {
        f.server.config.ignore_pattern_count=-1;f.server.workspace_folders[0].path="invalid";
        if(!f.method)xjson_get(f.params,"newName")->as.string[0]='?';
    }
    xjson_free(f.params);f.params=NULL;xlsp_document_store_free(f.server.doc_table);f.server.doc_table=NULL;
    if(status==XR_XIR_OK) {
        XrLspTask *after=xlsp_task_new(block,block_complete,&f.after,XLSP_TASK_NORMAL);CHECK(after);CHECK(xlsp_async_submit(f.async,after));
        if(o->scenario==1)CHECK(xlsp_request_cancel(&f.server,&f.id));
        if(o->scenario==3)++f.server.source_revision;
        if(o->scenario==4)f.server.exit_received=true;
        if(o->scenario==5)f.server.transport=NULL;
        if(o->scenario==6) {
            CHECK(xlsp_pending_request_remove(&f.server.pending_requests,&f.id));XlspRequestId owned={0};
            CHECK(xlsp_request_id_clone(&f.id,&owned)==XLSP_PROTOCOL_OK);
            CHECK(xlsp_pending_request_add(&f.server.pending_requests,owned,"replacement",100));
            xlsp_pending_request_find(&f.server.pending_requests,&f.id)->source_task_id=UINT64_MAX-1;
        }
        if(o->scenario==7)f.server.shutdown_received=true;
        if(o->scenario==2)task_cancel_arm();
        atomic_store(&f.gate.release,true);
        if(o->scenario==2){task_cancel_wait();CHECK(xlsp_request_cancel(&f.server,&f.id));task_cancel_release();}
        uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load(&f.after.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
        CHECK(task_resource_created_count==1&&ledger&&ledger==task_last_created_resources);metrics->stats=sdk_stats(ledger);
        if(o->scenario==8)CHECK(xlsp_request_cancel(&f.server,&f.id));
        atomic_store(&f.after.release,true);
    } else {metrics->stats=task_last_stats;atomic_store(&f.gate.release,true);}
    unsigned completed=0;uint64_t deadline=xr_time_monotonic_ms()+10000;
    while(completed<(status==XR_XIR_OK?3u:1u)){completed+=(unsigned)xlsp_async_poll(f.async);CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    metrics->source=runtime_attempts-begin;metrics->json=lsp_json_calls;
    metrics->source_hit=o->source_fault!=SIZE_MAX&&runtime_attempts>runtime_fail_at;metrics->json_hit=lsp_json_injected;
    task_last_created_resources=NULL;task_axis_limits=NULL;runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(fault)CHECK(metrics->source_hit||metrics->json_hit);
    if(o->scenario==6) {
        XlspPendingRequest *replacement=xlsp_pending_request_find(&f.server.pending_requests,&f.id);
        CHECK(replacement&&replacement->source_task_id==UINT64_MAX-1&&!replacement->cancelled);
    }
    rename_task_close(&f);
    if(o->scenario==2)CHECK(!task_cancel_check(rename_task_mode(&f)));
    if(o->report)fprintf(stderr,"RENAME_METRICS method=%u scenario=%u source=%zu json=%zu source_hit=%u json_hit=%u allocated=%llu peak=%llu work=%llu producer_dead=1 physical0/0 JSON0/0 app0/0\n",
        o->method,o->scenario,metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,(unsigned long long)metrics->stats.work);
    return 0;
}
#include "resource_cases.h"
#endif // LSP_RENAME_TASK_CASES_H
