/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_steps.c - Actual source boundaries without intermediate snapshots
 */
#define main reference_steps_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
typedef struct StepsFixture { XdapAdapter *adapter;XrCompileResources *ledger;XrXirDomain *domain;XrCompileResourceStats baseline; } StepsFixture;
static XrXirStatus steps_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;
    message->size=length;++out->seq;return XR_XIR_OK;
}
static StepsFixture steps_open(const char *file) {
    StepsFixture fixture={0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture.ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture.ledger,&fixture.baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture.ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture.domain,.write=steps_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);
    request(fixture.adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    request(fixture.adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);capture.count=0;return fixture;
}
static void steps_finish(StepsFixture *fixture) {
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void steps_dispose(StepsFixture *fixture) { CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);steps_finish(fixture); }
static void steps_start_leaf(XdapAdapter *adapter) {
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"leaf\"}]}}");
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[]}}");capture.count=0;
}
static XrXirSourceDebugPoint steps_point(XdapAdapter *adapter) {
    XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(adapter->snapshot,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    XrXirSourceDebugPoint point={0};CHECK(xdap_launch_source_point(adapter->launch,frame.function,frame.instruction,&point)==XR_XIR_OK);return point;
}
static void steps_command(XdapAdapter *adapter,const char *command,const char *granularity) {
    char message[512],option[96]={0};if (granularity) snprintf(option,sizeof(option),",\"granularity\":\"%s\"",granularity);
    snprintf(message,sizeof(message),"{\"seq\":6,\"type\":\"request\",\"command\":\"%s\",\"arguments\":{\"threadId\":1%s}}",command,option);
    request(adapter,message);XrJsonValue *reply=last_response(command,true);xjson_free(reply);capture.count=0;
}
static void steps_stop(XdapAdapter *adapter,uint32_t line,uint32_t column,uint64_t previous) {
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
    XrXirSourceDebugPoint point=steps_point(adapter);
    if ((uint32_t)point.range.line!=line || (uint32_t)point.range.column!=column)
        fprintf(stderr,"Source step expected %u:%u actual %d:%d\n",line,column,point.range.line,point.range.column);
    CHECK((uint32_t)point.range.line==line && (uint32_t)point.range.column==column);
    CHECK(adapter->stop_epoch==previous+1);capture.count=0;
}
static void steps_normal(void) {
    StepsFixture fixture=steps_open(XR_DAP_FIXTURE);XdapAdapter *adapter=fixture.adapter;steps_start_leaf(adapter);
    CHECK(steps_point(adapter).range.line==2);uint64_t epoch=adapter->stop_epoch;
    steps_command(adapter,"next",NULL);steps_stop(adapter,3,11,epoch);epoch=adapter->stop_epoch;
    steps_command(adapter,"next","line");steps_stop(adapter,4,5,epoch);epoch=adapter->stop_epoch;
    XrXirVmDebugSnapshot *escaped=NULL;
    CHECK(xdap_current_snapshot(adapter->launch->control,&escaped)==XR_XIR_VALUE_OK && escaped==adapter->snapshot);
    XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};CHECK(xr_xir_vm_debug_snapshot_frame(escaped,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    const XdapCodeFunction *function=&adapter->launch->functions[frame.function];uint32_t string=UINT32_MAX;
    for (uint32_t i=0;i<function->instructions;++i) if (adapter->launch->instructions[function->first+i].op==XR_XIR_CONST_STRING) string=function->parameters+i;
    CHECK(string!=UINT32_MAX);
    XrXirVmDebugSnapshot *same=adapter->snapshot;
    request(adapter,"{\"seq\":7,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1,\"granularity\":\"invalid\"}}");
    XrJsonValue *reply=last_response("next",false);xjson_free(reply);capture.count=0;CHECK(adapter->snapshot==same && adapter->stop_epoch==epoch);
    steps_command(adapter,"stepIn","statement");steps_stop(adapter,5,5,epoch);epoch=adapter->stop_epoch;
    steps_command(adapter,"stepOut","line");steps_stop(adapter,7,1,epoch);CHECK(adapter->depth==1);
    steps_command(adapter,"continue",NULL);CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    XrXirValue value={0};const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_vm_debug_snapshot_value(escaped,0,string,&value)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_view(&value,&bytes,&length) && length==11 && !memcmp(bytes,"owned\0value",11));
    xr_xir_value_drop(&value);xr_xir_vm_debug_snapshot_drop(escaped);steps_finish(&fixture);
}
static void steps_granularity(bool line) {
    StepsFixture fixture=steps_open(XR_DAP_STEPS_FIXTURE);steps_start_leaf(fixture.adapter);
    uint64_t epoch=fixture.adapter->stop_epoch;CHECK(steps_point(fixture.adapter).range.line==2);
    steps_command(fixture.adapter,"next",line ? "line" : "statement");
    steps_stop(fixture.adapter,line ? 3u : 2u,line ? 5u : 28u,epoch);steps_dispose(&fixture);
}
static void steps_into_call(void) {
    StepsFixture fixture=steps_open(XR_DAP_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    const char breakpoint[]="{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_FIXTURE "\"},\"breakpoints\":[{\"line\":7}]}}";
    request(adapter,breakpoint);request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && steps_point(adapter).range.line==7 && adapter->depth==1);
    uint64_t epoch=adapter->stop_epoch;steps_command(adapter,"stepIn",NULL);steps_stop(adapter,2,9,epoch);CHECK(adapter->depth==2);steps_dispose(&fixture);
}
static size_t steps_fault(size_t fail) {
    StepsFixture fixture=steps_open(XR_DAP_FIXTURE);XdapAdapter *adapter=fixture.adapter;steps_start_leaf(adapter);
    XrXirVmDebugSnapshot *old=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    CHECK(xdap_current_continue(adapter->launch->control,epoch,XR_XIR_VM_DEBUG_STATEMENT_OVER)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(fixture.domain);size_t begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;XdapCurrentResult result={0};
    CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(fail==SIZE_MAX || sites>fail);
    if (fail==SIZE_MAX) CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch==epoch+1);
    else CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM && !result.outcome.value.type);
    XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_value(old,0,0,&value)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_I64 && value.payload==20);xr_xir_value_drop(&value);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture.domain);
    CHECK(after.work>=before.work && after.requested_bytes>=before.requested_bytes);
    xdap_current_result_drop(&result);
    XrXirCallStatus cleanup=xdap_adapter_free(adapter);
    CHECK(cleanup==XR_XIR_CALL_READY || (fail!=SIZE_MAX && cleanup==XR_XIR_CALL_OOM));
    steps_finish(&fixture);return sites;
}
static void steps_loop(void) {
    StepsFixture fixture=steps_open(XR_DAP_LOOP_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    const char breakpoint[]="{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_LOOP_FIXTURE "\"},\"breakpoints\":[{\"line\":4}]}}";
    request(adapter,breakpoint);request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && steps_point(adapter).range.line==4);
    const char clear[]="{\"seq\":5,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_LOOP_FIXTURE "\"},\"breakpoints\":[]}}";
    request(adapter,clear);uint64_t epoch=adapter->stop_epoch;
    steps_command(adapter,"next","line");steps_stop(adapter,3,5,epoch);epoch=adapter->stop_epoch;
    steps_command(adapter,"next","line");steps_stop(adapter,4,9,epoch);steps_dispose(&fixture);
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    steps_normal();steps_granularity(false);steps_granularity(true);steps_into_call();steps_loop();
    size_t sites=steps_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t point=0;point<sites;++point) steps_fault(point);
    else { steps_fault(0);if (sites>1) steps_fault(sites-1); }
    printf("Real VM statement/line next/in/out, exact one stop epoch, owned escape; source-step capture sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
