/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_hits.c - Real function encounters, counter reset and atomic failed setters
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
#include "app/dap/xdap_hit.h"
static XrXirStatus hits_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;message->size=length;
    ++out->seq;return XR_XIR_OK;
}
static XdapAdapter *hits_setup(XrCompileResources **ledger,XrXirDomain **domain,XrCompileResourceStats *baseline) {
    memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,ledger)==XR_COMPILE_RESOURCE_OK);CHECK(xr_compile_resources_stats(*ledger,baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{*ledger,xr_xir_compile_default_limits()},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};CHECK(xr_xir_domain_new_budgeted(1048576,&budget,domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=*domain,.write=hits_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    XdapAdapter *adapter=NULL;CHECK(xdap_adapter_new(&compiler,&config,&adapter)==XR_XIR_OK);
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    XrJsonValue *reply=last_response("initialize",true);CHECK(xjson_get_bool(xjson_get_object(reply,"body"),"supportsHitConditionalBreakpoints"));xjson_free(reply);
    const char launch[]="{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_HITS_FIXTURE "\"}}";
    request(adapter,launch);reply=last_response("launch",true);xjson_free(reply);return adapter;
}
static void hits_dispose(XdapAdapter *adapter,XrCompileResources *ledger,XrXirDomain *domain,XrCompileResourceStats baseline) {
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void hits_set(XdapAdapter *adapter,const char *condition) {
    char message[512];snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"record\",\"hitCondition\":\"%s\"}]}}",condition);
    request(adapter,message);XrJsonValue *reply=last_response("setFunctionBreakpoints",true);
    CHECK(xjson_get_bool(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"breakpoints"),0),"verified"));xjson_free(reply);
}
static void hits_drive(XdapAdapter *adapter,const int64_t *expected,uint32_t count) {
    for (uint32_t i=0;i<count;++i) {
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
        XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_value(adapter->snapshot,0,0,&value)==XR_XIR_VALUE_OK);
        CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==expected[i]);xr_xir_value_drop(&value);
        request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    }
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && adapter->outcome.status==XR_XIR_CALL_RETURNED);
}
static void hits_case(const char *condition,const int64_t *expected,uint32_t count,bool restart) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=hits_setup(&ledger,&domain,&baseline);hits_set(adapter,condition);
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");hits_drive(adapter,expected,count);
    CHECK(event_count("stopped")==count && event_count("output")==4 && event_count("terminated")==1);
    if (restart) { request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"restart\"}");
        XrJsonValue *reply=last_response("restart",true);xjson_free(reply);hits_drive(adapter,expected,count);
        CHECK(event_count("stopped")==count*2 && event_count("output")==8 && event_count("terminated")==2); }
    hits_dispose(adapter,ledger,domain,baseline);
}
static void hits_replacement(void) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=hits_setup(&ledger,&domain,&baseline);hits_set(adapter,"%2");
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot);
    XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_value(adapter->snapshot,0,0,&value)==XR_XIR_VALUE_OK && value.payload==22);xr_xir_value_drop(&value);
    const char bad[]="{\"seq\":5,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"record\",\"hitCondition\":\"%0\"}]}}";
    request(adapter,bad);XrJsonValue *reply=last_response("setFunctionBreakpoints",false);xjson_free(reply);
    CHECK(adapter->function_rules[0].hit_kind==XR_XIR_VM_DEBUG_HIT_MULTIPLE && adapter->function_rules[0].hit_value==2);
    hits_set(adapter,"==2");request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    const int64_t last[]={44};hits_drive(adapter,last,1);CHECK(event_count("stopped")==2);
    hits_dispose(adapter,ledger,domain,baseline);
}
static void hits_duplicates(void) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=hits_setup(&ledger,&domain,&baseline);
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"record\",\"hitCondition\":\"%2\"},{\"name\":\"record\",\"hitCondition\":\"==3\"}]}}");
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    const int64_t expected[]={22,33,44};hits_drive(adapter,expected,3);CHECK(event_count("stopped")==3);
    hits_dispose(adapter,ledger,domain,baseline);
}
static void hits_rejected_rules(void) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=hits_setup(&ledger,&domain,&baseline);hits_set(adapter,">=2");
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    XrXirVmDebugRule invalid[]={adapter->function_rules[0],{.location={UINT32_MAX,0},.hit_kind=XR_XIR_VM_DEBUG_HIT_ALWAYS}};
    CHECK(xdap_current_rules_replace(adapter->launch->control,invalid,2)==XR_XIR_VALUE_BAD_ARGUMENT);
    invalid[1]=adapter->function_rules[0];invalid[1].hit_kind=XR_XIR_VM_DEBUG_HIT_MULTIPLE;invalid[1].hit_value=0;
    CHECK(xdap_current_rules_replace(adapter->launch->control,invalid,2)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch);
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    const int64_t expected[]={33,44};hits_drive(adapter,expected,2);CHECK(event_count("stopped")==3);
    hits_dispose(adapter,ledger,domain,baseline);
}
static void hits_parser(void) {
    XrCompileResourceStats baseline;XrCompileResources *owner=NULL;XrCompileResourceLimits limits={65536,65536,65536};
    CHECK(xr_compile_resources_new(&limits,&owner)==XR_COMPILE_RESOURCE_OK);CHECK(xr_compile_resources_stats(owner,&baseline)==XR_COMPILE_RESOURCE_OK);
    const char *bad[]={"%0","-1","18446744073709551616",">=3junk","===2","% 0","!3","\n3"};
    for (uint32_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) { XrXirVmDebugRule rule={.location={7,8},.hit_kind=XR_XIR_VM_DEBUG_HIT_EQUAL,.hit_value=99};
        CHECK(xdap_hit_parse(owner,bad[i],strlen(bad[i]),&rule)==XR_XIR_BAD_STRUCTURE);
        CHECK(rule.location.function==7 && rule.location.instruction==8 && rule.hit_kind==XR_XIR_VM_DEBUG_HIT_EQUAL && rule.hit_value==99); }
    XrXirVmDebugRule rule={0};const char max[]="18446744073709551615";
    CHECK(xdap_hit_parse(owner,max,sizeof(max)-1,&rule)==XR_XIR_OK && rule.hit_value==UINT64_MAX);
    xr_compile_resources_release(owner);CHECK(!runtime_live && !runtime_bytes);
}
static size_t hits_failure(size_t point) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=hits_setup(&ledger,&domain,&baseline);hits_set(adapter,"%2");
    char *old=adapter->function_breakpoints[0];XrXirVmDebugRule saved=adapter->function_rules[0];
    const char message[]="{\"seq\":8,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"record\",\"hitCondition\":\"==3\"}]}}";
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(ledger,&before)==XR_COMPILE_RESOURCE_OK);
    size_t begin=runtime_attempts;runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;
    XrXirStatus status=xdap_adapter_request(adapter,message,sizeof(message)-1);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (point==SIZE_MAX) CHECK(status==XR_XIR_OK);
    else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>point && adapter->function_breakpoints[0]==old);
        CHECK(adapter->function_rules[0].hit_kind==saved.hit_kind && adapter->function_rules[0].hit_value==saved.hit_value);
        XrCompileResourceStats failed={0};CHECK(xr_compile_resources_stats(ledger,&failed)==XR_COMPILE_RESOURCE_OK);
        CHECK(failed.live_bytes==before.live_bytes && failed.work>=before.work && failed.allocated_bytes>=before.allocated_bytes);
        request(adapter,message);
    }
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");const int64_t expected[]={33};hits_drive(adapter,expected,1);
    hits_dispose(adapter,ledger,domain,baseline);return sites;
}
#ifndef XDAP_HITS_MAIN
#define XDAP_HITS_MAIN main
#endif
int XDAP_HITS_MAIN(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);hits_parser();
    const int64_t ge[]={22,33,44},equal[]={33},multiple[]={22,44},greater[]={33,44},less[]={11,22},all[]={11,22,33,44};
    hits_case("2",ge,3,false);hits_case(" >= 2 ",ge,3,false);hits_case("==3",equal,1,false);hits_case("=3",equal,1,false);
    hits_case("%2",multiple,2,true);hits_case(">2",greater,2,false);hits_case("<3",less,2,false);hits_case("<=2",less,2,false);
    hits_case("0",all,4,false);hits_case("==0",NULL,0,false);hits_replacement();hits_duplicates();hits_rejected_rules();
    size_t sites=hits_failure(SIZE_MAX);CHECK(sites);if (resources) for (size_t i=0;i<sites;++i) hits_failure(i);
    printf("Real function hit operators, duplicates/reset/restart and failed setter: %zu allocation sites, injection=%s; physical0/0\n",sites,resources ? "ALL" : "NOT_RUN");return 0;
}
