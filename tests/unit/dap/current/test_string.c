/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_string.c - Authentic append retains old stopped String bytes
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
typedef struct StringFixture {
    XrCompileResources *ledger;
    XrCompileResourceStats baseline;
    XrXirDomain *domain;
    XdapAdapter *adapter;
    uint32_t function,cell,points[3];
} StringFixture;
static void string_points(StringFixture *fixture) {
    XdapLaunch *launch=fixture->adapter->launch;XdapFunctionSelection selection={.name="inspect",.length=7};
    XrXirVmDebugRule rules[32];uint32_t count=0;
    CHECK(xdap_launch_function_rules(launch,&selection,rules,32,&count)==XR_XIR_OK && count==1);
    fixture->function=rules[0].location.function;fixture->cell=UINT32_MAX;
    const XdapCodeFunction *function=&launch->functions[fixture->function];uint32_t appends=0;bool printed=false;
    for (uint32_t i=0;i<function->instructions;++i) {
        const XrXirInstruction *op=&launch->instructions[function->first+i];
        if (op->op==XR_XIR_CELL_STRING_APPEND) {
            CHECK(appends<2 && op->immediate==(int64_t)appends);
            if (!appends) fixture->cell=op->args[0];else CHECK(fixture->cell==op->args[0]);
            fixture->points[appends++]=i;
        } else if ((op->op==XR_XIR_PRINT || op->op==XR_XIR_OUTPUT) && !printed) {
            fixture->points[2]=i;printed=true;
        }
    }
    CHECK(appends==2 && printed && fixture->points[2]>fixture->points[1]);
    XrXirVmDebugLocation points[3];for (uint32_t i=0;i<3;++i) points[i]=(XrXirVmDebugLocation){fixture->function,fixture->points[i]};
    CHECK(xdap_current_breakpoints_replace(launch->control,points,3)==XR_XIR_VALUE_OK);
}
static void string_open(StringFixture *fixture) {
    *fixture=(StringFixture){0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture->ledger,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture->ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture->domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture->domain,.write=capture_message,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture->adapter)==XR_XIR_OK && !compiler);capture.adapter=fixture->adapter;capture.reentry=true;
    request(fixture->adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    request(fixture->adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_STRING_FIXTURE "\"}}");
    XrJsonValue *reply=last_response("launch",true);xjson_free(reply);string_points(fixture);
    request(fixture->adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\"}");
}
static void string_bytes(const XrXirValue *value,const char *expected,size_t length) {
    const char *bytes=NULL;size_t size=0;CHECK(xr_xir_string_view(value,&bytes,&size) && size==length && !memcmp(bytes,expected,length));
}
static void string_close(StringFixture *fixture) {
    XrXirCallStatus status=xdap_adapter_free(fixture->adapter);CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_OOM);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void string_case(bool cancel) {
    StringFixture fixture;string_open(&fixture);XdapAdapter *adapter=fixture.adapter;
    XrXirVmDebugSnapshot *snapshots[3]={0};XrXirValue values[3]={{0}};
    const char *expected[]={"a\0","a\0\xce\xa9","a\0\xce\xa9" "7"};const size_t lengths[]={2,4,5};
    for (uint32_t i=0;i<3;++i) {
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==i+1);
        CHECK(xdap_current_snapshot(adapter->launch->control,&snapshots[i])==XR_XIR_VALUE_OK);
        CHECK(xr_xir_vm_debug_snapshot_cell_value(snapshots[i],0,fixture.cell,&values[i])==XR_XIR_VALUE_OK);
        string_bytes(&values[i],expected[i],lengths[i]);
        for (uint32_t j=0;j<i;++j) { CHECK(values[j].payload!=values[i].payload);string_bytes(&values[j],expected[j],lengths[j]); }
        if (cancel && i==2) request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"terminate\"}");
        else request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    }
    if (!cancel) {
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && adapter->outcome.status==XR_XIR_CALL_RETURNED);
        char bytes[32]={0};size_t used=0;
        for (uint32_t i=0;i<capture.count;++i) {
            XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
            if (event && !strcmp(event,"output")) { XrJsonValue *text=xjson_get(xjson_get_object(message,"body"),"output");
                CHECK(used+text->string_len<=sizeof(bytes));memcpy(bytes+used,text->as.string,text->string_len);used+=text->string_len; }
            xjson_free(message);
        }
        CHECK(used==9 && !memcmp(bytes,"a\0\xce\xa9" "7\na\0\n",9));
    } else CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("output")==0 && adapter->terminal &&
        adapter->outcome.status==XR_XIR_CALL_CANCELLED && event_count("terminated")==1);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);fixture.adapter=NULL;
    for (uint32_t i=0;i<3;++i) {
        string_bytes(&values[i],expected[i],lengths[i]);XrXirValue copied={0};
        CHECK(xr_xir_vm_debug_snapshot_cell_value(snapshots[i],0,fixture.cell,&copied)==XR_XIR_VALUE_OK);
        string_bytes(&copied,expected[i],lengths[i]);xr_xir_value_drop(&copied);xr_xir_value_drop(&values[i]);xr_xir_vm_debug_snapshot_drop(snapshots[i]);
    }
    string_close(&fixture);
}
static size_t string_fault(uint32_t which,size_t point) {
    StringFixture fixture;string_open(&fixture);XdapAdapter *adapter=fixture.adapter;
    XrXirVmDebugSnapshot *old[2]={0};XrXirValue values[2]={{0}};
    const char *expected[]={"a\0","a\0\xce\xa9"};const size_t lengths[]={2,4};
    for (uint32_t i=0;i<=which;++i) {
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==i+1);
        CHECK(xdap_current_snapshot(adapter->launch->control,&old[i])==XR_XIR_VALUE_OK);
        CHECK(xr_xir_vm_debug_snapshot_cell_value(old[i],0,fixture.cell,&values[i])==XR_XIR_VALUE_OK);
        string_bytes(&values[i],expected[i],lengths[i]);
        if (i<which) request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    }
    uint64_t epoch=adapter->stop_epoch;
    CHECK(xdap_current_continue(adapter->launch->control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(fixture.domain);size_t begin=runtime_attempts;
    runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;XdapCurrentResult result={0};
    CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(point==SIZE_MAX || sites>point);
    if (point==SIZE_MAX) CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch>epoch);
    else CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM && !result.outcome.value.type && !result.debug_epoch);
    CHECK(adapter->snapshot==old[which] && adapter->stop_epoch==epoch);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture.domain);
    CHECK(after.work>=before.work && after.requested_bytes>=before.requested_bytes);
    for (uint32_t i=0;i<=which;++i) {
        string_bytes(&values[i],expected[i],lengths[i]);xr_xir_value_drop(&values[i]);xr_xir_vm_debug_snapshot_drop(old[i]);
    }
    xdap_current_result_drop(&result);string_close(&fixture);return sites;
}
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    xr_test_suppress_dialogs();string_case(false);string_case(true);
    for (uint32_t which=0;which<2;++which) {
        size_t sites=string_fault(which,SIZE_MAX);CHECK(sites);if (resources) for (size_t i=0;i<sites;++i) string_fault(which,i);
        printf("Stopped String append%u/next-capture: %zu sites, injection=%s; old bytes/epoch/original work/physical0\n",which,sites,resources ? "ALL" : "NOT_RUN");
    }
    puts("Authenticated String/scalar append, old stopped NUL/Unicode bytes, current output and cancellation physical0/0: PASS");return 0;
}
