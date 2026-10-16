/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_SIGNATURE_TASK_CASES_H
#define LSP_SIGNATURE_TASK_CASES_H
#include "app/lsp/xlsp_handlers_textdoc.h"
static const char signature_text[]="import { mix } from \"./lib\"\nfn main()->i64 {\n return mix(\n  1,\n  2\n )\n}\n";
static const char signature_library[]="export fn mix(left:i64,right:i64)->i64 {return left+right}\n";
typedef struct SignatureOptions {
    const char *uri,*library;
    unsigned scenario;
    size_t source_fault,json_fault;
    const XrCompileResourceLimits *limits;
    bool budget,report;
} SignatureOptions;
typedef struct SignatureMetrics {size_t source,json;bool source_hit,json_hit;XrCompileResourceStats stats;} SignatureMetrics;
typedef struct SignatureFixture {
    XrLspServer server;
    XrLspAsync *async;
    XrLspTransport *transport;
    Gate gate,after;
    XlspRequestId id;
    XrJsonValue *params;
} SignatureFixture;
static void signature_fixture(SignatureFixture *f,const SignatureOptions *options) {
    memset(f,0,sizeof(*f));f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.transport=f->transport=xlsp_transport_stdio();CHECK(f->transport);
    f->server.doc_table=xlsp_document_store_new(destroy_document);CHECK(f->server.doc_table);
    const char *text=signature_text,*library=signature_library;unsigned row=4,column=2;
    if(options->scenario==10) {
        text="import { mix } from \"./lib\"\nfn main()->i64 {\n var slot=1\n return mix(\n  ref slot,\n  2\n )\n}\n";
        library="export fn mix(left:ref i64,right:i64)->i64 {return left+right}\n";row=5;
    }
    if(options->scenario==11) {
        text="import { value } from \"./lib\"\nfn main()->i64 {\n return value()\n}\n";
        library="export fn value()->i64 {return 1}\n";row=2;column=14;
    }
    if(options->scenario==12){row=6;column=1;}
    if(options->scenario==16)
        text="import { mix as chosen } from \"./lib\"\nfn main()->i64 {\n return chosen(\n  1,\n  2\n )\n}\n";
    if(options->scenario==17) {
        text="import { Box } from \"./lib\"\nfn main()->i64 {\n const box=Box{value:1}\n return box.add(\n  2\n )\n}\n";
        library="export struct Box {\n const value:i64\n add(other:i64)->i64 {return this.value+other}\n}\n";
    }
    if(options->scenario==18||options->scenario==19||options->scenario==23) {
        library="export fn mix<T>(left:T,right:T)->T {return left}\n";
        if(options->scenario==18)
            text="import { mix } from \"./lib\"\nfn main()->i64 {\n return mix<i64>(\n  1,\n  2\n )\n}\n";
        if(options->scenario==23)
            text="import { mix } from \"./lib\"\nfn main()->i64 {\n return mix(\n  mix(1,2),\n  3\n )\n}\n";
    }
    if(options->scenario==20) {
        text="import { Box } from \"./lib\"\nfn main()->i64 {\n const box=Box<i64>{value:1}\n return box.add(\n  2\n )\n}\n";
        library="export struct Box<T> {\n const value:T\n add(other:T)->T {return other}\n}\n";
    }
    if(options->scenario==21) {
        text="import { Box } from \"./lib\"\nfn main()->i64 {\n return Box<i64>.pick(\n  2\n )\n}\n";row=3;
        library="export struct Box<T> {\n const value:T\n static pick(value:T)->T {return value}\n}\n";
    }
    if(options->scenario==22) {
        text="import { mix } from \"./lib\"\nfn main()->(i64,i64) {\n return mix<i64>(\n  1,\n  2\n )\n}\n";
        library="export fn mix<T>(left:T,right:T)->(T,T) {return (left,right)}\n";
    }
    if(options->scenario==24) {
        text="import { mix } from \"./lib\"\nfn wrapper<Item>(value:Item)->Item {\n return mix(\n  value,\n  value\n )\n}\nfn main()->i64 {return wrapper(1)}\n";
        library="export fn mix<T>(left:T,right:T)->T {return left}\n";
    }
    add_document(&f->server,options->uri,text,1);add_document(&f->server,options->library,library,7);
    f->params=parameters(options->uri);XrJsonValue *position=xjson_get_object(f->params,"position");
    xjson_get(position,"line")->as.integer=row;xjson_get(position,"character")->as.integer=column;
    f->id=identity();XlspRequestId owned={0};CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,"textDocument/signatureHelp",99));
}
static void signature_close(SignatureFixture *f) {
    if(f->async)xlsp_async_free(f->async);
    xlsp_source_workspace_free(f->server.source_workspace);xjson_free(f->params);
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count&&f->server.next_request_id==71);
    xlsp_request_id_free(&f->id);xlsp_document_store_free(f->server.doc_table);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static void signature_start(SignatureFixture *f) {
    f->server.async=f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);
    XrLspTask *task=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(task);
    CHECK(xlsp_async_submit(f->async,task));uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
}
static void signature_sync(SignatureFixture *f) {
    XrJsonValue *result=xlsp_handle_td_signature_help(&f->server,f->params);CHECK(result&&f->server.source_failure.status==XR_XIR_OK);
    xlsp_source_workspace_free(f->server.source_workspace);f->server.source_workspace=NULL;
    xlsp_document_store_free(f->server.doc_table);f->server.doc_table=NULL;
    CHECK(!runtime_live&&!runtime_bytes);size_t length=0;char *text=xjson_stringify(result,&length);CHECK(text&&length);
    puts(text);lsp_json_free(text);xjson_free(result);
    fprintf(stderr,"SIGNATURE_SYNC actual_public_handler=1 Source_owner_dead=1 physical0/0\n");
}
static void signature_death(SignatureFixture *f) {
    XlspSourceRequest *request=NULL;CHECK(xlsp_navigation_capture(&f->server,f->params,4,&request)==XR_XIR_OK);
    CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    xlsp_document_store_free(f->server.doc_table);xjson_free(f->params);f->params=NULL;
    memset(&f->server,0xa5,sizeof(f->server));_Atomic bool cancel=false;XlspSourceRequestResult result={0},again={0};
    CHECK(xlsp_source_request_run(request,&cancel,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&result.json&&!result.token_update);
    CHECK(xlsp_source_request_run(request,&cancel,&again)==XR_XIR_BAD_STRUCTURE&&!again.json);
    xlsp_source_request_free(request);CHECK(!runtime_live&&!runtime_bytes);
    size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);puts(text);
    lsp_json_free(text);xjson_free(result.json);xlsp_request_id_free(&f->id);xlsp_transport_free(f->transport);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
    fprintf(stderr,"SIGNATURE_DEATH server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0\n");
}
static void signature_admission(SignatureFixture *f) {
    const char *keys[]={"textDocument","position","uri","line","character"};
    for(unsigned i=0;i<5;++i) {
        XrJsonValue *params=xjson_clone(f->params);CHECK(params);
        XrJsonValue *object=i<2?params:i==2?xjson_get_object(params,"textDocument"):xjson_get_object(params,"position");
        CHECK(index_json_member(object,keys[i],xjson_clone(xjson_get(object,keys[i]))));
        XlspPendingRequests pending=f->server.pending_requests;
        CHECK(!xlsp_handle_td_signature_help(&f->server,params)&&f->server.source_failure.status==XR_XIR_BAD_STRUCTURE);
        CHECK(xlsp_source_task_submit(&f->server,&f->id,params,15)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&pending,&f->server.pending_requests,sizeof(pending))&&!runtime_live&&!runtime_bytes);
        xjson_free(params);
    }
    fprintf(stderr,"SIGNATURE_ADMISSION duplicate_known=5 synchronous_and_async=10 physical0/0\n");
}
static int signature_once(const SignatureOptions *options,SignatureMetrics *metrics) {
    SignatureFixture f;signature_fixture(&f,options);
    if(options->scenario==9){signature_sync(&f);signature_close(&f);return 0;}
    if(options->scenario==14){signature_start(&f);signature_admission(&f);atomic_store(&f.gate.release,true);signature_close(&f);return 0;}
    if(options->scenario==15){signature_death(&f);return 0;}
    signature_start(&f);size_t begin=runtime_attempts;task_resource_created_count=0;task_last_created_resources=NULL;
    task_axis_limits=options->limits;runtime_fail_at=options->source_fault==SIZE_MAX?SIZE_MAX:begin+options->source_fault;
    lsp_json_calls=0;lsp_json_fail=options->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,f.params,15);
    if(options->scenario==13){CHECK(status==XR_XIR_BAD_STRUCTURE);atomic_store(&f.gate.release,true);signature_close(&f);
        fprintf(stderr,"SIGNATURE_BEFORE mode15_rejected=1 physical0/0\n");return 0;}
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
    unsigned stopped=f.server.exit_received?1u:0u;signature_close(&f);
    if(options->report)fprintf(stderr,"SIGNATURE source=%zu json=%zu source_hit=%u json_hit=%u exit=%u allocated=%llu peak=%llu work=%llu created=1 producer_dead=1 physical0/0 JSON0/0 app0/0\n",
        metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,stopped,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,(unsigned long long)metrics->stats.work);
    return options->scenario==2?task_cancel_check(15):0;
}
static int signature_main(int argc,char **argv) {
    CHECK(argc>=5);SignatureOptions options={.uri=argv[1],.library=argv[3],.source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};
    SignatureMetrics metrics={0};
    if(!strcmp(argv[4],"--resource")){CHECK(argc==8);options.source_fault=(size_t)strtoull(argv[5],NULL,10);
        options.json_fault=(size_t)strtoull(argv[6],NULL,10);options.scenario=(unsigned)strtoul(argv[7],NULL,10);
        CHECK(options.scenario==0||(options.scenario>=10&&options.scenario<=12)||(options.scenario>=18&&options.scenario<=24));return signature_once(&options,&metrics);}
    if(!strcmp(argv[4],"--chunk")) {
        CHECK(argc==10);bool source=!strcmp(argv[5],"source");CHECK(source||!strcmp(argv[5],"json"));
        size_t start=(size_t)strtoull(argv[6],NULL,10),count=(size_t)strtoull(argv[7],NULL,10),total=(size_t)strtoull(argv[8],NULL,10);
        CHECK(count&&start<total&&count<=total-start);options.report=false;options.scenario=(unsigned)strtoul(argv[9],NULL,10);
        CHECK(options.scenario==0||(options.scenario>=10&&options.scenario<=12)||(options.scenario>=18&&options.scenario<=24));
        for(size_t i=0;i<count;++i){options.source_fault=source?start+i:SIZE_MAX;options.json_fault=source?SIZE_MAX:start+i;
            CHECK(!signature_once(&options,&metrics));}
        fprintf(stderr,"SIGNATURE_CHUNK domain=%s start=%zu count=%zu total=%zu checked=%zu physical0/0 JSON0/0 app0/0\n",
            argv[5],start,count,total,count);return 0;
    }
    if(!strcmp(argv[4],"--axes")) {
        CHECK(argc==9);options.report=false;options.scenario=(unsigned)strtoul(argv[8],NULL,10);
        CHECK(options.scenario==0||(options.scenario>=10&&options.scenario<=12)||(options.scenario>=18&&options.scenario<=24));
        XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[5],NULL,10),(uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10)};
        CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&baseline.work<=UINT64_C(128000000));
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits cut=baseline;
            uint64_t *bound=axis==0?&cut.allocated_bytes:axis==1?&cut.live_bytes:&cut.work;CHECK(*bound&&*bound<INT64_MAX);
            *bound=(uint64_t)((int64_t)*bound+delta);options.limits=&cut;options.budget=delta<0;CHECK(!signature_once(&options,&metrics));}
        fprintf(stderr,"SIGNATURE_AXES checked=9 producer_dead=1 physical0/0 JSON0/0 app0/0\n");return 0;
    }
    CHECK(argc==5);options.scenario=(unsigned)strtoul(argv[4],NULL,10);CHECK(options.scenario<=24);
    return signature_once(&options,&metrics);
}
#endif // LSP_SIGNATURE_TASK_CASES_H
