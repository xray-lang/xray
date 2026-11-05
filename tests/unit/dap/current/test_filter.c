/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_filter.c - Authenticated stop causes and preserved stepping anchors
 */
#define main reference_filter_dispatch_main
#include "test_dispatch.c"
#undef main
#include "xir/xxir_vm_debug_internal.h"
#include "app/dap/xdap_dispatch_internal.h"
typedef struct FilterFixture { XdapAdapter *adapter;XrCompileResources *ledger;XrXirDomain *domain;XrCompileResourceStats baseline; } FilterFixture;
static XrXirStatus filter_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;
    message->size=length;++out->seq;return XR_XIR_OK;
}
static FilterFixture filter_open(const char *file) {
    FilterFixture fixture={0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture.ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture.ledger,&fixture.baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture.ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture.domain,.write=filter_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);
    request(fixture.adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    request(fixture.adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);capture.count=0;return fixture;
}
static void filter_finish(FilterFixture *fixture) {
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void filter_dispose(FilterFixture *fixture) { CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);filter_finish(fixture); }
static XrXirVmDebugFrameInfo filter_frame(FilterFixture *fixture,const XrXirVmDebugSnapshot *snapshot,uint32_t line) {
    XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    XrXirSourceDebugPoint point={0};CHECK(xdap_launch_source_point(fixture->adapter->launch,frame.function,frame.instruction,&point)==XR_XIR_OK);
    CHECK((uint32_t)point.range.line==line);return frame;
}
static void filter_anchor(void) {
    FilterFixture fixture=filter_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    const char outer[]="{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_CALL_FIXTURE "\"},\"breakpoints\":[{\"line\":9}]}}";
    request(adapter,outer);request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && adapter->depth==2);
    XrXirVmDebugFrameInfo original=filter_frame(&fixture,adapter->snapshot,9);
    const char inner[]="{\"seq\":5,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_CALL_FIXTURE "\"},\"breakpoints\":[{\"line\":4}]}}";
    request(adapter,inner);request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1,\"granularity\":\"line\"}}");
    XdapCurrentResult result={0};CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *hidden=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&hidden)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_snapshot_depth(hidden)==3);filter_frame(&fixture,hidden,4);
    XirVmDebugStopCause cause={0};CHECK(xir_vm_debug_snapshot_cause(hidden,&cause)==XR_XIR_VALUE_OK && cause.groups==1 && !cause.unfiltered);
    CHECK(xdap_current_continue_filtered(adapter->launch->control,result.debug_epoch+1)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_continue_filtered(adapter->launch->control,result.debug_epoch)==XR_XIR_VALUE_OK);xdap_current_result_drop(&result);
    CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
    XrXirVmDebugSnapshot *visible=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&visible)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_snapshot_depth(visible)==2);XrXirVmDebugFrameInfo returned=filter_frame(&fixture,visible,10);CHECK(returned.id==original.id);
    CHECK(xir_vm_debug_snapshot_cause(visible,&cause)==XR_XIR_VALUE_OK && !cause.groups && cause.unfiltered);
    CHECK(xdap_current_continue_filtered(adapter->launch->control,result.debug_epoch)==XR_XIR_VALUE_BAD_ARGUMENT);
    xdap_current_result_drop(&result);CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    CHECK(xir_vm_debug_snapshot_cause(hidden,&cause)==XR_XIR_VALUE_OK && cause.groups==1 && !cause.unfiltered);
    xr_xir_vm_debug_snapshot_drop(hidden);xr_xir_vm_debug_snapshot_drop(visible);filter_finish(&fixture);
}
static void filter_groups(void) {
    FilterFixture fixture=filter_open(XR_DAP_LOOP_FIXTURE);XdapAdapter *adapter=fixture.adapter;uint32_t function=UINT32_MAX,instruction=UINT32_MAX;
    for (uint32_t f=0;f<adapter->launch->function_count;++f) {
        if (strcmp(adapter->launch->functions[f].name,"accumulate")) continue;
        function=f;
        for (uint32_t i=0;i<adapter->launch->functions[f].instructions;++i) {
            XrXirSourceDebugPoint point={0};
            if (xdap_launch_source_point(adapter->launch,f,i,&point)==XR_XIR_OK && point.range.line==4 && (point.flags&XR_XIR_SOURCE_DEBUG_ENTRY)) { instruction=i;break; }
        }
    }
    CHECK(function!=UINT32_MAX && instruction!=UINT32_MAX);
    XrXirVmDebugRule rules[3]={
        {{function,instruction},XR_XIR_VM_DEBUG_HIT_ALWAYS,0,32},
        {{function,instruction},XR_XIR_VM_DEBUG_HIT_EQUAL,2,7},
        {{function,instruction},XR_XIR_VM_DEBUG_HIT_ALWAYS,0,32}};
    CHECK(xdap_current_rules_replace(adapter->launch->control,rules,3)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_start(adapter->launch->control,adapter->launch->entry,NULL,0)==XR_XIR_CALL_READY);
    for (uint32_t visit=0;visit<4;++visit) {
        XdapCurrentResult result={0};CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
        XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&snapshot)==XR_XIR_VALUE_OK);filter_frame(&fixture,snapshot,4);
        XirVmDebugStopCause cause={0};uint32_t expected=UINT32_C(1)<<31;if (visit==1) expected|=UINT32_C(1)<<6;
        CHECK(xir_vm_debug_snapshot_cause(snapshot,&cause)==XR_XIR_VALUE_OK && cause.groups==expected && !cause.unfiltered);
        CHECK(xdap_current_continue_filtered(adapter->launch->control,result.debug_epoch)==XR_XIR_VALUE_OK);
        xr_xir_vm_debug_snapshot_drop(snapshot);xdap_current_result_drop(&result);
    }
    XdapCurrentResult result={0};CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED);
    xdap_current_result_drop(&result);filter_dispose(&fixture);
}
int main(void) {
    xr_test_suppress_dialogs();filter_anchor();filter_groups();
    puts("Real logical stop masks/group32/shared counters, stale/unfiltered refusal, preserved caller/source step anchor, owned cause and physical=0/0 PASS");return 0;
}
