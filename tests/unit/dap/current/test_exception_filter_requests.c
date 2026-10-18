/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_exception_filter_requests.c - Atomic protocol filters and captured stop modes
 */
#define XR_DAP_EXCEPTION_ENTRY reference_filter_exceptions_main
#include "test_exceptions.c"
#include "xir/xxir_vm_debug_internal.h"
static void filter_info(XdapAdapter *adapter,const char *mode) {
    request(adapter,"{\"seq\":80,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}");
    XrJsonValue *reply=last_response("exceptionInfo",true),*body=xjson_get_object(reply,"body");
    CHECK(!strcmp(xjson_get_string(body,"exceptionId"),"DapFailure.Expected") && !strcmp(xjson_get_string(body,"breakMode"),mode));xjson_free(reply);
}
static void filter_close(XdapAdapter *adapter,XrXirDomain *domain,XrCompileResources *resources,XrCompileResourceStats baseline) {
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY && !xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
}
static void filter_open(XdapAdapter *adapter,const char *filters,bool caught) {
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    XrJsonValue *reply=last_response("initialize",true),*items=xjson_get_array(xjson_get_object(reply,"body"),"exceptionBreakpointFilters");
    CHECK(xjson_array_len(items)==3);const char *names[]={"all","caught","uncaught"};
    for (int i=0;i<3;++i) CHECK(!strcmp(xjson_get_string(xjson_array_get(items,i),"filter"),names[i]));xjson_free(reply);
    char message[4096];snprintf(message,sizeof(message),
        "{\"seq\":2,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":%s}}",filters);
    request(adapter,message);exception_reply("setExceptionBreakpoints",true);
    snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",
        caught ? XR_DAP_CAUGHT_FIXTURE : XR_DAP_UNCAUGHT_FIXTURE);
    request(adapter,message);exception_reply("launch",true);
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");exception_reply("configurationDone",true);
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK);
}
static void filter_case(bool caught,const char *filters,bool stopped,const char *mode) {
    XrCompileResources *resources=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=exception_setup(&resources,&domain,&baseline);filter_open(adapter,filters,caught);
    XrXirVmDebugSnapshot *escaped=NULL;uint32_t saved=0;
    if (stopped) {
        CHECK(adapter->snapshot && !adapter->terminal && event_count("stopped")==1 && !event_count("output"));filter_info(adapter,mode);
        CHECK(xdap_current_snapshot(adapter->launch->control,&escaped)==XR_XIR_VALUE_OK);
        CHECK(xir_vm_debug_snapshot_exception_filter(escaped,&saved)==XR_XIR_VALUE_OK);
        request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[]}}");exception_reply("setExceptionBreakpoints",true);
        CHECK(!adapter->break_throw);filter_info(adapter,mode);
        request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");exception_reply("continue",true);
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK);
    }
    CHECK(adapter->terminal && event_count("stopped")== (stopped ? 1u : 0u) && event_count("terminated")==1 && event_count("exited")==1);
    CHECK(adapter->outcome.status==(caught ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_THROWN));
    CHECK(event_count("output")== (caught ? 1u : 0u));
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    if (escaped) { uint32_t after=0;CHECK(xir_vm_debug_snapshot_exception_filter(escaped,&after)==XR_XIR_VALUE_OK && after==saved); }
    xr_xir_vm_debug_snapshot_drop(escaped);
    CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
}
static size_t filter_config_fault(size_t fail) {
    XrCompileResources *resources=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=exception_setup(&resources,&domain,&baseline);filter_open(adapter,"[\"all\"]",true);
    CHECK(adapter->snapshot && adapter->break_throw==XR_XIR_VM_DEBUG_EXCEPTION_ALL);XrXirVmDebugSnapshot *snapshot=adapter->snapshot;
    uint64_t epoch=adapter->stop_epoch;XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirDomainBudgetStats domain_before=xr_xir_domain_budget_stats(domain);
    const char message[]="{\"seq\":90,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"caught\"]}}";
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,sizeof(message)-1);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) CHECK(status==XR_XIR_OK && adapter->break_throw==XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT);
    else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && adapter->break_throw==XR_XIR_VM_DEBUG_EXCEPTION_ALL);
        request(adapter,message);exception_reply("setExceptionBreakpoints",true);CHECK(adapter->break_throw==XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT);
    }
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running);filter_info(adapter,"always");
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes && after.work>=before.work && after.allocated_bytes>=before.allocated_bytes);
    XrXirDomainBudgetStats domain_after=xr_xir_domain_budget_stats(domain);
    CHECK(domain_after.metadata_live==domain_before.metadata_live && domain_after.work>=domain_before.work);
    filter_close(adapter,domain,resources,baseline);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    filter_case(true,"[\"caught\"]",true,"always");filter_case(false,"[\"caught\"]",false,NULL);
    filter_case(true,"[\"uncaught\"]",false,NULL);filter_case(false,"[\"uncaught\"]",true,"unhandled");
    filter_case(true,"[\"caught\",\"uncaught\"]",true,"always");filter_case(false,"[\"caught\",\"uncaught\"]",true,"unhandled");
    filter_case(false,"[\"all\",\"caught\",\"uncaught\"]",true,"always");filter_case(false,"[]",false,NULL);
    size_t sites=filter_config_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t fail=0;fail<sites;++fail) filter_config_fault(fail);
    else { filter_config_fault(0);filter_config_fault(sites-1); }
    printf("JSON caught/uncaught/combined/disabled, captured breakMode after policy changes and producer death; config sites=%zu injection=%s same owner retry/physical0/0 PASS\n",
        sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
