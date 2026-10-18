/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_SEMANTIC_TASK_CASES_H
#define LSP_SEMANTIC_TASK_CASES_H
#include "app/lsp/xlsp_hierarchy_request.h"
#include "app/lsp/xlsp_token_update.h"
static const char semantic_text[]="/* 😀 */ fn value(v:i64)->i64{return v}\r\nconst answer=value(1)\r\n";
static const char semantic_changed_text[]="/* 😀 */ fn value(v:i64)->i64{return v}\r\nconst answer=value(100)+value(2)\r\n";
typedef struct SemanticRun {
    const char *uri;
    unsigned method,scenario;
    size_t source_fault,json_fault;
    bool report;
    const XrCompileResourceLimits *limits;
    bool budget_expected;
} SemanticRun;
typedef struct SemanticMetrics {
    size_t source,json;
    bool source_hit,json_hit;
    XrCompileResourceStats stats;
} SemanticMetrics;
typedef struct SemanticFixture {
    XrLspServer server;
    XrLspTransport *transport;
    XrLspAsync *async;
    Gate gate,after;
    XlspRequestId id,second;
    XrJsonValue *params;
    uintptr_t cache_address;
    uint64_t cache_hash,next_hash;
    uint32_t cache_id;
    unsigned tasks;
} SemanticFixture;
static void semantic_destroy_document(XrLspDocument *doc) {
    if(!doc)return;
    xlsp_source_tokens_free(doc->source_tokens);doc->source_tokens=NULL;destroy_document(doc);
}
static uint64_t semantic_hash(const XlspSourceTokens *tokens) {
    size_t count=0;const XlspSourceToken *items=xlsp_source_tokens_items(tokens,&count);
    CHECK(tokens&&items&&count);uint64_t hash=UINT64_C(1469598103934665603);
    for(size_t i=0;i<count;++i){
        uint32_t words[]={items[i].line,items[i].column,items[i].length,items[i].type,items[i].modifiers};
        for(unsigned j=0;j<5;++j){hash^=words[j];hash*=UINT64_C(1099511628211);}
    }
    return hash;
}
static XrJsonValue *semantic_params(const char *uri,unsigned method) {
    XrJsonValue *params=parameters(uri);
    if(method==12)CHECK(index_json_member(params,"previousResultId",xjson_new_string("1")));
    if(method==13){
        const char text[]="{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":1,\"character\":0}}";
        CHECK(index_json_member(params,"range",xjson_parse(text,sizeof(text)-1)));
    }
    return params;
}
static uint32_t semantic_json_word(XrJsonValue *array,int index) {
    XrJsonValue *value=xjson_array_get(array,index);CHECK(value&&value->type==XR_JSON_NUMBER);
    double number=value->is_integer?(double)value->as.integer:value->as.number;
    CHECK(number>=0&&number<=UINT32_MAX&&(double)(uint32_t)number==number);return (uint32_t)number;
}
static uint64_t semantic_json_hash(XrJsonValue *json) {
    XrJsonValue *data=xjson_get_array(json,"data");CHECK(data&&data->as.array.count&&data->as.array.count%5==0);
    uint32_t line=0,column=0;uint64_t hash=UINT64_C(1469598103934665603);
    for(int i=0;i<data->as.array.count;i+=5){
        uint32_t delta=semantic_json_word(data,i),offset=semantic_json_word(data,i+1);
        CHECK(delta<=UINT32_MAX-line);line+=delta;
        if(!delta){CHECK(offset<=UINT32_MAX-column);column+=offset;}else column=offset;
        uint32_t words[]={line,column,semantic_json_word(data,i+2),semantic_json_word(data,i+3),semantic_json_word(data,i+4)};
        for(unsigned j=0;j<5;++j){hash^=words[j];hash*=UINT64_C(1099511628211);}
    }
    return hash;
}
static void semantic_base(SemanticFixture *f,const SemanticRun *options) {
    memset(f,0,sizeof(*f));f->server.initialized=true;f->server.source_revision=41;f->server.next_request_id=71;
    f->server.doc_table=xlsp_document_store_new(semantic_destroy_document);CHECK(f->server.doc_table);
    add_document(&f->server,options->uri,semantic_text,7);
    XrJsonValue *params=semantic_params(options->uri,11),*baseline=xlsp_semantic_handle(&f->server,params,0);
    CHECK(baseline&&f->server.source_failure.status==XR_XIR_OK&&!f->server.source_failure.stage);
    if(options->report){
        size_t length=0;char *text=xjson_stringify(baseline,&length);CHECK(text);
        fprintf(stderr,"BASELINE %s\n",text);lsp_json_free(text);
    }
    xjson_free(params);xjson_free(baseline);
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,options->uri);
    CHECK(doc&&doc->source_tokens&&doc->sem_token_result_id==1&&!f->server.source_workspace);
    f->cache_address=(uintptr_t)doc->source_tokens;f->cache_hash=semantic_hash(doc->source_tokens);f->cache_id=1;
    f->next_hash=f->cache_hash;
    f->params=semantic_params(options->uri,options->method);
}
static void semantic_start(SemanticFixture *f) {
    f->transport=xlsp_transport_stdio();CHECK(f->transport);f->server.transport=f->transport;
    f->async=xlsp_async_new(NULL,NULL);CHECK(f->async);f->server.async=f->async;
    XrLspTask *barrier=xlsp_task_new(block,block_complete,&f->gate,XLSP_TASK_NORMAL);CHECK(barrier);
    CHECK(xlsp_async_submit(f->async,barrier));f->tasks=1;
    uint64_t deadline=xr_time_monotonic_ms()+5000;
    while(!atomic_load(&f->gate.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    f->id=identity();XlspRequestId owned={0};CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
    CHECK(xlsp_pending_request_add(&f->server.pending_requests,owned,"semantic/query",99));
}
static void semantic_cleanup(SemanticFixture *f) {
    xjson_free(f->params);f->params=NULL;
    if(f->async){xlsp_async_free(f->async);f->async=NULL;f->server.async=NULL;}
    if(xlsp_pending_request_find(&f->server.pending_requests,&f->id))
        CHECK(xlsp_pending_request_remove(&f->server.pending_requests,&f->id));
    CHECK(!f->server.pending_requests.count);
    xlsp_request_id_free(&f->id);xlsp_request_id_free(&f->second);
    xlsp_document_store_free(f->server.doc_table);f->server.doc_table=NULL;
    xlsp_transport_free(f->transport);f->transport=NULL;
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
}
static void semantic_rejected(SemanticFixture *f,unsigned method,XrXirStatus expected) {
    size_t blocks=runtime_live,bytes=runtime_bytes,json=lsp_json_live,json_bytes=lsp_json_bytes;
    size_t app=task_live,app_bytes=task_bytes;XlspPendingRequests pending=f->server.pending_requests;
    CHECK(xlsp_source_task_submit(&f->server,&f->id,f->params,method)==expected);
    CHECK(!memcmp(&pending,&f->server.pending_requests,sizeof(pending)));
    CHECK(runtime_live==blocks&&runtime_bytes==bytes&&lsp_json_live==json&&lsp_json_bytes==json_bytes);
    CHECK(task_live==app&&task_bytes==app_bytes);
}
static void semantic_admission(SemanticFixture *f,const SemanticRun *options) {
    unsigned checked=0;
    XrJsonValue *document=xjson_get_object(f->params,"textDocument");
    xjson_object_set_new(document,"uri",xjson_new_string(options->uri));
    semantic_rejected(f,options->method,XR_XIR_BAD_STRUCTURE);++checked;
    // Restore exact input ownership instead of mutating a duplicate away.
    xjson_free(f->params);f->params=semantic_params(options->uri,options->method);
    xjson_object_set_new(f->params,"textDocument",xjson_new_object());
    semantic_rejected(f,options->method,XR_XIR_BAD_STRUCTURE);++checked;
    xjson_free(f->params);f->params=semantic_params(options->uri,options->method);
    if(options->method==12){
        xjson_object_set(f->params,"previousResultId",xjson_new_number(1));
        semantic_rejected(f,options->method,XR_XIR_BAD_STRUCTURE);++checked;
        xjson_free(f->params);f->params=semantic_params(options->uri,options->method);
        xjson_object_set_new(f->params,"previousResultId",xjson_new_string("1"));
        semantic_rejected(f,options->method,XR_XIR_BAD_STRUCTURE);++checked;
        xjson_free(f->params);f->params=semantic_params(options->uri,options->method);
    }
    if(options->method==13){
        for(unsigned bad=0;bad<4;++bad){
            XrJsonValue *start=xjson_get_object(xjson_get_object(f->params,"range"),"start");
            XrJsonValue *line=xjson_get(start,"line");
            if(bad==0){line->is_integer=false;line->as.number=0.5;}
            if(bad==1)line->as.integer=-1;
            if(bad==2)line->as.integer=(int64_t)INT32_MAX+1;
            if(bad==3)xjson_object_set_new(start,"line",xjson_new_number(0));
            semantic_rejected(f,options->method,XR_XIR_BAD_STRUCTURE);++checked;
            xjson_free(f->params);f->params=semantic_params(options->uri,options->method);
        }
    }
    for(size_t i=0;i<3;++i){
        task_fail=task_attempts+i;semantic_rejected(f,options->method,XR_XIR_OUT_OF_MEMORY);
        CHECK(task_attempts==task_fail+1);task_fail=SIZE_MAX;
        ++checked;
    }
    fprintf(stderr,"SEM_ADMISSION method=%u checks=%u\n",options->method,checked);
}
static void semantic_steer_before(SemanticFixture *f,const SemanticRun *options) {
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,options->uri);CHECK(doc);
    if(options->scenario==10){doc->sem_token_result_id=UINT32_MAX;f->cache_id=UINT32_MAX;}
    if(options->scenario==12)xjson_object_set_new(xjson_get_object(f->params,"textDocument"),"uri",xjson_new_string(options->uri));
    if(options->scenario==13&&options->method==12)xjson_object_set(f->params,"previousResultId",xjson_new_string("unknown"));
    if(options->scenario==19&&options->method==13){
        XrJsonValue *start=xjson_get_object(xjson_get_object(f->params,"range"),"start");
        xjson_get(start,"character")->as.integer=4;
    }
    if(options->scenario==14)semantic_admission(f,options);
    if(options->scenario==21){
        memset(doc->content,'?',doc->length);sdk_fixture_free(doc->content);
        doc->content=xr_strdup(semantic_changed_text);CHECK(doc->content);doc->length=sizeof(semantic_changed_text)-1;
        ++doc->version;++f->server.source_revision;
        XrJsonValue *params=semantic_params(options->uri,11);XlspHierarchyRequest *request=NULL;
        CHECK(xlsp_hierarchy_request_capture(&f->server,params,0,XLSP_SOURCE_SEMANTIC_FULL,&request)==XR_XIR_OK);
        xjson_free(params);XlspSourceRequestResult result={0};
        CHECK(xlsp_hierarchy_request_run(request,NULL,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&result.json);
        xlsp_hierarchy_request_free(request);f->next_hash=semantic_json_hash(result.json);
        size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);
        fprintf(stderr,"CHANGED %s\n",text);lsp_json_free(text);xjson_free(result.json);xlsp_token_update_free(result.token_update);
        CHECK(doc->sem_token_result_id==1&&(uintptr_t)doc->source_tokens==f->cache_address);
    }
}
static void semantic_steer_after(SemanticFixture *f,const SemanticRun *options) {
    XrLspServer *server=&f->server;
    if(options->scenario==1)CHECK(xlsp_request_cancel(server,&f->id));
    if(options->scenario==3){++server->source_revision;xlsp_document_store_free(server->doc_table);server->doc_table=NULL;}
    if(options->scenario==4)++server->source_revision;
    if(options->scenario==5)server->exit_received=true;
    if(options->scenario==6)server->transport=NULL;
    if(options->scenario==7)server->shutdown_received=true;
    if(options->scenario==8){
        XrJsonValue *params=semantic_params(options->uri,11),*json=xlsp_semantic_handle(server,params,0);
        CHECK(json);xjson_free(params);xjson_free(json);
        XrLspDocument *doc=xlsp_document_store_get(server->doc_table,options->uri);
        CHECK(doc&&doc->sem_token_result_id==2);f->cache_id=2;f->cache_address=(uintptr_t)doc->source_tokens;
    }
    if(options->scenario==9){
        CHECK(xlsp_pending_request_remove(&server->pending_requests,&f->id));XlspRequestId owned={0};
        CHECK(xlsp_request_id_clone(&f->id,&owned)==XLSP_PROTOCOL_OK);
        CHECK(xlsp_pending_request_add(&server->pending_requests,owned,"replacement",100));
        xlsp_pending_request_find(&server->pending_requests,&f->id)->source_task_id=UINT64_MAX-1;
    }
    if(options->scenario==16){
        XrLspTask *after=xlsp_task_new(block,block_complete,&f->after,XLSP_TASK_NORMAL);CHECK(after);
        CHECK(xlsp_async_submit(f->async,after));++f->tasks;
    }
    if(options->scenario==17){
        f->second=(XlspRequestId){.kind=XLSP_ID_NUMBER,.as.number=42};XlspRequestId owned={0};
        CHECK(xlsp_request_id_clone(&f->second,&owned)==XLSP_PROTOCOL_OK);
        CHECK(xlsp_pending_request_add(&server->pending_requests,owned,"semantic/concurrent",101));
        XrJsonValue *params=semantic_params(options->uri,11);
        CHECK(xlsp_source_task_submit(server,&f->second,params,11)==XR_XIR_OK);xjson_free(params);++f->tasks;
        XrLspTask *after=xlsp_task_new(block,block_complete,&f->after,XLSP_TASK_NORMAL);CHECK(after);
        CHECK(xlsp_async_submit(f->async,after));++f->tasks;
    }
}
static void semantic_finish_worker(SemanticFixture *f,const SemanticRun *options) {
    if(options->scenario==2){
        task_cancel_arm();atomic_store(&f->gate.release,true);task_cancel_wait();
        CHECK(xlsp_request_cancel(&f->server,&f->id));task_cancel_release();
    }else atomic_store(&f->gate.release,true);
    if(options->scenario==16||options->scenario==17){
        uint64_t deadline=xr_time_monotonic_ms()+5000;
        while(!atomic_load(&f->after.entered)){CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
        if(options->scenario==16)CHECK(xlsp_request_cancel(&f->server,&f->id));
        atomic_store(&f->after.release,true);
    }
    uint64_t deadline=xr_time_monotonic_ms()+10000;unsigned processed=0;
    while(processed<f->tasks){processed+=(unsigned)xlsp_async_poll(f->async);CHECK(xr_time_monotonic_ms()<deadline);xr_thread_sleep_ms(1);}
    CHECK(f->gate.completed==1&&((options->scenario!=16&&options->scenario!=17)||f->after.completed==1));
}
static void semantic_cache(SemanticFixture *f,const SemanticRun *options,bool failed) {
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,options->uri);
    if(options->scenario==3){CHECK(!doc);return;}
    CHECK(doc&&doc->source_tokens);
    bool accepted=!failed&&(options->scenario==0||options->scenario==7||options->scenario==13||options->scenario==14||options->scenario==21);
    bool advanced=(accepted&&options->method!=13)||options->scenario==17;
    uint32_t expected=advanced?f->cache_id+1:f->cache_id;
    CHECK(semantic_hash(doc->source_tokens)==(advanced?f->next_hash:f->cache_hash));
    CHECK(doc->sem_token_result_id==expected);
    if(advanced)CHECK((uintptr_t)doc->source_tokens!=f->cache_address);
    else CHECK((uintptr_t)doc->source_tokens==f->cache_address);
}
static int semantic_producer_death(const SemanticRun *options) {
    SemanticFixture f;semantic_base(&f,options);XlspHierarchyRequest *request=NULL;
    XlspSourceScopeQueryKind kind=(XlspSourceScopeQueryKind)(XLSP_SOURCE_SEMANTIC_FULL+options->method-11);
    CHECK(xlsp_hierarchy_request_capture(&f.server,f.params,0,kind,&request)==XR_XIR_OK);
    xjson_free(f.params);f.params=NULL;xlsp_document_store_free(f.server.doc_table);f.server.doc_table=NULL;
    memset(&f.server,0xa5,sizeof(f.server));
    _Atomic bool cancel=false;XlspSourceRequestResult result={0};
    CHECK(xlsp_hierarchy_request_run(request,&cancel,&result)==XR_XIR_OK&&result.status==XR_XIR_OK&&result.json&&!result.cancelled);
    XlspSourceRequestResult again={0};CHECK(xlsp_hierarchy_request_run(request,&cancel,&again)==XR_XIR_BAD_STRUCTURE&&!again.json&&!again.token_update);
    xlsp_hierarchy_request_free(request);request=NULL;
    CHECK((result.token_update!=NULL)==(options->method!=13));
    size_t length=0;char *text=xjson_stringify(result.json,&length);CHECK(text&&length);
    puts(text);lsp_json_free(text);xjson_free(result.json);xlsp_token_update_free(result.token_update);
    CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes&&!task_live&&!task_bytes);
    fprintf(stderr,"SEM_DEATH method=%u original_server_poisoned=1 one_shot=1 physical0/0 JSON0/0 app0/0\n",options->method);return 0;
}
static int semantic_once(const SemanticRun *options,SemanticMetrics *metrics) {
    if(options->scenario==15)return semantic_producer_death(options);
    task_axis_limits=NULL;
    SemanticFixture f;semantic_base(&f,options);semantic_start(&f);semantic_steer_before(&f,options);
    task_axis_limits=options->limits;task_last_stats=(XrCompileResourceStats){0};
    size_t source_begin=runtime_attempts;
    runtime_fail_at=options->source_fault==SIZE_MAX?SIZE_MAX:source_begin+options->source_fault;
    lsp_json_calls=0;lsp_json_fail=options->json_fault;lsp_json_injected=false;
    XrXirStatus status=xlsp_source_task_submit(&f.server,&f.id,f.params,options->method);
    if(options->scenario==20){
        CHECK(status==XR_XIR_BAD_STRUCTURE);atomic_store(&f.gate.release,true);semantic_cleanup(&f);
        fprintf(stderr,"BEFORE actual SourceTask rejected semantic mode=%u status=1 physical0/0 JSON0/0 app0/0\n",options->method);return 1;
    }
    bool admission_rejected=(options->scenario==10&&options->method!=13)||options->scenario==12;
    if(admission_rejected)CHECK(status==(options->scenario==10?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE));
    else if(options->budget_expected)CHECK(status==XR_XIR_OK||status==XR_XIR_BUDGET);
    else if(options->source_fault==SIZE_MAX)CHECK(status==XR_XIR_OK);
    else CHECK(status==XR_XIR_OK||status==XR_XIR_OUT_OF_MEMORY);
    xjson_free(f.params);f.params=NULL;
    if(status==XR_XIR_OK){++f.tasks;semantic_steer_after(&f,options);}
    else CHECK(xlsp_pending_request_find(&f.server.pending_requests,&f.id)->source_task_id==0);
    semantic_finish_worker(&f,options);
    metrics->source=runtime_attempts-source_begin;metrics->json=lsp_json_calls;
    metrics->source_hit=options->source_fault!=SIZE_MAX&&runtime_attempts>runtime_fail_at;
    metrics->json_hit=lsp_json_injected;
    metrics->stats=task_last_stats;task_axis_limits=NULL;
    runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    bool failed=options->source_fault!=SIZE_MAX||options->json_fault!=SIZE_MAX;
    if(failed)CHECK(metrics->source_hit||metrics->json_hit);
    semantic_cache(&f,options,failed||admission_rejected||options->budget_expected);
    if(options->scenario==9){
        XlspPendingRequest *pending=xlsp_pending_request_find(&f.server.pending_requests,&f.id);
        CHECK(pending&&pending->source_task_id==UINT64_MAX-1&&!pending->cancelled);
    }else if(status==XR_XIR_OK)CHECK(!f.server.pending_requests.count);
    CHECK(f.server.next_request_id==71&&!f.server.source_workspace);
    unsigned stopped=f.server.exit_received?1u:0u;semantic_cleanup(&f);
    if(options->report)fprintf(stderr,"SEM method=%u scenario=%u source=%zu json=%zu source_hit=%u json_hit=%u exit=%u allocated=%llu peak=%llu work=%llu physical0/0 JSON0/0 app0/0\n",
        options->method,options->scenario,metrics->source,metrics->json,metrics->source_hit?1u:0u,metrics->json_hit?1u:0u,stopped,
        (unsigned long long)metrics->stats.allocated_bytes,(unsigned long long)metrics->stats.peak_bytes,
        (unsigned long long)metrics->stats.work);
    if(options->scenario==2)return task_cancel_check(options->method);
    return 0;
}
static int semantic_main(int argc,char **argv) {
    CHECK(argc>=5);SemanticRun options={.uri=argv[1],.method=(unsigned)strtoul(argv[3],NULL,10),
        .source_fault=SIZE_MAX,.json_fault=SIZE_MAX,.report=true};
    CHECK(options.method>=11&&options.method<=13);
    SemanticMetrics metrics={0};
    if(!strcmp(argv[4],"--axes")){
        CHECK(argc==8);XrCompileResourceLimits baseline={(uint64_t)strtoull(argv[5],NULL,10),
            (uint64_t)strtoull(argv[6],NULL,10),(uint64_t)strtoull(argv[7],NULL,10)};
        CHECK(baseline.allocated_bytes<=UINT64_C(64)*1024*1024&&baseline.live_bytes<=UINT64_C(8)*1024*1024&&
            baseline.work<=UINT64_C(128000000));
        options.report=false;
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
            XrCompileResourceLimits limits=baseline;
            uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
            CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
            options.limits=&limits;options.budget_expected=delta<0;CHECK(!semantic_once(&options,&metrics));
        }
        fprintf(stderr,"SEM_AXES method=%u checked=9 budget=%u physical0/0 JSON0/0 app0/0\n",
            options.method,(unsigned)XR_XIR_BUDGET);return 0;
    }
    if(!strcmp(argv[4],"--resource")){
        CHECK(argc==7);options.source_fault=(size_t)strtoull(argv[5],NULL,10);options.json_fault=(size_t)strtoull(argv[6],NULL,10);
        CHECK(!semantic_once(&options,&metrics));return 0;
    }
    if(!strcmp(argv[4],"--chunk")){
        CHECK(argc==9);bool source=!strcmp(argv[5],"source");CHECK(source||!strcmp(argv[5],"json"));
        size_t start=(size_t)strtoull(argv[6],NULL,10),count=(size_t)strtoull(argv[7],NULL,10),total=(size_t)strtoull(argv[8],NULL,10);
        CHECK(count&&start<total&&count<=total-start);options.report=false;
        for(size_t i=0;i<count;++i){
            options.source_fault=source?start+i:SIZE_MAX;options.json_fault=source?SIZE_MAX:start+i;
            CHECK(!semantic_once(&options,&metrics));
        }
        fprintf(stderr,"SEM_CHUNK method=%u domain=%s start=%zu count=%zu total=%zu checked=%zu physical0/0 JSON0/0 app0/0\n",
            options.method,argv[5],start,count,total,count);return 0;
    }
    options.scenario=(unsigned)strtoul(argv[4],NULL,10);CHECK(options.scenario<=21&&options.scenario!=11);
    CHECK(argc==5||(argc==6&&options.scenario==18));
    if(options.scenario==18)options.json_fault=(size_t)strtoull(argv[5],NULL,10);
    return semantic_once(&options,&metrics);
}
#endif // LSP_SEMANTIC_TASK_CASES_H
