/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_watches.c - Watch text ownership drives actual current Source frames
 */
#define XDAP_EVALUATE_MAIN reference_watches_evaluate_main
#include "test_evaluate.c"
#undef XDAP_EVALUATE_MAIN
#include "app/dap/xdap_watch.h"
#include "frontend/parser/xparse.h"
static uint32_t watches_count(XdapAdapter *adapter) {
    uint32_t count=99;CHECK(xdap_watch_count(adapter,&count)==XR_XIR_OK);return count;
}
static void watches_value(XdapAdapter *adapter,uint32_t id,uint32_t frame,int64_t expected) {
    XrXirValue value={0};CHECK(xdap_watch_evaluate(adapter,id,frame,&value)==XR_XIR_OK);
    CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==expected);xr_xir_value_drop(&value);
}
static void watches_resume(XdapAdapter *adapter) {
    uint64_t thread=xr_xir_vm_debug_snapshot_stopped_thread(adapter->snapshot);char message[256];
    snprintf(message,sizeof(message),"{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":%llu}}",
        (unsigned long long)thread);capture.count=0;request(adapter,message);
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && event_count("output")==1);
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *json=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(json);
        const char *event=xjson_get_string(json,"event");
        if (event && !strcmp(event,"output")) CHECK(!strcmp(xjson_get_string(xjson_get_object(json,"body"),"output"),"44\n"));
        xjson_free(json);
    }
    capture.count=0;
}
static void watches_normal(void) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    CHECK(!watches_count(adapter));uint32_t ids[5]={0};char owned[]="doubled + 2";
    CHECK(xdap_watch_add(adapter,owned,sizeof(owned)-1,&ids[0])==XR_XIR_OK);memset(owned,'x',sizeof(owned)-1);
    const char *texts[]={"input + value","label","doubled + 2","print(value)"};
    for (uint32_t i=0;i<4;++i) CHECK(xdap_watch_add(adapter,texts[i],strlen(texts[i]),&ids[i+1])==XR_XIR_OK && ids[i+1]>ids[i]);
    CHECK(watches_count(adapter)==5);XdapWatchText first={0},escaped={0};
    CHECK(xdap_watch_at(adapter,0,&first)==XR_XIR_OK && first.id==ids[0] && first.length==11 && !memcmp(first.bytes,"doubled + 2",12));
    first.bytes[0]='x';xdap_watch_text_drop(&first);
    CHECK(xdap_watch_at(adapter,0,&first)==XR_XIR_OK && !strcmp(first.bytes,"doubled + 2"));xdap_watch_text_drop(&first);
    CHECK(xdap_watch_at(adapter,2,&escaped)==XR_XIR_OK && escaped.id==ids[2] && !strcmp(escaped.bytes,"label"));
    evaluate_start(adapter);uint32_t inner=adapter->frames[0].handle,outer=adapter->frames[1].handle;
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    watches_value(adapter,ids[0],inner,42);watches_value(adapter,ids[1],outer,39);watches_value(adapter,ids[3],inner,42);
    XrXirValue rejected={0};CHECK(xdap_watch_evaluate(adapter,ids[0],outer,&rejected)!=XR_XIR_OK && !rejected.type && !rejected.payload);
    CHECK(xdap_watch_evaluate(adapter,ids[4],inner,&rejected)==XR_XIR_BAD_TYPE && !rejected.type && !rejected.payload);
    XrXirValue label={0};CHECK(xdap_watch_evaluate(adapter,ids[2],inner,&label)==XR_XIR_OK);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&label,&bytes,&length) && length==5 && !memcmp(bytes,"inner",5));
    evaluate_context(adapter,"doubled + 2",inner,"42","\"watch\"");CHECK(watches_count(adapter)==5);
    evaluate_context(adapter,"value",inner,"20","\"watch\"");CHECK(watches_count(adapter)==6);
    XdapWatchText added={0};CHECK(xdap_watch_at(adapter,5,&added)==XR_XIR_OK && !strcmp(added.bytes,"value"));
    uint32_t last=added.id;xdap_watch_text_drop(&added);watches_value(adapter,last,inner,20);
    evaluate_context(adapter,"value",inner,"20","\"watch\"");CHECK(watches_count(adapter)==6);
    evaluate_context(adapter,"doubled",inner,"40","\"hover\"");CHECK(watches_count(adapter)==6);
    evaluate_context(adapter,"unknown_watch",inner,NULL,"\"watch\"");CHECK(watches_count(adapter)==6);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running && !capture.count);
    watches_resume(adapter);CHECK(watches_count(adapter)==6);
    CHECK(xdap_watch_evaluate(adapter,ids[0],inner,&rejected)==XR_XIR_BAD_STRUCTURE && !rejected.type && !rejected.payload);
    request(adapter,"{\"seq\":9,\"type\":\"request\",\"command\":\"restart\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && watches_count(adapter)==6);
    uint32_t fresh=adapter->frames[0].handle;CHECK(fresh!=inner);
    CHECK(xdap_watch_evaluate(adapter,ids[0],inner,&rejected)==XR_XIR_BAD_STRUCTURE);
    watches_value(adapter,ids[0],fresh,42);
    CHECK(xdap_watch_remove(adapter,ids[0])==XR_XIR_OK && watches_count(adapter)==5);
    CHECK(xdap_watch_evaluate(adapter,ids[0],fresh,&rejected)==XR_XIR_UNRESOLVED);
    watches_value(adapter,ids[3],fresh,42);CHECK(xdap_watch_clear(adapter)==XR_XIR_OK && !watches_count(adapter));
    uint32_t replacement=0;CHECK(xdap_watch_add(adapter,"value",5,&replacement)==XR_XIR_OK && replacement>last);
    watches_value(adapter,replacement,fresh,20);watches_resume(adapter);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    CHECK(escaped.id==ids[2] && escaped.length==5 && !memcmp(escaped.bytes,"label",6));
    CHECK(xr_xir_string_view(&label,&bytes,&length) && length==5 && !memcmp(bytes,"inner",5));
    xr_xir_value_drop(&label);xdap_watch_text_drop(&escaped);evaluate_finish(&fixture);
    puts("Original watch add/multiple/owned-text obligations: unique IDs, caller42/39, real Source44 twice, restart/stale-frame refusal and escaped owners PASS");
}
static void watches_unicode(void) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;evaluate_start(adapter);
    const char text[]="\"\xe7\x95\x8c\\0a\"";uint32_t id=0;
    CHECK(xdap_watch_add(adapter,text,sizeof(text)-1,&id)==XR_XIR_OK);
    XrXirValue value={0};CHECK(xdap_watch_evaluate(adapter,id,adapter->frames[0].handle,&value)==XR_XIR_OK);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&value,&bytes,&length));
    CHECK(length==5 && !memcmp(bytes,"\xe7\x95\x8c\0a",5));xr_xir_value_drop(&value);
    XdapWatchText copy={0};CHECK(xdap_watch_at(adapter,0,&copy)==XR_XIR_OK);
    CHECK(copy.length==sizeof(text)-1 && !memcmp(copy.bytes,text,sizeof(text)));xdap_watch_text_drop(&copy);
    watches_resume(adapter);evaluate_dispose(&fixture);
    puts("Watch source text preserves UTF8 and escaped NUL; actual readonly Source result preserves all5 bytes/final44 PASS");
}
static XrXirStatus watches_failed_write(void *context,const char *bytes,size_t length) {
    XdapAdapter *adapter=context;uint32_t count=77,id=99;XdapWatchText text={0};XrXirValue value={0};
    (void)bytes;(void)length;
    CHECK(xdap_watch_count(adapter,&count)==XR_XIR_BAD_STRUCTURE && count==77);
    CHECK(xdap_watch_add(adapter,"value",5,&id)==XR_XIR_BAD_STRUCTURE && id==99);
    CHECK(xdap_watch_at(adapter,0,&text)==XR_XIR_BAD_STRUCTURE && !text.bytes);
    CHECK(xdap_watch_remove(adapter,1)==XR_XIR_BAD_STRUCTURE && xdap_watch_clear(adapter)==XR_XIR_BAD_STRUCTURE);
    CHECK(xdap_watch_evaluate(adapter,1,1,&value)==XR_XIR_BAD_STRUCTURE && !value.type && !value.payload);
    return XR_XIR_IO;
}
static void watches_io(void) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;evaluate_start(adapter);
    uint32_t frame=adapter->frames[0].handle;char message[256];snprintf(message,sizeof(message),
        "{\"seq\":5,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"context\":\"watch\",\"expression\":\"doubled\",\"frameId\":%u}}",frame);
    adapter->config.write=watches_failed_write;adapter->config.context=adapter;
    CHECK(xdap_adapter_request(adapter,message,strlen(message))==XR_XIR_IO && !watches_count(adapter) && adapter->next_watch_id==1);
    adapter->config.write=evaluate_write;adapter->config.context=&capture;
    CHECK(!capture.count && adapter->snapshot && !adapter->running);request(adapter,message);CHECK(watches_count(adapter)==1);
    watches_value(adapter,1,frame,40);watches_resume(adapter);evaluate_dispose(&fixture);
    puts("Watch response I/O rollback and public API callback reentry refusal; same-ledger retry/final44 PASS");
}
static size_t watches_fault(uint32_t mode,size_t fail) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;evaluate_start(adapter);
    uint32_t id=0,count=0;
    if (mode==1 || mode==2) CHECK(xdap_watch_add(adapter,"label",5,&id)==XR_XIR_OK);
    CHECK(xdap_watch_count(adapter,&count)==XR_XIR_OK);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    uint32_t next=adapter->next_watch_id,seq=adapter->seq;XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    XdapWatchText text={0};XrXirValue value={0};char message[256];snprintf(message,sizeof(message),
        "{\"seq\":5,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"context\":\"watch\",\"expression\":\"doubled\",\"frameId\":%u}}",
        adapter->frames[0].handle);
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;XrXirStatus status;
    if (!mode) { id=77;status=xdap_watch_add(adapter,"doubled",7,&id); }
    else if (mode==1) status=xdap_watch_at(adapter,0,&text);
    else if (mode==2) status=xdap_watch_evaluate(adapter,id,adapter->frames[0].handle,&value);
    else status=xdap_adapter_request(adapter,message,strlen(message));
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail!=SIZE_MAX) {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && watches_count(adapter)==count);
        CHECK(xr_compile_state_status(xr_source_compiler_state(adapter->compiler))==XR_COMPILE_RESOURCE_OK);
        CHECK(adapter->next_watch_id==next && adapter->seq==seq && !capture.count && !text.bytes && !value.type && !value.payload);
        if (!mode) CHECK(id==77);
        CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes && after.work>before.work && after.allocated_bytes>=before.allocated_bytes);
        if (!mode) CHECK(xdap_watch_add(adapter,"doubled",7,&id)==XR_XIR_OK);
        else if (mode==1) CHECK(xdap_watch_at(adapter,0,&text)==XR_XIR_OK);
        else if (mode==2) {
            XrXirStatus retried=xdap_watch_evaluate(adapter,id,adapter->frames[0].handle,&value);
            if (retried!=XR_XIR_OK) fprintf(stderr,"Watch retry mode=%u fail=%zu sites=%zu status=%d id=%u frame=%u\n",mode,fail,sites,(int)retried,id,adapter->frames[0].handle);
            CHECK(retried==XR_XIR_OK);
        }
        else request(adapter,message);
    } else CHECK(status==XR_XIR_OK);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running);
    if (mode==3) { CHECK(watches_count(adapter)==1 && adapter->watches[0].id==1);watches_value(adapter,1,adapter->frames[0].handle,40); }
    xdap_watch_text_drop(&text);xr_xir_value_drop(&value);capture.count=0;watches_resume(adapter);evaluate_dispose(&fixture);return sites;
}
static void watches_compiler_failure(void) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;evaluate_start(adapter);
    uint32_t id=0;CHECK(xdap_watch_add(adapter,"label",5,&id)==XR_XIR_OK);
    XrCompileState *state=xr_source_compiler_state(adapter->compiler);
    AstNode *program=NULL;size_t begin=runtime_attempts;runtime_fail_at=begin;
    XrParseStatus parsed=xr_compile_parse_expression_string(xr_source_compiler_session(adapter->compiler),
        "label","<shared-parser-control>",&program);runtime_fail_at=SIZE_MAX;
    CHECK(parsed==XR_PARSE_OUT_OF_MEMORY && runtime_attempts>begin && !program);
    CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    CHECK(xr_compile_parse_expression_string(xr_source_compiler_session(adapter->compiler),
        "label","<shared-parser-control>",&program)==XR_PARSE_OUT_OF_MEMORY && !program);
    XrXirValue value={0};CHECK(xdap_watch_evaluate(adapter,id,adapter->frames[0].handle,&value)==XR_XIR_OUT_OF_MEMORY);
    CHECK(!value.type && !value.payload && watches_count(adapter)==1);
    CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    watches_resume(adapter);evaluate_dispose(&fixture);
    puts("A pre-existing sticky SourceCompiler failure remains authoritative; no new allowance/reset; original target44 still completes PASS");
}
static void watches_limits(void) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;uint32_t id=77;
    CHECK(xdap_watch_add(adapter,NULL,1,&id)==XR_XIR_BAD_TYPE && id==77);
    CHECK(xdap_watch_add(adapter,"",0,&id)==XR_XIR_BAD_TYPE && id==77);
    CHECK(xdap_watch_add(adapter,"x\0y",3,&id)==XR_XIR_BAD_TYPE && id==77);
    for (uint32_t i=0;i<128;++i) {
        char text[32];snprintf(text,sizeof(text),"value + %u",i);
        CHECK(xdap_watch_add(adapter,text,strlen(text),&id)==XR_XIR_OK && id==i+1);
    }
    CHECK(watches_count(adapter)==128 && xdap_watch_add(adapter,"overflow",8,&id)==XR_XIR_BUDGET && id==128);
    CHECK(xdap_watch_remove(adapter,65)==XR_XIR_OK);
    CHECK(xdap_watch_add(adapter,"next",4,&id)==XR_XIR_OK && id==129);
    CHECK(xdap_watch_clear(adapter)==XR_XIR_OK && xdap_watch_add(adapter,"after",5,&id)==XR_XIR_OK && id==130);
    CHECK(xdap_watch_remove(adapter,1)==XR_XIR_UNRESOLVED);evaluate_dispose(&fixture);
    fixture=evaluate_open(XR_DAP_CALL_FIXTURE);adapter=fixture.adapter;char large[16384];memset(large,'x',sizeof(large));
    CHECK(xdap_watch_add(adapter,large,sizeof(large)-1,&id)==XR_XIR_OK && adapter->watch_bytes==sizeof(large));
    CHECK(xdap_watch_add(adapter,"next",4,&id)==XR_XIR_BUDGET && watches_count(adapter)==1);
    CHECK(xdap_watch_clear(adapter)==XR_XIR_OK);
    CHECK(xdap_watch_add(adapter,large,sizeof(large),&id)==XR_XIR_BUDGET);evaluate_dispose(&fixture);
    fixture=evaluate_open(XR_DAP_CALL_FIXTURE);adapter=fixture.adapter;adapter->next_watch_id=INT32_MAX;
    CHECK(xdap_watch_add(adapter,"last",4,&id)==XR_XIR_OK && id==INT32_MAX);
    CHECK(xdap_watch_clear(adapter)==XR_XIR_OK && xdap_watch_add(adapter,"no_wrap",7,&id)==XR_XIR_BUDGET);
    evaluate_dispose(&fixture);
    fixture=evaluate_open(XR_DAP_CALL_FIXTURE);adapter=fixture.adapter;
    CHECK(xdap_watch_add(adapter,"value",5,&id)==XR_XIR_OK);XrCompileResourceStats spent={0};
    CHECK(xr_compile_resources_stats(fixture.ledger,&spent)==XR_COMPILE_RESOURCE_OK && spent.work<128000000);
    CHECK(xr_compile_resources_work(fixture.ledger,128000000-spent.work)==XR_COMPILE_RESOURCE_OK);
    XdapWatchText copy={0};CHECK(xdap_watch_at(adapter,0,&copy)==XR_XIR_BUDGET && !copy.bytes);
    CHECK(xdap_watch_remove(adapter,id)==XR_XIR_BUDGET && xdap_watch_clear(adapter)==XR_XIR_BUDGET && watches_count(adapter)==1);
    evaluate_dispose(&fixture);
    puts("Watch count128/aggregate bytes/exhausted IDs/NUL/work refusal preserve owners; unmetered destructor cleanup physical0 PASS");
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    watches_normal();watches_unicode();watches_io();watches_limits();watches_compiler_failure();
    for (uint32_t mode=0;mode<4;++mode) {
        size_t sites=watches_fault(mode,SIZE_MAX);CHECK(sites);
        if (resources) for (size_t i=0;i<sites;++i) watches_fault(mode,i);
        else { watches_fault(mode,0);if (sites>1) watches_fault(mode,sites-1); }
        printf("Current watch mode=%u allocation sites=%zu injection=%s; old frame/ID/text/sequence and physical0 retained PASS\n",
            mode,sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
    return 0;
}
