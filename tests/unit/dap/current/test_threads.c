/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_threads.c - Live authenticated child metadata without stopped values
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
typedef struct ThreadsFixture {
    XrCompileResources *ledger;
    XrCompileResourceStats baseline;
    XrXirDomain *domain;
    XdapAdapter *adapter;
    uint32_t rejected_reentry;
} ThreadsFixture;
static XrXirStatus threads_capture(void *context,const char *bytes,size_t length) {
    ThreadsFixture *fixture=context;
    const char marker[]="\"event\":\"output\"";bool output=false;
    for (size_t i=0;i+sizeof(marker)-1<=length;++i) if (!memcmp(bytes+i,marker,sizeof(marker)-1)) output=true;
    if (output) {
        uint64_t untouched=123;uint32_t count=77;
        CHECK(xdap_current_threads(fixture->adapter->launch->control,&untouched,1,&count)==XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(untouched==123 && count==77);++fixture->rejected_reentry;
    }
    return capture_message(&capture,bytes,length);
}
static void threads_open(ThreadsFixture *fixture) {
    *fixture=(ThreadsFixture){0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture->ledger,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture->ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture->domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture->domain,.write=threads_capture,.context=fixture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture->adapter)==XR_XIR_OK && !compiler);
    capture.adapter=fixture->adapter;capture.reentry=true;
    request(fixture->adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    request(fixture->adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_THREADS_FIXTURE "\"}}");
    XrJsonValue *reply=last_response("launch",true);xjson_free(reply);
}
static void threads_close(ThreadsFixture *fixture) {
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void threads_protocol(ThreadsFixture *fixture,const uint64_t *expected,uint32_t count) {
    request(fixture->adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"threads\"}");
    XrJsonValue *reply=last_response("threads",true),*threads=xjson_get_array(xjson_get_object(reply,"body"),"threads");
    CHECK(xjson_array_len(threads)==(int)count);
    for (uint32_t i=0;i<count;++i) CHECK((uint64_t)xjson_get_int(xjson_array_get(threads,(int)i),"id")==expected[i]);
    xjson_free(reply);
}
static void threads_request_faults(ThreadsFixture *fixture,const uint64_t *expected,bool resources) {
    const char text[]="{\"seq\":8,\"type\":\"request\",\"command\":\"threads\"}";
    size_t sites=0;
    for (size_t point=SIZE_MAX;point==SIZE_MAX || (resources && point<sites);point=point==SIZE_MAX ? 0 : point+1) {
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(fixture->ledger,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(fixture->domain);
        size_t begin=runtime_attempts;runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;
        XrXirStatus status=xdap_adapter_request(fixture->adapter,text,sizeof(text)-1);
        size_t attempts=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
        if (point==SIZE_MAX) { CHECK(status==XR_XIR_OK);sites=attempts;CHECK(sites); }
        else { CHECK(attempts>point && status==XR_XIR_OUT_OF_MEMORY); }
        CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes && after.allocated_bytes>=before.allocated_bytes && after.work>=before.work);
        XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(fixture->domain);
        CHECK(spent.metadata_live==budget.metadata_live && spent.requested_bytes==budget.requested_bytes && spent.work>=budget.work);
        CHECK(fixture->adapter->running && !fixture->adapter->snapshot && !fixture->adapter->stop_epoch && !fixture->adapter->value_count);
        capture.count=0;threads_protocol(fixture,expected,3);capture.count=0;
    }
    printf("Running threads JSON full FI: %zu allocation sites, injection=%s; original owners retained\n",sites,resources ? "ALL" : "NOT_RUN");
}
static void threads_case(uint32_t mode,bool resources) {
    ThreadsFixture fixture;threads_open(&fixture);XdapAdapter *adapter=fixture.adapter;
    uint64_t ids[64]={0};uint32_t count=77;
    CHECK(xdap_current_threads(adapter->launch->control,ids,64,&count)==XR_XIR_VALUE_OK && !count);threads_protocol(&fixture,NULL,0);
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_current_threads(adapter->launch->control,ids,64,&count)==XR_XIR_VALUE_OK && count==1 && ids[0]==1);
    threads_protocol(&fixture,ids,1);bool found=false;
    for (uint32_t step=0;step<10000 && !adapter->terminal;++step) {
        CHECK(xdap_adapter_step(adapter,1)==XR_XIR_OK);
        XrXirValueStatus status=xdap_current_threads(adapter->launch->control,ids,64,&count);
        if (status==XR_XIR_VALUE_UNSUPPORTED) continue;
        CHECK(status==XR_XIR_VALUE_OK);if (count<3) continue;
        CHECK(count==3 && ids[0]==1 && ids[1]>1 && ids[2]>ids[1]);found=true;break;
    }
    CHECK(found && adapter->running && !adapter->snapshot && !event_count("stopped"));threads_protocol(&fixture,ids,3);
    uint64_t untouched=UINT64_C(0x11223344);uint32_t untouched_count=77;
    CHECK(xdap_current_threads(adapter->launch->control,&untouched,1,&untouched_count)==XR_XIR_VALUE_LIMIT);
    CHECK(untouched==UINT64_C(0x11223344) && untouched_count==77);
    CHECK(xdap_current_threads(adapter->launch->control,NULL,1,&untouched_count)==XR_XIR_VALUE_BAD_ARGUMENT && untouched_count==77);
    CHECK(xdap_current_threads(adapter->launch->control,NULL,0,&untouched_count)==XR_XIR_VALUE_LIMIT && untouched_count==77);
    CHECK(xdap_current_threads(adapter->launch->control,&untouched,1026,&untouched_count)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(untouched==UINT64_C(0x11223344) && untouched_count==77);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(fixture.domain);size_t attempts=runtime_attempts;
    CHECK(xdap_current_threads(adapter->launch->control,ids,64,&count)==XR_XIR_VALUE_OK && count==3 && runtime_attempts==attempts);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture.domain);
    CHECK(after.work>before.work && after.metadata_live==before.metadata_live && after.requested_bytes==before.requested_bytes);
    if (!mode) threads_request_faults(&fixture,ids,resources);
    if (!mode) {
        request(adapter,"{\"seq\":9,\"type\":\"request\",\"command\":\"pause\",\"arguments\":{\"threadId\":1}}");
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
        XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
        threads_protocol(&fixture,ids,3);threads_protocol(&fixture,ids,3);
        CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running && !adapter->value_count);
        request(adapter,"{\"seq\":10,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
        CHECK(adapter->running && !adapter->snapshot);threads_protocol(&fixture,ids,3);
    }
    if (mode==2) {
        CHECK(after.work<128000000 && xr_xir_domain_work(fixture.domain,128000000-after.work));
        for (uint32_t i=0;i<2;++i) {
            untouched=UINT64_C(0x11223344);untouched_count=77;
            CHECK(xdap_current_threads(adapter->launch->control,&untouched,1,&untouched_count)==XR_XIR_VALUE_LIMIT);
            CHECK(untouched==UINT64_C(0x11223344) && untouched_count==77);
        }
        threads_close(&fixture);return;
    }
    if (mode==1) request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"terminate\"}");
    for (uint32_t step=0;step<10000 && !adapter->terminal;++step) CHECK(xdap_adapter_step(adapter,1)==XR_XIR_OK);
    CHECK(adapter->terminal && adapter->outcome.status==(mode==1 ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
    CHECK(xdap_current_threads(adapter->launch->control,ids,64,&count)==XR_XIR_VALUE_OK && !count);
    threads_protocol(&fixture,NULL,0);CHECK(event_count("output")==(mode==1 ? 0u : 1u) && event_count("terminated")==1);
    CHECK(fixture.rejected_reentry==(mode==1 ? 0u : 1u));
    if (!mode) {
        bool output=false;
        for (uint32_t i=0;i<capture.count;++i) {
            XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
            if (event && !strcmp(event,"output")) { CHECK(!strcmp(xjson_get_string(xjson_get_object(message,"body"),"output"),"33\n"));output=true; }
            xjson_free(message);
        }
        CHECK(output);
    }
    threads_close(&fixture);
}
#include "test_threads_pause.inc.c"
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    xr_test_suppress_dialogs();threads_case(0,resources);threads_case(1,false);threads_case(2,false);
    size_t sites=threads_pause_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t point=0;point<sites;++point) threads_pause_fault(point);
    else { threads_pause_fault(0);if (sites>1) threads_pause_fault(sites-1); }
    printf("Child all-stop pause: response allocation sites=%zu injection=%s; failure never schedules a pause PASS\n",
        sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    puts("Running root/two real child identities, no stopped values, capacity rollback, completion/cancel retirement and physical0: PASS");return 0;
}
