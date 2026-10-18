/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_child_write.c - Real DAP child writes preserve source and response ownership
 */
#define XR_DAP_ORIGIN_ENTRY reference_child_origin_main
#include "test_origin.c"
#undef XR_DAP_ORIGIN_ENTRY
static WriteFixture child_open(void) {
    WriteFixture fixture=write_open(XR_DAP_CHILD_WRITE_FIXTURE);char message[2048];
    snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":10}]}}",XR_DAP_CHILD_WRITE_FIXTURE);
    request(fixture.adapter,message);request(fixture.adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && fixture.adapter->snapshot && !fixture.adapter->running);capture.count=0;return fixture;
}
static size_t child_message(char *message,size_t capacity,uint32_t handle,const char *name,const char *value) {
    int bytes=snprintf(message,capacity,"{\"seq\":7,\"type\":\"request\",\"command\":\"setVariable\",\"arguments\":{\"variablesReference\":%u,\"name\":\"%s\",\"value\":\"%s\"}}",handle,name,value);
    CHECK(bytes>0 && (size_t)bytes<capacity);return (size_t)bytes;
}
static uint32_t child_set(XdapAdapter *adapter,uint32_t handle,const char *name,const char *value,bool success) {
    char message[512];child_message(message,sizeof(message),handle,name,value);request(adapter,message);
    CHECK(capture.count==1);XrJsonValue *reply=last_response("setVariable",success);uint32_t reference=0;
    if (success) reference=(uint32_t)xjson_get_int(xjson_get_object(reply,"body"),"variablesReference");
    xjson_free(reply);capture.count=0;return reference;
}
static uint32_t child_items(XdapAdapter *adapter) {
    uint32_t root=origin_child(adapter,adapter->frames[0].scope,"root");return origin_child(adapter,origin_child(adapter,root,"[0]"),"items");
}
enum { CHILD_TEXT_OLD,CHILD_TEXT_REPLACEMENT,CHILD_TEXT_NUL };
static void child_output(XdapAdapter *adapter,int64_t expected_count,uint32_t text_kind) {
    request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);
    CHECK(text_kind<=CHILD_TEXT_NUL);const char *texts[]={"old\n","replacement\n","owned\0new\n"};const size_t text_lengths[]={4,12,10};
    char first[32];int size=snprintf(first,sizeof(first),"%lld\n",(long long)expected_count);CHECK(size>0 && (size_t)size<sizeof(first));
    const char *expected[]={first,texts[text_kind],"11\n","11\n","37\n","23\n","23\n"};
    const size_t lengths[]={(size_t)size,text_lengths[text_kind],3,3,3,3,3};uint32_t found=0;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
        if (event && !strcmp(event,"output")) {
            XrJsonValue *text=xjson_get(xjson_get_object(message,"body"),"output");CHECK(found<7 && text->string_len==lengths[found]);
            CHECK(!memcmp(text->as.string,expected[found],lengths[found]));++found;
        }
        xjson_free(message);
    }
    CHECK(found==7);
}
static void child_normal(void) {
    WriteFixture fixture=child_open();XdapAdapter *adapter=fixture.adapter;uint32_t scope=adapter->frames[0].scope,frame_handle=adapter->frames[0].handle;
    uint32_t root=origin_child(adapter,scope,"root"),before=origin_child(adapter,scope,"before"),twin=origin_child(adapter,scope,"twin");
    const XdapValueRef *a=origin_ref(adapter,root),*b=origin_ref(adapter,twin),*c=origin_ref(adapter,before);
    CHECK(root!=before && root!=twin && before!=twin && a->value.payload==b->value.payload && b->value.payload==c->value.payload);
    CHECK(a->origin.operand!=b->origin.operand && c->origin.epoch==adapter->stop_epoch);
    CHECK(c->origin.operand!=a->origin.operand && c->origin.operand!=b->origin.operand);
    uint64_t epoch=adapter->stop_epoch;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;XrXirVmDebugFrameInfo original=write_frame(snapshot);
    child_set(adapter,before,"[0]","leaf",false);CHECK(adapter->stop_epoch==epoch && adapter->snapshot==snapshot);
    child_set(adapter,root,"[00]","root",false);child_set(adapter,root,"[-1]","root",false);child_set(adapter,root,"[1]","root",false);
    uint32_t parcel=origin_child(adapter,root,"[0]"),items=origin_child(adapter,parcel,"items"),piece=origin_child(adapter,items,"[0]");
    child_set(adapter,parcel,"sibling","99",false);
    child_set(adapter,piece,"stable","99",false);child_set(adapter,piece,"count","true",false);child_set(adapter,piece,"missing","99",false);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch);
    uint32_t new_items=child_set(adapter,parcel,"items","incoming",true);CHECK(new_items && adapter->stop_epoch==epoch+1 && adapter->value_count==1);
    CHECK(origin_ref(adapter,new_items)->origin.path.count==2 && origin_ref(adapter,new_items)->origin.epoch==epoch+1);
    child_set(adapter,items,"[0]","leaf",false);
    uint32_t new_piece=child_set(adapter,new_items,"[0]","leaf",true);CHECK(new_piece && adapter->stop_epoch==epoch+2 && adapter->value_count==1);
    CHECK(origin_ref(adapter,new_piece)->origin.path.count==3);
    CHECK(!child_set(adapter,new_piece,"count","99",true) && adapter->stop_epoch==epoch+3 && !adapter->value_count);
    piece=origin_child(adapter,child_items(adapter),"[0]");CHECK(!child_set(adapter,piece,"text","next",true) && adapter->stop_epoch==epoch+4);
    CHECK(adapter->frames[0].scope==scope && adapter->frames[0].handle==frame_handle);
    XrXirVmDebugFrameInfo same=write_frame(adapter->snapshot);CHECK(same.id==original.id && same.instruction==original.instruction && same.thread==original.thread);
    CHECK(!capture.count);child_output(adapter,99,CHILD_TEXT_NUL);write_dispose(&fixture);
}
static size_t child_fault(size_t fail) {
    WriteFixture fixture=child_open();XdapAdapter *adapter=fixture.adapter;uint32_t items=child_items(adapter);
    char message[512];size_t length=child_message(message,sizeof(message),items,"[0]","leaf");
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;uint32_t before_refs=adapter->value_count,next=adapter->next_handle;
    XrCompileResourceStats resources={0};CHECK(xr_compile_resources_stats(fixture.ledger,&resources)==XR_COMPILE_RESOURCE_OK);
    size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,length);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) {
        CHECK(status==XR_XIR_OK && adapter->stop_epoch==epoch+1 && adapter->value_count==1);
        CHECK(adapter->values[0].origin.path.count==3 && adapter->values[0].origin.epoch==epoch+1);
        XrJsonValue *reply=last_response("setVariable",true);xjson_free(reply);capture.count=0;
        CHECK(!child_set(adapter,adapter->values[0].handle,"count","99",true));
    } else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && adapter->stop_epoch==epoch && adapter->snapshot==snapshot);
        CHECK(runtime_live==blocks && runtime_bytes==bytes && adapter->value_count==before_refs && adapter->next_handle==next);
        XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==resources.live_bytes && after.work>=resources.work);
    }
    child_output(adapter,fail==SIZE_MAX ? 99 : 11,fail==SIZE_MAX ? CHILD_TEXT_REPLACEMENT : CHILD_TEXT_OLD);write_dispose(&fixture);return sites;
}
static XrXirStatus child_failed_output(void *context,const char *bytes,size_t length) {
    CHECK(context==&capture && bytes && length && !capture.count);return XR_XIR_IO;
}
static void child_io(void) {
    WriteFixture fixture=child_open();XdapAdapter *adapter=fixture.adapter;uint32_t items=child_items(adapter);
    char message[512];size_t length=child_message(message,sizeof(message),items,"[0]","leaf");uint64_t epoch=adapter->stop_epoch;
    XrXirVmDebugFrameInfo original=write_frame(adapter->snapshot);adapter->config.write=child_failed_output;
    CHECK(xdap_adapter_request(adapter,message,length)==XR_XIR_IO && !capture.count);
    CHECK(adapter->stop_epoch==epoch+1 && adapter->value_count==1 && adapter->values[0].origin.path.count==3);
    CHECK(adapter->values[0].origin.epoch==epoch+1);
    XrXirVmDebugFrameInfo same=write_frame(adapter->snapshot);CHECK(same.id==original.id && same.instruction==original.instruction);
    adapter->config.write=write_write;child_output(adapter,23,CHILD_TEXT_REPLACEMENT);write_dispose(&fixture);
}
#ifndef XR_DAP_CHILD_ENTRY
#define XR_DAP_CHILD_ENTRY main
#endif
int XR_DAP_CHILD_ENTRY(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    child_normal();child_io();size_t sites=child_fault(SIZE_MAX);CHECK(sites);
    for (size_t i=0;i<sites;++i) if (resources || !i || i==sites-1) child_fault(i);
    printf("DAP child Array/Struct writes, equal-source isolation, owned aggregate responses, NUL/retirement/PC/readonly/FI; sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
