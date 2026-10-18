/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_loop.c - Original loop visits through current scalar snapshot storage
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
typedef struct LoopFixture {
    XrCompileResources *ledger;
    XrCompileResourceStats baseline;
    XrXirDomain *domain;
    XdapAdapter *adapter;
    uint32_t function,at,index_slot,sum_slot;
} LoopFixture;
static void loop_point(LoopFixture *fixture,XrXirVmDebugHitKind kind,uint64_t hit) {
    XdapLaunch *launch=fixture->adapter->launch;XdapFunctionSelection selection={.name="accumulate",.length=10};
    XrXirVmDebugRule rules[32];uint32_t count=0;
    CHECK(xdap_launch_function_rules(launch,&selection,rules,32,&count)==XR_XIR_OK && count==1);
    fixture->function=rules[0].location.function;fixture->index_slot=fixture->sum_slot=fixture->at=UINT32_MAX;
    const XdapCodeFunction *function=&launch->functions[fixture->function];
    for (uint32_t i=0;i<function->instructions;++i) {
        const XrXirInstruction *op=&launch->instructions[function->first+i];
        if (op->op!=XR_XIR_LT_INT) continue;
        CHECK(fixture->index_slot==UINT32_MAX && op->args[0]>=function->parameters);
        uint32_t read=op->args[0]-function->parameters;CHECK(read<function->instructions);
        const XrXirInstruction *left=&launch->instructions[function->first+read];
        CHECK(left->op==XR_XIR_CELL_READ);fixture->index_slot=left->args[0];
    }
    CHECK(fixture->index_slot!=UINT32_MAX);
    for (uint32_t i=0;i<function->instructions;++i) {
        const XrXirInstruction *op=&launch->instructions[function->first+i];
        if (op->op!=XR_XIR_CELL_WRITE || op->args[0]==fixture->index_slot) continue;
        CHECK(fixture->at==UINT32_MAX);fixture->at=i;fixture->sum_slot=op->args[0];
    }
    CHECK(fixture->sum_slot!=UINT32_MAX);
    XrXirVmDebugRule rule={.location={fixture->function,fixture->at},.hit_kind=kind,.hit_value=hit};
    CHECK(xdap_current_rules_replace(launch->control,&rule,1)==XR_XIR_VALUE_OK);
}
static void loop_open(LoopFixture *fixture,XrXirVmDebugHitKind kind,uint64_t hit) {
    *fixture=(LoopFixture){0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
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
    CHECK(xdap_adapter_new(&compiler,&config,&fixture->adapter)==XR_XIR_OK && !compiler);
    capture.adapter=fixture->adapter;capture.reentry=true;
    request(fixture->adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    request(fixture->adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_LOOP_FIXTURE "\"}}");
    XrJsonValue *reply=last_response("launch",true);xjson_free(reply);loop_point(fixture,kind,hit);
    request(fixture->adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\"}");
}
static void loop_close(LoopFixture *fixture) {
    XrXirCallStatus status=xdap_adapter_free(fixture->adapter);CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_OOM);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static XrXirVmDebugSnapshot *loop_values(LoopFixture *fixture,int64_t index,int64_t sum) {
    XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(fixture->adapter->launch->control,&snapshot)==XR_XIR_VALUE_OK);
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&info,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    CHECK(info.function==fixture->function && info.instruction==fixture->at);
    const uint32_t slots[]={fixture->index_slot,fixture->sum_slot};const int64_t expected[]={index,sum};
    for (uint32_t i=0;i<2;++i) {
        XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(snapshot,0,slots[i],&value)==XR_XIR_VALUE_OK);
        CHECK(value.type==XR_XIR_I64 && value.payload==expected[i]);xr_xir_value_drop(&value);
    }
    return snapshot;
}
static void loop_case(XrXirVmDebugHitKind kind,uint64_t hit,const int64_t *indices,uint32_t count) {
    LoopFixture fixture;loop_open(&fixture,kind,hit);XdapAdapter *adapter=fixture.adapter;
    const int64_t sums[]={0,0,1,3};XrXirVmDebugSnapshot *snapshots[4]={0};uint64_t frame=0;
    for (uint32_t visit=0;visit<count;++visit) {
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==visit+1);
        CHECK(!adapter->running && !adapter->terminal);
        snapshots[visit]=loop_values(&fixture,indices[visit],sums[indices[visit]]);
        XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
        CHECK(xr_xir_vm_debug_snapshot_frame(snapshots[visit],0,&info,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
        if (!visit) frame=info.id;else CHECK(info.id==frame);
        uint64_t epoch=adapter->stop_epoch;
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==visit+1 && adapter->stop_epoch==epoch);
        request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    }
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && adapter->outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(event_count("stopped")==count && event_count("output")==1 && event_count("terminated")==1);
    bool output=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
        if (event && !strcmp(event,"output")) { CHECK(!strcmp(xjson_get_string(xjson_get_object(message,"body"),"output"),"6\n"));output=true; }
        xjson_free(message);
    }
    CHECK(output);CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);fixture.adapter=NULL;
    for (uint32_t visit=0;visit<count;++visit) {
        XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(snapshots[visit],0,fixture.sum_slot,&value)==XR_XIR_VALUE_OK);
        CHECK(value.type==XR_XIR_I64 && value.payload==sums[indices[visit]]);xr_xir_value_drop(&value);xr_xir_vm_debug_snapshot_drop(snapshots[visit]);
    }
    loop_close(&fixture);
}
static size_t loop_fault(size_t point) {
    LoopFixture fixture;loop_open(&fixture,XR_XIR_VM_DEBUG_HIT_ALWAYS,0);XdapAdapter *adapter=fixture.adapter;
    XrXirVmDebugSnapshot *old[2]={0};
    for (uint32_t i=0;i<2;++i) {
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==i+1);
        old[i]=loop_values(&fixture,i,0);
        if (!i) request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    }
    uint64_t epoch=adapter->stop_epoch;
    CHECK(xdap_current_continue(adapter->launch->control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(fixture.domain);size_t begin=runtime_attempts;
    runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;XdapCurrentResult result={0};
    CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(point==SIZE_MAX || sites>point);
    if (point==SIZE_MAX) {
        CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch>epoch);
        XrXirVmDebugSnapshot *third=loop_values(&fixture,2,1);xr_xir_vm_debug_snapshot_drop(third);
    } else CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM && !result.outcome.value.type && !result.debug_epoch);
    CHECK(adapter->snapshot==old[1] && adapter->stop_epoch==epoch);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture.domain);
    CHECK(after.work>=before.work && after.requested_bytes>=before.requested_bytes);
    for (uint32_t i=0;i<2;++i) {
        XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(old[i],0,fixture.index_slot,&value)==XR_XIR_VALUE_OK);
        CHECK(value.type==XR_XIR_I64 && value.payload==i);xr_xir_value_drop(&value);xr_xir_vm_debug_snapshot_drop(old[i]);
    }
    xdap_current_result_drop(&result);loop_close(&fixture);return sites;
}
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    xr_test_suppress_dialogs();const int64_t all[]={0,1,2,3},third[]={2},after_third[]={2,3},even[]={1,3};
    loop_case(XR_XIR_VM_DEBUG_HIT_ALWAYS,0,all,4);loop_case(XR_XIR_VM_DEBUG_HIT_EQUAL,3,third,1);
    loop_case(XR_XIR_VM_DEBUG_HIT_GREATER_EQUAL,3,after_third,2);loop_case(XR_XIR_VM_DEBUG_HIT_MULTIPLE,2,even,2);
    size_t sites=loop_fault(SIZE_MAX);CHECK(sites);if (resources) for (size_t i=0;i<sites;++i) loop_fault(i);
    printf("Original loop third-capture full FI: %zu allocation sites, injection=%s; original metadata work/owned prior stops/physical0\n",sites,resources ? "ALL" : "NOT_RUN");
    puts("Original Source loop index/sum0,0/1,0/2,1/3,3, hit3, stable frames, frozen scalar stops and stdout6: PASS");return 0;
}
