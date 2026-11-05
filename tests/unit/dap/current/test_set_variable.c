/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_set_variable.c - Real DAP mutation and precommit failure ownership
 */
#define main reference_set_dispatch_main
#include "test_dispatch.c"
#undef main
#include "xir/xxir_vm_debug_internal.h"
#include "app/dap/xdap_dispatch_internal.h"
typedef struct SetFixture { XdapAdapter *adapter;XrCompileResources *ledger;XrXirDomain *domain;XrCompileResourceStats baseline; } SetFixture;
static XrXirStatus set_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;
    message->size=length;XrJsonValue *json=xjson_parse(bytes,length);CHECK(json && xjson_get_int(json,"seq")==++out->seq);
    xjson_free(json);return XR_XIR_OK;
}
static SetFixture set_open(const char *file) {
    SetFixture fixture={0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture.ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture.ledger,&fixture.baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture.ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture.domain,.write=set_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);
    request(fixture.adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"supportsVariableType\":true}}");
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    request(fixture.adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);capture.count=0;return fixture;
}
static void set_finish(SetFixture *fixture) {
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void set_dispose(SetFixture *fixture) { CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);set_finish(fixture); }
#include "app/dap/xdap_lexical.h"
#include "xir/xxir_array.h"
static void set_pause(SetFixture *fixture,const char *file,uint32_t line) {
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%u}]}}",file,line);
    request(fixture->adapter,message);request(fixture->adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(fixture->adapter,1000)==XR_XIR_OK && fixture->adapter->snapshot && !fixture->adapter->running);capture.count=0;
}
static size_t set_message(char *message,size_t capacity,uint32_t scope,const char *name,const char *expression) {
    int size=snprintf(message,capacity,"{\"seq\":5,\"type\":\"request\",\"command\":\"setVariable\",\"arguments\":{\"variablesReference\":%u,\"name\":\"%s\",\"value\":\"%s\"}}",scope,name,expression);
    CHECK(size>0 && (size_t)size<capacity);return (size_t)size;
}
static XrJsonValue *set_request(XdapAdapter *adapter,uint32_t scope,const char *name,const char *expression,bool success) {
    char message[1024];set_message(message,sizeof(message),scope,name,expression);request(adapter,message);
    CHECK(!event_count("stopped") && !event_count("output"));return last_response("setVariable",success);
}
static XrXirValue set_owned(XdapAdapter *adapter,const XrXirVmDebugSnapshot *snapshot,const char *name) {
    XdapLexicalView lexical={0};CHECK(xdap_lexical_open(adapter->launch,snapshot,0,&lexical)==XR_XIR_OK);
    for (uint32_t b=lexical.environment;b;b=lexical.function->bindings[b-1].parent) {
        const XrXirSourceDebugBinding *binding=&lexical.function->bindings[b-1];
        if (strcmp(lexical.source->declarations[binding->declaration-1].name,name)) continue;
        XrXirValue value={0};const char *text=NULL;size_t length=0;
        CHECK(xdap_lexical_read(&lexical,b,&value,&text,&length)==XR_XIR_VALUE_OK);return value;
    }
    CHECK(false);return (XrXirValue){0};
}
static void set_scalar(XdapAdapter *adapter,const char *name,int64_t expected) {
    XrXirValue value=set_owned(adapter,adapter->snapshot,name);CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==expected);xr_xir_value_drop(&value);
}
static uint32_t set_evaluate(XdapAdapter *adapter,const char *name) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":6,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"%s\"}}",name);
    request(adapter,message);XrJsonValue *reply=last_response("evaluate",true);
    uint32_t reference=(uint32_t)xjson_get_int(xjson_get_object(reply,"body"),"variablesReference");xjson_free(reply);capture.count=0;return reference;
}
static void set_resume(XdapAdapter *adapter) {
    request(adapter,"{\"seq\":7,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);
}
static void set_output(uint32_t index,const char *text,size_t length) {
    uint32_t found=0;bool seen=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
        if (event && !strcmp(event,"output") && found++==index) {
            XrJsonValue *output=xjson_get(xjson_get_object(message,"body"),"output");
            CHECK(output && output->string_len==length && !memcmp(output->as.string,text,length));seen=true;
        }
        xjson_free(message);
    }
    CHECK(seen);
}
static void set_normal(void) {
    SetFixture fixture=set_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;set_pause(&fixture,XR_DAP_CALL_FIXTURE,4);
    CHECK(adapter->variable_types);uint32_t scope=adapter->frames[0].scope,handle=adapter->frames[0].handle,next=adapter->next_handle;
    XrXirVmDebugSnapshot *old=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&old)==XR_XIR_VALUE_OK);uint64_t epoch=adapter->stop_epoch;
    const char *names[]={"label","missing","doubled","doubled","doubled","doubled"};
    const char *rejected[]={"label","1","true","doubled = 9","print(9)","1; print(9)"};
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        XrJsonValue *reply=set_request(adapter,scope,names[i],rejected[i],false);xjson_free(reply);capture.count=0;
        CHECK(adapter->stop_epoch==epoch && adapter->snapshot==old && adapter->next_handle==next);set_scalar(adapter,"doubled",40);
    }
    XrJsonValue *reply=set_request(adapter,UINT32_MAX,"doubled","1",false);xjson_free(reply);capture.count=0;
    reply=set_request(adapter,scope,"doubled","doubled + 60",true);XrJsonValue *body=xjson_get_object(reply,"body");
    CHECK(!strcmp(xjson_get_string(body,"value"),"100") && !strcmp(xjson_get_string(body,"type"),"i64") && !xjson_get_int(body,"variablesReference"));xjson_free(reply);
    CHECK(adapter->stop_epoch==epoch+1 && adapter->snapshot!=old && adapter->frames[0].scope==scope && adapter->frames[0].handle==handle);
    set_scalar(adapter,"doubled",100);XrXirValue prior=set_owned(adapter,old,"doubled");CHECK(prior.payload==40);xr_xir_value_drop(&prior);
    set_resume(adapter);set_output(0,"104\n",4);capture.count=0;
    reply=set_request(adapter,scope,"doubled","1",false);xjson_free(reply);capture.count=0;
    request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"restart\"}");CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot);capture.count=0;
    set_scalar(adapter,"doubled",40);set_resume(adapter);set_output(0,"44\n",3);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);prior=(XrXirValue){0};
    CHECK(xr_xir_vm_debug_snapshot_depth(old)==3);xr_xir_vm_debug_snapshot_drop(old);set_finish(&fixture);
}
static void set_intrinsic(void) {
    SetFixture fixture=set_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;set_pause(&fixture,XR_DAP_CALL_FIXTURE,4);
    XrXirVmDebugSnapshot *old=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&old)==XR_XIR_VALUE_OK);
    uint64_t epoch=adapter->stop_epoch;uint32_t scope=adapter->frames[0].scope,frame=adapter->frames[0].handle;
    XrJsonValue *reply=set_request(adapter,scope,"doubled","len(label)",true);
    CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"value"),"5"));xjson_free(reply);capture.count=0;
    CHECK(adapter->stop_epoch==epoch+1 && adapter->frames[0].scope==scope && adapter->frames[0].handle==frame);
    set_scalar(adapter,"doubled",5);XrXirValue prior=set_owned(adapter,old,"doubled");CHECK(prior.payload==40);xr_xir_value_drop(&prior);
    set_resume(adapter);set_output(0,"9\n",2);CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    CHECK(xr_xir_vm_debug_snapshot_depth(old)==3);xr_xir_vm_debug_snapshot_drop(old);set_finish(&fixture);
}
static void set_array_page(XdapAdapter *adapter,uint32_t reference,bool success,int64_t expected) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":6,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%u}}",reference);
    request(adapter,message);XrJsonValue *reply=last_response("variables",success);
    if (success) { XrJsonValue *values=xjson_get_array(xjson_get_object(reply,"body"),"variables");CHECK(xjson_array_len(values)==3);
        char number[32];snprintf(number,sizeof(number),"%lld",(long long)expected);CHECK(!strcmp(xjson_get_string(xjson_array_get(values,1),"value"),number)); }
    xjson_free(reply);capture.count=0;
}
static void set_managed(void) {
    SetFixture fixture=set_open(XR_DAP_ARRAY_FIXTURE);XdapAdapter *adapter=fixture.adapter;set_pause(&fixture,XR_DAP_ARRAY_FIXTURE,6);
    uint32_t original=set_evaluate(adapter,"values"),replacement=set_evaluate(adapter,"snapshot");CHECK(original && replacement && original!=replacement);
    XrXirValue owned=set_owned(adapter,adapter->snapshot,"values");set_array_page(adapter,original,true,42);
    XrJsonValue *reply=set_request(adapter,adapter->frames[0].scope,"values","snapshot",true);
    uint32_t written=(uint32_t)xjson_get_int(xjson_get_object(reply,"body"),"variablesReference");CHECK(written && written!=replacement);xjson_free(reply);capture.count=0;
    CHECK(adapter->value_count==1 && adapter->values[0].origin.epoch==adapter->stop_epoch);
    set_array_page(adapter,original,false,0);set_array_page(adapter,replacement,false,0);set_array_page(adapter,written,true,20);
    set_resume(adapter);CHECK(event_count("output")==3);set_output(0,"20\n",3);set_output(1,"20\n",3);set_output(2,"owned\0value\n",12);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);XrXirValue element={0};
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(&owned),.domain=fixture.domain,.work=65536,.scratch_bytes=65536};
    XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(&owned,1,&admission,&element,&fault)==XR_XIR_VALUE_OK && element.payload==42);
    xr_xir_value_drop(&element);xr_xir_value_drop(&owned);set_finish(&fixture);
    fixture=set_open(XR_DAP_STRING_WRITE_FIXTURE);adapter=fixture.adapter;set_pause(&fixture,XR_DAP_STRING_WRITE_FIXTURE,4);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;char message[512];
    size_t length=set_message(message,sizeof(message),adapter->frames[0].scope,"text","large");
    adapter->config.message_limit=256;CHECK(length<256 && xdap_adapter_request(adapter,message,length)==XR_XIR_BUDGET && !capture.count);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch);XrXirValue original_text=set_owned(adapter,snapshot,"text");
    const char *bytes=NULL;size_t count=0;CHECK(xr_xir_string_view(&original_text,&bytes,&count) && count==3 && !memcmp(bytes,"old",3));
    xr_xir_value_drop(&original_text);adapter->config.message_limit=16384;
    reply=set_request(adapter,adapter->frames[0].scope,"text","next",true);XrJsonValue *text=xjson_get(xjson_get_object(reply,"body"),"value");
    CHECK(text && text->string_len==9 && !memcmp(text->as.string,"owned\0new",9));xjson_free(reply);capture.count=0;
    set_resume(adapter);set_output(0,"owned\0new\n",10);set_dispose(&fixture);
}
static XrXirStatus set_failed_output(void *context,const char *bytes,size_t length) {
    CHECK(context==&capture && bytes && length && !capture.count);return XR_XIR_IO;
}
static void set_caller_and_io(void) {
    SetFixture fixture=set_open(XR_DAP_CALLER_WRITE_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    set_pause(&fixture,XR_DAP_CALLER_WRITE_FIXTURE,3);CHECK(adapter->depth==3);
    XrJsonValue *reply=set_request(adapter,adapter->frames[1].scope,"suffix","10",true);xjson_free(reply);capture.count=0;
    set_resume(adapter);set_output(0,"11\n",3);set_dispose(&fixture);
    fixture=set_open(XR_DAP_CALL_FIXTURE);adapter=fixture.adapter;set_pause(&fixture,XR_DAP_CALL_FIXTURE,4);
    char message[512];size_t length=set_message(message,sizeof(message),adapter->frames[0].scope,"doubled","100");uint64_t epoch=adapter->stop_epoch;
    adapter->config.write=set_failed_output;CHECK(xdap_adapter_request(adapter,message,length)==XR_XIR_IO && !capture.count);
    adapter->config.write=set_write;CHECK(adapter->stop_epoch==epoch+1 && !adapter->running);set_scalar(adapter,"doubled",100);
    set_resume(adapter);set_output(0,"104\n",4);set_dispose(&fixture);
}
static size_t set_fault(size_t fail) {
    SetFixture fixture=set_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;set_pause(&fixture,XR_DAP_CALL_FIXTURE,4);
    char message[512];size_t length=set_message(message,sizeof(message),adapter->frames[0].scope,"doubled","doubled + 60");
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;uint32_t next=adapter->next_handle;
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,length);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) { CHECK(status==XR_XIR_OK && adapter->stop_epoch==epoch+1);XrJsonValue *reply=last_response("setVariable",true);xjson_free(reply);set_scalar(adapter,"doubled",100); }
    else {
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"setVariable fault=%zu status=%u sites=%zu\n",fail,status,sites);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !capture.count && adapter->snapshot==snapshot && adapter->stop_epoch==epoch);
        CHECK(runtime_live==live && runtime_bytes==bytes);set_scalar(adapter,"doubled",40);
    }
    CHECK(!adapter->running && adapter->next_handle==next);set_resume(adapter);set_output(0,fail==SIZE_MAX ? "104\n" : "44\n",fail==SIZE_MAX ? 4 : 3);
    set_dispose(&fixture);return sites;
}
#ifndef XR_DAP_SET_VARIABLE_ENTRY
#define XR_DAP_SET_VARIABLE_ENTRY main
#endif
int XR_DAP_SET_VARIABLE_ENTRY(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    set_normal();set_intrinsic();set_managed();set_caller_and_io();size_t sites=set_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t i=0;i<sites;++i) set_fault(i);else { set_fault(0);if (sites>1) set_fault(sites-1); }
    printf("DAP setVariable scalar100/actual104, readonly/type/stale rejection, managed/NUL/CoW, retired refs/restart, original owners; sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
