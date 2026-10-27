/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_wait.c - Authentic pending timers, stale tokens and responsive control
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
typedef struct WaitFixture {
    XrCompileResources *resources;
    XrCompileResourceStats baseline;
    XrXirDomain *domain;
    XdapAdapter *adapter;
    int64_t now;
    unsigned reads;
    bool fail;
} WaitFixture;
static XrXirTimeStatus wait_clock(void *context,XrXirClockKind clock,int64_t *output) {
    WaitFixture *fixture=context;CHECK(clock==XR_XIR_CLOCK_MONOTONIC);++fixture->reads;
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_BUSY);
    CHECK(xdap_adapter_step(fixture->adapter,1)==XR_XIR_BAD_STRUCTURE);
    if (fixture->fail) return XR_XIR_TIME_FAILED;
    *output=fixture->now;return XR_XIR_TIME_OK;
}
static XrXirTimeStatus wait_offset(void *context,int64_t seconds,int64_t *output) {
    (void)context;(void)seconds;*output=0;return XR_XIR_TIME_OK;
}
static void wait_new(WaitFixture *fixture) {
    memset(fixture,0,sizeof(*fixture));memset(&capture,0,sizeof(capture));fixture->now=1000000000;
    XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture->resources,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture->resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture->domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture->domain,.write=capture_message,
        .context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    config.instance.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,wait_clock,wait_offset,fixture};
    CHECK(xdap_adapter_new(&compiler,&config,&fixture->adapter)==XR_XIR_OK && !compiler);
    capture.adapter=fixture->adapter;capture.reentry=true;
    request(fixture->adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    char message[4096];snprintf(message,sizeof(message),
        "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",XR_DAP_TIMER_FIXTURE);
    request(fixture->adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);
    request(fixture->adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\"}");
}
static void wait_free(WaitFixture *fixture) {
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void timer_boundary(void) {
    WaitFixture fixture;wait_new(&fixture);XdapAdapter *adapter=fixture.adapter;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && xdap_adapter_waiting(adapter));
    CHECK(fixture.reads==1 && !event_count("output"));uint32_t messages=capture.count;
    fixture.now+=119999999;
    for (unsigned i=0;i<3;++i) CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && xdap_adapter_waiting(adapter));
    CHECK(capture.count==messages && fixture.reads==4);
    ++fixture.now;CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && !xdap_adapter_running(adapter));
    CHECK(event_count("output")==1 && event_count("exited")==1 && event_count("terminated")==1);
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("terminated")==1);wait_free(&fixture);
}
static void timer_control(void) {
    WaitFixture fixture;wait_new(&fixture);XdapAdapter *adapter=fixture.adapter;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && xdap_adapter_waiting(adapter));
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"pause\",\"arguments\":{\"threadId\":1}}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && !event_count("stopped"));
    fixture.now+=120000000;CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==1);
    CHECK(!xdap_adapter_running(adapter) && !event_count("output"));
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"terminate\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("terminated")==1 && !event_count("output"));
    wait_free(&fixture);
    wait_new(&fixture);adapter=fixture.adapter;CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && xdap_adapter_waiting(adapter));
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"terminate\"}");
    CHECK(!xdap_adapter_waiting(adapter));fixture.fail=true;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && !event_count("output") && event_count("terminated")==1);
    wait_free(&fixture);
}
static void timer_failure(void) {
    for (unsigned mode=0;mode<3;++mode) {
        WaitFixture fixture;wait_new(&fixture);XdapAdapter *adapter=fixture.adapter;
        if (mode==2) fixture.fail=true;
        else CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && xdap_adapter_waiting(adapter));
        if (!mode) --fixture.now;else fixture.fail=true;
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_IO && xdap_adapter_running(adapter));
        CHECK(!event_count("output") && !event_count("terminated"));wait_free(&fixture);
    }
}
static void wait_tokens(void) {
    WaitFixture fixture;wait_new(&fixture);XdapCurrent *control=fixture.adapter->launch->control;
    XdapCurrentResult result={0};CHECK(xdap_current_poll(control,1000,&result)==XR_XIR_VALUE_OK);
    CHECK(result.state==XDAP_CURRENT_WAITING && result.instance_epoch && result.outcome.wake);
    XrXirWaitRequest request_value;memset(&request_value,0xa5,sizeof(request_value));
    XrXirWaitRequest sentinel=request_value;
    CHECK(xdap_current_wait_request(control,result.instance_epoch+1,result.outcome.wake,&request_value)==XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&request_value,&sentinel,sizeof(sentinel)));
    CHECK(xdap_current_wait_request(control,result.instance_epoch,result.outcome.wake+1,&request_value)==XR_XIR_CALL_BAD_STATE);
    CHECK(!memcmp(&request_value,&sentinel,sizeof(sentinel)));
    CHECK(xdap_current_resume(control,result.instance_epoch+1,result.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xdap_current_resume(control,result.instance_epoch,result.outcome.wake+1)==XR_XIR_CALL_BAD_STATE);
    CHECK(xdap_current_wait_request(control,result.instance_epoch,result.outcome.wake,&request_value)==XR_XIR_CALL_READY);
    CHECK(request_value.kind==XR_XIR_WAIT_TIMER_MS && request_value.after_ms==120);
    CHECK(xdap_current_continue(control,result.instance_epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_resume(control,result.instance_epoch,result.outcome.wake)==XR_XIR_CALL_READY);
    CHECK(xdap_current_resume(control,result.instance_epoch,result.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(!event_count("output"));xdap_current_result_drop(&result);wait_free(&fixture);
}
int main(void) {
    xr_test_suppress_dialogs();timer_boundary();timer_control();timer_failure();wait_tokens();
    puts("Authentic timer duration, pause/cancel, failed/backward clocks and owner baselines: PASS");return 0;
}
