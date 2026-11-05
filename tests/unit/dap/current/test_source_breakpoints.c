/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_breakpoints.c - Real file sites, restart and atomic rule admission
 */
#define main reference_source_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"

static XrXirStatus source_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;
    message->size=length;++out->seq;return XR_XIR_OK;
}
typedef struct SourceFixture { XdapAdapter *adapter;XrCompileResources *ledger;XrXirDomain *domain;XrCompileResourceStats baseline; } SourceFixture;
static SourceFixture source_fixture(const char *file) {
    SourceFixture fixture={0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture.ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture.ledger,&fixture.baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture.ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XdapAdapterConfig config={.write=source_write,.context=&capture,.message_limit=16384};
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);config.observer_metadata=fixture.domain;
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);
    request(fixture.adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    request(fixture.adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);capture.count=0;return fixture;
}
static void source_dispose(SourceFixture *fixture) {
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);
    CHECK(!runtime_live && !runtime_bytes);
}
static size_t source_message(char *output,size_t capacity,const char *file,uint32_t line,const char *hit) {
    char predicate[96]={0};if (hit) snprintf(predicate,sizeof(predicate),",\"hitCondition\":\"%s\"",hit);
    int size=snprintf(output,capacity,"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%u%s}]}}",file,line,predicate);
    CHECK(size>0 && (size_t)size<capacity);return (size_t)size;
}
static void source_set(XdapAdapter *adapter,const char *file,uint32_t line,const char *hit) {
    char message[1024];source_message(message,sizeof(message),file,line,hit);request(adapter,message);
    XrJsonValue *reply=last_response("setBreakpoints",true);
    XrJsonValue *point=xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"breakpoints"),0);
    CHECK(xjson_get_bool(point,"verified") && xjson_get_int(point,"line")==line);xjson_free(reply);capture.count=0;
}
static void source_clear(XdapAdapter *adapter,const char *file) {
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[]}}",file);
    request(adapter,message);XrJsonValue *reply=last_response("setBreakpoints",true);xjson_free(reply);capture.count=0;
}
static void source_function(XdapAdapter *adapter,const char *name) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":4,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"%s\"}]}}",name);
    request(adapter,message);XrJsonValue *reply=last_response("setFunctionBreakpoints",true);xjson_free(reply);capture.count=0;
}
static void source_start(XdapAdapter *adapter) {
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"configurationDone\"}");capture.count=0;
}
static void source_resume(XdapAdapter *adapter) {
    request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
}
static void source_stopped(XdapAdapter *adapter,uint32_t line) {
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
    XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(adapter->snapshot,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    XrXirSourceDebugPoint point={0};CHECK(xdap_launch_source_point(adapter->launch,frame.function,frame.instruction,&point)==XR_XIR_OK);
    CHECK((uint32_t)point.range.line==line);capture.count=0;
}
static void source_normal(void) {
    SourceFixture fixture=source_fixture(XR_DAP_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    source_set(adapter,XR_DAP_FIXTURE,4,NULL);source_function(adapter,"leaf");
    CHECK(adapter->source_breakpoint_count==1 && adapter->function_breakpoint_count==1);
    source_start(adapter);source_stopped(adapter,2);source_resume(adapter);source_stopped(adapter,4);
    request(adapter,"{\"seq\":7,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[]}}");
    CHECK(!adapter->function_breakpoint_count && adapter->source_breakpoint_count==1);capture.count=0;
    char invalid[1024];size_t length=source_message(invalid,sizeof(invalid),XR_DAP_FIXTURE,0,NULL);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    CHECK(xdap_adapter_request(adapter,invalid,length)==XR_XIR_BAD_STRUCTURE);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && adapter->source_breakpoints[0].line==4);
    const char modified[]="{\"seq\":7,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_FIXTURE "\"},\"sourceModified\":true,\"breakpoints\":[{\"line\":5}]}}";
    request(adapter,modified);XrJsonValue *rejected=last_response("setBreakpoints",false);xjson_free(rejected);capture.count=0;
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && adapter->source_breakpoints[0].line==4);
    source_set(adapter,XR_DAP_FIXTURE,5,NULL);source_resume(adapter);source_stopped(adapter,5);
    source_clear(adapter,XR_DAP_FIXTURE);CHECK(!adapter->source_breakpoint_count);
    source_resume(adapter);CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);
    source_set(adapter,XR_DAP_FIXTURE,4,NULL);
    request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"restart\"}");source_stopped(adapter,4);
    source_clear(adapter,XR_DAP_FIXTURE);source_resume(adapter);
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && adapter->outcome.status==XR_XIR_CALL_RETURNED);
    source_dispose(&fixture);
}
static void source_loop_hit(void) {
    SourceFixture fixture=source_fixture(XR_DAP_LOOP_FIXTURE);
    source_set(fixture.adapter,XR_DAP_LOOP_FIXTURE,3,"==3");source_start(fixture.adapter);source_stopped(fixture.adapter,3);
    source_resume(fixture.adapter);CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && fixture.adapter->terminal);
    source_dispose(&fixture);
    fixture=source_fixture(XR_DAP_LOOP_FIXTURE);source_set(fixture.adapter,XR_DAP_LOOP_FIXTURE,3,"3");source_start(fixture.adapter);
    for (uint32_t i=0;i<3;++i) {
        source_stopped(fixture.adapter,3);
        if (!i) {
            char unknown[1024];source_message(unknown,sizeof(unknown),"not-a-loaded-module.xr",4,NULL);request(fixture.adapter,unknown);
            XrJsonValue *reply=last_response("setBreakpoints",true);
            CHECK(!xjson_get_bool(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"breakpoints"),0),"verified"));
            xjson_free(reply);capture.count=0;CHECK(fixture.adapter->source_breakpoint_count==1);
        }
        source_resume(fixture.adapter);
    }
    CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && fixture.adapter->terminal);source_dispose(&fixture);
}
static void source_files(void) {
    SourceFixture fixture=source_fixture(XR_DAP_PRIVATE_MAIN);XdapAdapter *adapter=fixture.adapter;
    source_set(adapter,XR_DAP_PRIVATE_MAIN,5,NULL);source_set(adapter,XR_DAP_PRIVATE_LIB,2,NULL);
    CHECK(adapter->source_breakpoint_count==2);source_start(adapter);source_stopped(adapter,5);
    source_clear(adapter,XR_DAP_PRIVATE_MAIN);CHECK(adapter->source_breakpoint_count==1);
    source_resume(adapter);source_stopped(adapter,2);source_clear(adapter,XR_DAP_PRIVATE_LIB);
    CHECK(!adapter->source_breakpoint_count);source_resume(adapter);
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && adapter->outcome.status==XR_XIR_CALL_THROWN);
    source_dispose(&fixture);
}
static size_t source_fault(size_t fail) {
    SourceFixture fixture=source_fixture(XR_DAP_LOOP_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    source_set(adapter,XR_DAP_LOOP_FIXTURE,4,NULL);source_function(adapter,"accumulate");source_start(adapter);
    source_stopped(adapter,2);source_resume(adapter);source_stopped(adapter,4);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    uint32_t next=adapter->next_handle,depth=adapter->depth;char *identity=adapter->source_breakpoints[0].identity;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirDomainBudgetStats domain=xr_xir_domain_budget_stats(fixture.domain);
    char message[1024];size_t length=source_message(message,sizeof(message),XR_DAP_LOOP_FIXTURE,6,NULL);
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,length);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) CHECK(status==XR_XIR_OK && adapter->source_breakpoints[0].line==6);
    else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !capture.count);
        CHECK(adapter->source_breakpoints[0].line==4 && adapter->source_breakpoints[0].identity==identity);
        XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(fixture.domain);
        CHECK(spent.work==domain.work && spent.requested_bytes==domain.requested_bytes && spent.metadata_live==domain.metadata_live);
    }
    CHECK(adapter->function_breakpoint_count==1 && adapter->source_breakpoint_count==1);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && adapter->next_handle==next && adapter->depth==depth && !adapter->running);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes && after.work>before.work && after.allocated_bytes>=before.allocated_bytes);
    source_resume(adapter);source_stopped(adapter,fail==SIZE_MAX ? 6 : 4);
    source_dispose(&fixture);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    source_normal();source_loop_hit();source_files();size_t sites=source_fault(SIZE_MAX);CHECK(sites);
    if (!resources) { source_fault(0);if (sites>1) source_fault(sites-1); }
    if (resources) for (size_t point=0;point<sites;++point) source_fault(point);
    printf("Source file/statement rules, multi-file/function coexistence, replacement/restart, real loop header hit3; setter sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
