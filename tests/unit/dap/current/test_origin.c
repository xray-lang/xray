/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_origin.c - Equal observed values retain distinct Source Cell selectors
 */
#define XR_DAP_WRITE_ENTRY reference_origin_write_main
#include "test_write.c"
#undef XR_DAP_WRITE_ENTRY
static WriteFixture origin_open(void) {
    WriteFixture fixture=write_open(XR_DAP_ORIGIN_FIXTURE);char message[2048];
    snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":6}]}}",XR_DAP_ORIGIN_FIXTURE);
    request(fixture.adapter,message);request(fixture.adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && fixture.adapter->snapshot && !fixture.adapter->running);capture.count=0;return fixture;
}
static size_t origin_message(char *message,size_t capacity,uint32_t handle) {
    int bytes=snprintf(message,capacity,"{\"seq\":5,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%u}}",handle);
    CHECK(bytes>0 && (size_t)bytes<capacity);return (size_t)bytes;
}
static uint32_t origin_child(XdapAdapter *adapter,uint32_t parent,const char *name) {
    char message[256];origin_message(message,sizeof(message),parent);request(adapter,message);
    XrJsonValue *reply=last_response("variables",true),*values=xjson_get_array(xjson_get_object(reply,"body"),"variables");uint32_t handle=0;
    for (int i=0;i<xjson_array_len(values);++i) {
        XrJsonValue *value=xjson_array_get(values,i);
        if (!strcmp(xjson_get_string(value,"name"),name)) handle=(uint32_t)xjson_get_int(value,"variablesReference");
    }
    xjson_free(reply);capture.count=0;CHECK(handle);return handle;
}
static const XdapValueRef *origin_ref(XdapAdapter *adapter,uint32_t handle) {
    for (uint32_t i=0;i<adapter->value_count;++i) if (adapter->values[i].handle==handle) return &adapter->values[i];
    CHECK(false);return NULL;
}
static void origin_normal(void) {
    WriteFixture fixture=origin_open();XdapAdapter *adapter=fixture.adapter;uint32_t scope=adapter->frames[0].scope;
    uint32_t root=origin_child(adapter,scope,"root"),before=origin_child(adapter,scope,"before");CHECK(root!=before);
    const XdapValueRef *writable=origin_ref(adapter,root),*readonly=origin_ref(adapter,before);
    CHECK(writable->value.type==readonly->value.type && writable->value.payload==readonly->value.payload);
    CHECK(writable->origin.epoch==adapter->stop_epoch && writable->origin.frame==write_frame(adapter->snapshot).id);
    CHECK(!writable->origin.path.count && !writable->origin.path.steps && readonly->origin.epoch==adapter->stop_epoch);
    CHECK(readonly->origin.operand!=writable->origin.operand);
    CHECK(origin_child(adapter,scope,"root")==root && origin_child(adapter,scope,"before")==before && adapter->value_count==2);
    request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"root\"}}");
    XrJsonValue *reply=last_response("evaluate",true);uint32_t pure=(uint32_t)xjson_get_int(xjson_get_object(reply,"body"),"variablesReference");
    CHECK(pure && pure!=before && pure!=root && !origin_ref(adapter,pure)->origin.epoch);
    CHECK(origin_ref(adapter,pure)->value.payload==writable->value.payload && adapter->value_count==3);
    xjson_free(reply);capture.count=0;
    uint32_t parcel=origin_child(adapter,root,"[0]"),items=origin_child(adapter,parcel,"items"),piece=origin_child(adapter,items,"[0]");
    const XdapValueRef *refs[]={origin_ref(adapter,parcel),origin_ref(adapter,items),origin_ref(adapter,piece)};
    for (uint32_t i=0;i<3;++i) {
        CHECK(refs[i]->origin.epoch==adapter->stop_epoch && refs[i]->origin.frame==writable->origin.frame && refs[i]->origin.operand==writable->origin.operand);
        CHECK(refs[i]->origin.path.count==i+1 && refs[i]->origin.path.steps && refs[i]->origin.path.steps!=writable->origin.path.steps);
        if (i) CHECK(refs[i]->origin.path.steps!=refs[i-1]->origin.path.steps);
    }
    CHECK(refs[2]->origin.path.steps[0].kind==XR_XIR_PATH_INDEX && (uint32_t)refs[2]->origin.path.steps[0].container==writable->value.type);
    CHECK(refs[2]->origin.path.steps[1].kind==XR_XIR_PATH_FIELD && refs[2]->origin.path.steps[1].selector==0);
    CHECK(refs[2]->origin.path.steps[2].kind==XR_XIR_PATH_INDEX && refs[2]->origin.path.steps[2].selector==0);
    uint32_t old_parcel=origin_child(adapter,before,"[0]"),old_items=origin_child(adapter,old_parcel,"items");
    CHECK(origin_ref(adapter,old_parcel)->origin.operand==readonly->origin.operand && origin_ref(adapter,old_parcel)->origin.path.count==1);
    CHECK(origin_ref(adapter,old_items)->origin.operand==readonly->origin.operand && origin_ref(adapter,old_items)->origin.path.count==2);
    write_dispose(&fixture);
}
static size_t origin_fault(size_t fail) {
    WriteFixture fixture=origin_open();XdapAdapter *adapter=fixture.adapter;uint32_t root=origin_child(adapter,adapter->frames[0].scope,"root");
    char message[256];size_t length=origin_message(message,sizeof(message),root);uint32_t before_refs=adapter->value_count;
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,length);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) {
        CHECK(status==XR_XIR_OK && adapter->value_count==before_refs+1);
        CHECK(adapter->values[before_refs].origin.path.count==1 && adapter->values[before_refs].origin.path.steps);
    } else {
        if (status!=XR_XIR_OUT_OF_MEMORY || sites<=fail || adapter->value_count!=before_refs)
            fprintf(stderr,"Origin fault site=%zu/%zu status=%d refs=%u/%u\n",fail,sites,(int)status,adapter->value_count,before_refs);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && adapter->value_count==before_refs);
        CHECK(runtime_live==blocks && runtime_bytes==bytes);XrCompileResourceStats after={0};
        CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==before.live_bytes && after.work>=before.work);
        request(adapter,message);CHECK(adapter->value_count==before_refs+1);
    }
    CHECK(snapshot==adapter->snapshot && epoch==adapter->stop_epoch);
    XrJsonValue *reply=last_response("variables",true);xjson_free(reply);write_dispose(&fixture);return sites;
}
#ifndef XR_DAP_ORIGIN_ENTRY
#define XR_DAP_ORIGIN_ENTRY main
#endif
int XR_DAP_ORIGIN_ENTRY(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    origin_normal();size_t sites=origin_fault(SIZE_MAX);CHECK(sites);
    for (size_t i=0;i<sites;++i) if (resources || !i || i==sites-1) origin_fault(i);
    printf("Source Cell origins isolate equal const/evaluate values, own nested paths, preserve failed queries; sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
