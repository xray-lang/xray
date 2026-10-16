/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_detach.c - Real detached execution outlives its debugger and compiler
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
typedef struct DetachFixture {
    XrCompileResources *resources;
    XrCompileResourceStats baseline;
    XrXirDomain *domain;
    XdapAdapter *adapter;
    XdapDetached *target;
    XrXirOutputSink sink;
    char bytes[256];
    size_t length;
    int64_t now;
    uint32_t reads,fail_write,handoff_writes;
    bool fail_clock,fail_output;
} DetachFixture;
static const char detach_request[]="{\"seq\":8,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{\"terminateDebuggee\":false}}";
static XrXirStatus detach_capture(void *context,const char *bytes,size_t length) {
    DetachFixture *fixture=context;CHECK(capture.count<128 && length<sizeof(capture.messages[0].bytes));
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_BUSY);
    if (fixture->target) {
        bool ended=true;CHECK(xdap_detached_free(fixture->target)==XR_XIR_CALL_BUSY);
        CHECK(xdap_detached_poll(fixture->target,1,&ended)==XR_XIR_BAD_STRUCTURE && ended);
    }
    if (fixture->target && ++fixture->handoff_writes==fixture->fail_write) return XR_XIR_IO;
    Message *message=&capture.messages[capture.count++];memcpy(message->bytes,bytes,length);
    message->bytes[length]=0;message->size=length;++capture.seq;return XR_XIR_OK;
}
static XrXirOutputStatus detach_host_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    DetachFixture *fixture=context;CHECK(!fixture->adapter && fixture->target && stream==XR_XIR_STDOUT);
    CHECK(xdap_detached_free(fixture->target)==XR_XIR_CALL_BUSY);
    bool ended=true;CHECK(xdap_detached_poll(fixture->target,1,&ended)==XR_XIR_BAD_STRUCTURE && ended);
    if (fixture->fail_output) return XR_XIR_OUTPUT_ERROR;
    CHECK(length<=sizeof(fixture->bytes)-fixture->length);memcpy(fixture->bytes+fixture->length,bytes,length);
    fixture->length+=length;return XR_XIR_OUTPUT_OK;
}
static XrXirTimeStatus detach_clock(void *context,XrXirClockKind clock,int64_t *output) {
    DetachFixture *fixture=context;CHECK(clock==XR_XIR_CLOCK_MONOTONIC);++fixture->reads;
    if (fixture->target) CHECK(xdap_detached_free(fixture->target)==XR_XIR_CALL_BUSY);
    else CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_BUSY);
    if (fixture->fail_clock) return XR_XIR_TIME_FAILED;
    *output=fixture->now;return XR_XIR_TIME_OK;
}
static XrXirTimeStatus detach_offset(void *context,int64_t seconds,int64_t *output) {
    (void)context;(void)seconds;*output=0;return XR_XIR_TIME_OK;
}
static void detach_open(DetachFixture *fixture,const char *file,bool attached) {
    memset(fixture,0,sizeof(*fixture));memset(&capture,0,sizeof(capture));fixture->now=1000000000;
    XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture->resources,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture->resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture->domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture->domain,.write=detach_capture,.context=fixture,
        .message_limit=16384,.detached=&fixture->target};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    config.instance.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,detach_clock,detach_offset,fixture};
    fixture->sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,detach_host_output,fixture,sizeof(fixture->bytes)};
    config.instance.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&fixture->sink};
    XdapAdapterConfig invalid=config;invalid.instance.output=(XrXirOutputProvider){0};
    CHECK(xdap_adapter_new(&compiler,&invalid,&fixture->adapter)==XR_XIR_BAD_STRUCTURE && compiler && !fixture->adapter && !fixture->target);
    invalid=config;invalid.instance.output.abi_version++;
    CHECK(xdap_adapter_new(&compiler,&invalid,&fixture->adapter)==XR_XIR_BAD_STRUCTURE && compiler && !fixture->adapter);
    XdapDetached occupied={0};XdapDetached *occupied_slot=&occupied;invalid=config;invalid.detached=&occupied_slot;
    CHECK(xdap_adapter_new(&compiler,&invalid,&fixture->adapter)==XR_XIR_BAD_STRUCTURE && compiler && occupied_slot==&occupied);
    CHECK(xdap_adapter_new(&compiler,&config,&fixture->adapter)==XR_XIR_OK && !compiler);
    request(fixture->adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    XrJsonValue *reply=last_response("initialize",true);
    const char *capabilities[]={"supportsConfigurationDoneRequest","supportsConditionalBreakpoints","supportsHitConditionalBreakpoints",
        "supportsLogPoints","supportsFunctionBreakpoints","supportsTerminateRequest","supportsRestartRequest","supportsSetVariable",
        "supportsEvaluateForHovers","supportsDisassembleRequest","supportTerminateDebuggee","supportsPauseRequest","supportsExceptionInfoRequest"};
    for (uint32_t i=0;i<sizeof(capabilities)/sizeof(*capabilities);++i) CHECK(xjson_get_bool(xjson_get_object(reply,"body"),capabilities[i]));
    xjson_free(reply);
    char message[4096];snprintf(message,sizeof(message),
        "{\"seq\":2,\"type\":\"request\",\"command\":\"%s\",\"arguments\":{\"program\":\"%s\"}}",attached ? "attach" : "launch",file);
    request(fixture->adapter,message);reply=last_response(attached ? "attach" : "launch",true);xjson_free(reply);
}
static void detach_adapter_drop(DetachFixture *fixture) {
    CHECK(fixture->target && xdap_adapter_closed(fixture->adapter));
    CHECK(!fixture->adapter->launch && !fixture->adapter->execution && !fixture->adapter->snapshot && !fixture->adapter->running);
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);fixture->adapter=NULL;
}
static void detach_close(DetachFixture *fixture) {
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);fixture->adapter=NULL;
    CHECK(xdap_detached_free(fixture->target)==XR_XIR_CALL_READY);fixture->target=NULL;
    if (fixture->domain) { CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain); }
    if (fixture->resources) {
        XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->resources);
    }
    CHECK(!runtime_live && !runtime_bytes);
}
static void detach_start(DetachFixture *fixture,bool children,bool paused) {
    XdapAdapter *adapter=fixture->adapter;
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\"}");
    if (children) {
        uint64_t ids[64];uint32_t count=0;bool found=false;
        for (uint32_t step=0;step<10000 && !adapter->terminal;++step) {
            CHECK(xdap_adapter_step(adapter,1)==XR_XIR_OK);
            XrXirValueStatus status=xdap_current_threads(adapter->launch->control,ids,64,&count);
            if (status==XR_XIR_VALUE_UNSUPPORTED) continue;
            CHECK(status==XR_XIR_VALUE_OK);
            if (count==3) { CHECK(ids[0]==1 && ids[1]>1 && ids[2]>ids[1]);found=true;break; }
        }
        CHECK(found && !event_count("output"));
    }
    if (paused) {
        request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"pause\",\"arguments\":{\"threadId\":1}}");
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
    }
}
static void detach_finish(DetachFixture *fixture,const char *bytes,size_t length,XrXirCallStatus expected) {
    bool ended=true;uint32_t messages=capture.count;
    CHECK(xdap_detached_poll(fixture->target,0,&ended)==XR_XIR_BAD_STRUCTURE && ended);ended=false;
    XrXirCallResult before={0};CHECK(xdap_detached_result(fixture->target,&before)==XR_XIR_VALUE_UNSUPPORTED);
    for (uint32_t step=0;step<10000 && !ended;++step) CHECK(xdap_detached_poll(fixture->target,1,&ended)==XR_XIR_OK);
    CHECK(ended && fixture->length==length && !memcmp(fixture->bytes,bytes,length) && capture.count==messages);
    XrXirCallResult result={0};CHECK(xdap_detached_result(fixture->target,&result)==XR_XIR_VALUE_OK && result.status==expected);
    xr_xir_call_result_drop(&result);ended=false;
    CHECK(xdap_detached_poll(fixture->target,1,&ended)==XR_XIR_OK && ended && capture.count==messages);
}
static size_t detach_children(bool paused,size_t fail,uint32_t io_failure,bool release_host) {
    DetachFixture fixture;detach_open(&fixture,XR_DAP_THREADS_FIXTURE,false);detach_start(&fixture,true,paused);
    XdapAdapter *adapter=fixture.adapter;XdapLaunch *launch=adapter->launch;
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot,*escaped=NULL;uint64_t epoch=adapter->stop_epoch;uint32_t seq=adapter->seq;
    if (paused) CHECK(xdap_current_snapshot(launch->control,&escaped)==XR_XIR_VALUE_OK);
    capture.count=0;fixture.fail_write=io_failure;XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(fixture.resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,detach_request,sizeof(detach_request)-1);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail!=SIZE_MAX) {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !fixture.target && !adapter->closed && !capture.count && adapter->seq==seq);
        CHECK(adapter->launch==launch && adapter->snapshot==snapshot && adapter->stop_epoch==epoch && adapter->running!=paused);
        CHECK(xr_compile_resources_stats(fixture.resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes && after.allocated_bytes>=before.allocated_bytes && after.work>before.work);
        if (!paused) CHECK(xdap_adapter_step(adapter,16)==XR_XIR_OK && adapter->running && !adapter->snapshot);
        request(adapter,detach_request);
    } else CHECK(status==(io_failure ? XR_XIR_IO : XR_XIR_OK));
    CHECK(fixture.target && adapter->closed && !fixture.length);
    if (!io_failure) {
        XrJsonValue *reply=last_response("disconnect",true);xjson_free(reply);
        CHECK(event_count("terminated")==1 && !event_count("exited"));
    } else CHECK(capture.count==io_failure-1);
    detach_adapter_drop(&fixture);
    if (release_host) {
        xr_xir_domain_drop(fixture.domain);fixture.domain=NULL;
        xr_compile_resources_release(fixture.resources);fixture.resources=NULL;
    }
    detach_finish(&fixture,"33\n",3,XR_XIR_CALL_RETURNED);
    CHECK(xdap_detached_free(fixture.target)==XR_XIR_CALL_READY);fixture.target=NULL;
    if (escaped) { CHECK(xr_xir_vm_debug_snapshot_depth(escaped)>0);xr_xir_vm_debug_snapshot_drop(escaped); }
    detach_close(&fixture);return sites;
}
static void detach_timer(bool unstarted) {
    DetachFixture fixture;detach_open(&fixture,XR_DAP_TIMER_FIXTURE,false);
    if (!unstarted) {
        detach_start(&fixture,false,false);
        CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && xdap_adapter_waiting(fixture.adapter));
    }
    request(fixture.adapter,detach_request);CHECK(fixture.target);detach_adapter_drop(&fixture);
    bool ended=false;
    if (unstarted) CHECK(xdap_detached_poll(fixture.target,1000,&ended)==XR_XIR_OK && !ended && xdap_detached_waiting(fixture.target));
    uint64_t wake=fixture.target->wait_wake,epoch=fixture.target->wait_epoch;CHECK(wake && epoch);
    fixture.fail_clock=true;ended=true;
    CHECK(xdap_detached_poll(fixture.target,1000,&ended)==XR_XIR_IO && ended && fixture.target->wait_wake==wake);
    fixture.fail_clock=false;--fixture.now;
    CHECK(xdap_detached_poll(fixture.target,1000,&ended)==XR_XIR_IO && ended && fixture.target->wait_epoch==epoch);
    fixture.now=1119999999;uint32_t reads=fixture.reads;
    for (uint32_t i=0;i<3;++i) {
        CHECK(xdap_detached_poll(fixture.target,1000,&ended)==XR_XIR_OK && !ended && !fixture.length);
        CHECK(fixture.target->wait_wake==wake && fixture.target->wait_epoch==epoch);
    }
    CHECK(fixture.reads==reads+3);++fixture.now;
    detach_finish(&fixture,"timer done\n",11,XR_XIR_CALL_RETURNED);detach_close(&fixture);
}
static void detach_binary_and_cleanup(bool cancel,bool output_failure) {
    DetachFixture fixture;detach_open(&fixture,XR_DAP_FIXTURE,false);
    request(fixture.adapter,detach_request);detach_adapter_drop(&fixture);
    fixture.fail_output=output_failure;
    if (!cancel) detach_finish(&fixture,output_failure ? "" : "owned\0value\n",output_failure ? 0 : 12,
        output_failure ? XR_XIR_CALL_OUTPUT_ERROR : XR_XIR_CALL_RETURNED);
    if (output_failure) {
        CHECK(xdap_detached_free(fixture.target)==XR_XIR_CALL_OUTPUT_ERROR);fixture.target=NULL;
    }
    detach_close(&fixture);
}
static void detach_active_cleanup(bool timer) {
    DetachFixture fixture;detach_open(&fixture,timer ? XR_DAP_TIMER_FIXTURE : XR_DAP_THREADS_FIXTURE,false);
    detach_start(&fixture,!timer,false);
    if (timer) CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && xdap_adapter_waiting(fixture.adapter));
    request(fixture.adapter,detach_request);detach_adapter_drop(&fixture);
    uint32_t reads=fixture.reads;fixture.fail_clock=true;
    CHECK(xdap_detached_free(fixture.target)==XR_XIR_CALL_READY);fixture.target=NULL;
    CHECK(!fixture.length && fixture.reads==reads);detach_close(&fixture);
}
static void detach_sequence_budget(void) {
    for (uint32_t mode=0;mode<2;++mode) {
        DetachFixture fixture;detach_open(&fixture,XR_DAP_THREADS_FIXTURE,false);detach_start(&fixture,true,false);
        XdapAdapter *adapter=fixture.adapter;adapter->seq=INT32_MAX-mode;capture.count=0;
        CHECK(xdap_adapter_request(adapter,detach_request,sizeof(detach_request)-1)==XR_XIR_BUDGET);
        CHECK(!fixture.target && !adapter->closed && adapter->launch && adapter->running && !capture.count);
        CHECK(xdap_adapter_step(adapter,16)==XR_XIR_OK && !fixture.target && adapter->running && !adapter->snapshot);
        detach_close(&fixture);
    }
}
static void detach_default_policy(uint32_t mode) {
    DetachFixture fixture;detach_open(&fixture,XR_DAP_THREADS_FIXTURE,mode!=0);detach_start(&fixture,true,false);
    capture.count=0;
    const char *message=mode==2 ?
        "{\"seq\":8,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{\"terminateDebuggee\":true}}" :
        "{\"seq\":8,\"type\":\"request\",\"command\":\"disconnect\"}";
    request(fixture.adapter,message);XrJsonValue *reply=last_response("disconnect",true);xjson_free(reply);
    CHECK(event_count("terminated")==1 && !event_count("exited"));
    if (mode==1) { CHECK(fixture.target);detach_adapter_drop(&fixture);detach_finish(&fixture,"33\n",3,XR_XIR_CALL_RETURNED); }
    else {
        CHECK(!fixture.target && fixture.adapter->closed);
        XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture.adapter->launch->control,1,&result)==XR_XIR_VALUE_OK);
        CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_CANCELLED);
        xdap_current_result_drop(&result);
    }
    detach_close(&fixture);
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    for (uint32_t paused=0;paused<2;++paused) {
        size_t sites=detach_children(paused!=0,SIZE_MAX,false,false);CHECK(sites);
        if (resources) for (size_t i=0;i<sites;++i) detach_children(paused!=0,i,false,false);
        else { detach_children(paused!=0,0,false,false);if (sites>1) detach_children(paused!=0,sites-1,false,false); }
        printf("Actual root/two-child detach paused=%u allocation sites=%zu injection=%s; adapter/compiler gone, final33/physical0 PASS\n",
            paused,sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
    detach_children(true,SIZE_MAX,1,true);detach_children(true,SIZE_MAX,2,true);detach_children(false,SIZE_MAX,0,true);
    detach_timer(false);detach_timer(true);
    detach_binary_and_cleanup(false,false);detach_binary_and_cleanup(true,false);detach_binary_and_cleanup(false,true);
    detach_active_cleanup(false);detach_active_cleanup(true);
    detach_sequence_budget();for (uint32_t mode=0;mode<3;++mode) detach_default_policy(mode);
    puts("Independent detach owner/output/wait, real pending timer and unstarted Source, retained NUL/output faults, IO handoff, budget, reentry and escaped snapshot PASS");
    return 0;
}
