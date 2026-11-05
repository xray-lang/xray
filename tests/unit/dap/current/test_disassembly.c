/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_disassembly.c - Numeric current-code addresses and atomic observations
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
typedef struct DisassemblyFixture {
    XrCompileResources *ledger;
    XrCompileResourceStats baseline;
    XrXirDomain *domain;
    XdapAdapter *adapter;
} DisassemblyFixture;
static void disassembly_open(DisassemblyFixture *fixture,const char *file) {
    *fixture=(DisassemblyFixture){0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture->ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture->ledger,&fixture->baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture->ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
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
    char text[2048];int size=snprintf(text,sizeof(text),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    CHECK(size>0 && (size_t)size<sizeof(text));request(fixture->adapter,text);
    XrJsonValue *reply=last_response("launch",true);xjson_free(reply);
}
static void disassembly_close(DisassemblyFixture *fixture) {
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static XrJsonValue *disassembly_request(DisassemblyFixture *fixture,const char *arguments,bool success) {
    char text[2048];int size=snprintf(text,sizeof(text),"{\"seq\":3,\"type\":\"request\",\"command\":\"disassemble\",\"arguments\":%s}",arguments);
    CHECK(size>0 && (size_t)size<sizeof(text));request(fixture->adapter,text);return last_response("disassemble",success);
}
static XrJsonValue *disassembly_page(DisassemblyFixture *fixture,uint32_t address,int64_t offset,uint32_t count,bool symbols) {
    char arguments[256];snprintf(arguments,sizeof(arguments),
        "{\"memoryReference\":\"%u\",\"instructionOffset\":%lld,\"instructionCount\":%u,\"resolveSymbols\":%s}",
        address,(long long)offset,count,symbols ? "true" : "false");
    return disassembly_request(fixture,arguments,true);
}
static XrJsonValue *disassembly_items(XrJsonValue *reply,uint32_t expected) {
    XrJsonValue *items=xjson_get_array(xjson_get_object(reply,"body"),"instructions");CHECK(xjson_array_len(items)==(int)expected);return items;
}
static void disassembly_rejections(DisassemblyFixture *fixture) {
    const char *bad[]={"{\"instructionCount\":1}","{\"memoryReference\":\"0\"}",
        "{\"memoryReference\":\"0\",\"instructionCount\":257}","{\"memoryReference\":\"0\",\"instructionCount\":1,\"resolveSymbols\":null}",
        "{\"memoryReference\":\"0\",\"instructionCount\":1,\"instructionOffset\":-257}",
        "{\"memoryReference\":\"0\",\"instructionCount\":1,\"instructionOffset\":257}",
        "{\"memoryReference\":\"0\",\"instructionCount\":1,\"offset\":-1}",
        "{\"memoryReference\":\"0\",\"instructionCount\":1,\"offset\":1}"};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        XrJsonValue *reply=disassembly_request(fixture,bad[i],false);xjson_free(reply);capture.count=0;
    }
    const char *references[]={"f4294967296:i0","f0:i0","","-1","+0"," 0","0 ","1junk","4294967296", "4294967295",
        "9999999999999999999999999999999999999999","0x","0x100000000","0xG","0X0"};
    for (size_t i=0;i<sizeof(references)/sizeof(*references);++i) {
        char arguments[256];snprintf(arguments,sizeof(arguments),"{\"memoryReference\":\"%s\",\"instructionCount\":1}",references[i]);
        XrJsonValue *reply=disassembly_request(fixture,arguments,false);xjson_free(reply);capture.count=0;
    }
    char past[128];snprintf(past,sizeof(past),"{\"memoryReference\":\"%u\",\"instructionCount\":1}",fixture->adapter->launch->instruction_count);
    XrJsonValue *reply=disassembly_request(fixture,past,false);xjson_free(reply);capture.count=0;
}
static void disassembly_metadata(DisassemblyFixture *fixture) {
    XdapLaunch *launch=fixture->adapter->launch;
    XrJsonValue *reply=disassembly_page(fixture,0,-1,3,false),*items=disassembly_items(reply,3);
    CHECK(!strcmp(xjson_get_string(xjson_array_get(items,0),"address"),"-1"));
    CHECK(!strcmp(xjson_get_string(xjson_array_get(items,0),"presentationHint"),"invalid"));
    CHECK(!strcmp(xjson_get_string(xjson_array_get(items,1),"address"),"0"));
    CHECK(!xjson_get(xjson_array_get(items,1),"presentationHint") && !xjson_get(xjson_array_get(items,1),"symbol"));
    xjson_free(reply);capture.count=0;
    reply=disassembly_page(fixture,launch->instruction_count-1,0,3,true);items=disassembly_items(reply,3);
    CHECK(!xjson_get(xjson_array_get(items,0),"presentationHint"));
    for (int i=1;i<3;++i) CHECK(!strcmp(xjson_get_string(xjson_array_get(items,i),"presentationHint"),"invalid"));
    xjson_free(reply);capture.count=0;
    CHECK(launch->function_count>1 && launch->functions[0].instructions && launch->functions[1].instructions);
    uint32_t boundary=launch->functions[1].first;CHECK(boundary && boundary==launch->functions[0].instructions);
    reply=disassembly_page(fixture,boundary-1,0,2,true);items=disassembly_items(reply,2);
    CHECK(!strcmp(xjson_get_string(xjson_array_get(items,0),"symbol"),launch->functions[0].name));
    CHECK(!strcmp(xjson_get_string(xjson_array_get(items,1),"symbol"),launch->functions[1].name));
    CHECK(!xjson_get(xjson_array_get(items,1),"presentationHint"));xjson_free(reply);capture.count=0;
    reply=disassembly_request(fixture,"{\"memoryReference\":\"0x0\",\"instructionCount\":1}",true);
    CHECK(!strcmp(xjson_get_string(xjson_array_get(disassembly_items(reply,1),0),"address"),"0"));xjson_free(reply);capture.count=0;
    reply=disassembly_page(fixture,0,0,0,false);disassembly_items(reply,0);xjson_free(reply);capture.count=0;
}
static void disassembly_stopped(DisassemblyFixture *fixture) {
    XdapAdapter *adapter=fixture->adapter;
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"leaf\"}]}}");
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
    XrXirVmDebugFrameInfo top={0};XrXirValue name={0};CHECK(xr_xir_vm_debug_snapshot_frame(adapter->snapshot,0,&top,&name)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&name);uint32_t address=adapter->launch->functions[top.function].first+top.instruction;
    request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    XrJsonValue *reply=last_response("stackTrace",true),*frame=xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"stackFrames"),0);
    char expected[32];snprintf(expected,sizeof(expected),"%u",address);
    CHECK(!strcmp(xjson_get_string(frame,"instructionPointerReference"),expected));xjson_free(reply);capture.count=0;
    reply=disassembly_page(fixture,address,0,1,true);frame=xjson_array_get(disassembly_items(reply,1),0);
    CHECK(!strcmp(xjson_get_string(frame,"address"),expected) && !strcmp(xjson_get_string(frame,"symbol"),"leaf"));
    CHECK(!strcmp(xjson_get_string(xjson_get_object(frame,"location"),"name"),"wire_fixture.xr"));
    CHECK(xjson_get_int(frame,"line")==2 && xjson_get_int(frame,"column")==9);
    CHECK(!xjson_get(frame,"instructionBytes"));xjson_free(reply);capture.count=0;
}
static void disassembly_faults(DisassemblyFixture *fixture,bool resources) {
    const char text[]="{\"seq\":7,\"type\":\"request\",\"command\":\"disassemble\",\"arguments\":{\"memoryReference\":\"0\",\"instructionCount\":32,\"resolveSymbols\":true}}";
    XdapAdapter *adapter=fixture->adapter;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    uint32_t next=adapter->next_handle,depth=adapter->depth;size_t sites=0;
    XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(fixture->domain);
    for (size_t point=SIZE_MAX;point==SIZE_MAX || (resources && point<sites);point=point==SIZE_MAX ? 0 : point+1) {
        XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&before)==XR_COMPILE_RESOURCE_OK);
        size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;uint32_t seq=adapter->seq;
        runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;XrXirStatus status=xdap_adapter_request(adapter,text,sizeof(text)-1);
        size_t attempts=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
        if (point==SIZE_MAX) { CHECK(status==XR_XIR_OK && capture.count==1);sites=attempts;CHECK(sites); }
        else { CHECK(attempts>point && status==XR_XIR_OUT_OF_MEMORY && !capture.count && adapter->seq==seq); }
        CHECK(runtime_live==live && runtime_bytes==bytes);
        CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes && after.work>before.work && after.allocated_bytes>=before.allocated_bytes);
        CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && adapter->next_handle==next && adapter->depth==depth && !adapter->value_count && !adapter->running);
        XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(fixture->domain);
        CHECK(spent.work==budget.work && spent.requested_bytes==budget.requested_bytes && spent.metadata_live==budget.metadata_live);
        capture.count=0;XrJsonValue *reply=disassembly_page(fixture,0,0,2,false);disassembly_items(reply,2);xjson_free(reply);capture.count=0;
    }
    printf("Current disassembly JSON full FI: %zu allocation sites, injection=%s; same owners and stopped target preserved\n",sites,resources ? "ALL" : "NOT_RUN");
    const char bounded[]="{\"seq\":8,\"type\":\"request\",\"command\":\"disassemble\",\"arguments\":{\"memoryReference\":\"0\",\"instructionOffset\":-256,\"instructionCount\":256}}";
    CHECK(xdap_adapter_request(adapter,bounded,sizeof(bounded)-1)==XR_XIR_BUDGET && !capture.count);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && adapter->next_handle==next);
    XrJsonValue *reply=disassembly_page(fixture,0,0,2,true);disassembly_items(reply,2);xjson_free(reply);capture.count=0;
}
static XrXirStatus disassembly_write_failure(void *context,const char *bytes,size_t length) {
    CHECK(context==&capture && bytes && length && !capture.count);return XR_XIR_IO;
}
static void disassembly_transport_failure(DisassemblyFixture *fixture) {
    const char text[]="{\"seq\":8,\"type\":\"request\",\"command\":\"disassemble\",\"arguments\":{\"memoryReference\":\"0\",\"instructionCount\":2}}";
    XdapAdapter *adapter=fixture->adapter;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;
    uint64_t epoch=adapter->stop_epoch;uint32_t seq=adapter->seq,next=adapter->next_handle;
    size_t live=runtime_live,bytes=runtime_bytes;adapter->config.write=disassembly_write_failure;
    CHECK(xdap_adapter_request(adapter,text,sizeof(text)-1)==XR_XIR_IO);adapter->config.write=capture_message;
    CHECK(runtime_live==live && runtime_bytes==bytes && adapter->snapshot==snapshot && adapter->stop_epoch==epoch);
    CHECK(adapter->seq==seq && adapter->next_handle==next && !adapter->running && !capture.count);
    XrJsonValue *reply=disassembly_page(fixture,0,0,2,true);disassembly_items(reply,2);xjson_free(reply);capture.count=0;
}
static void disassembly_branch(void) {
    DisassemblyFixture fixture;disassembly_open(&fixture,XR_DAP_LOOP_FIXTURE);XdapLaunch *launch=fixture.adapter->launch;bool found=false;
    for (uint32_t i=0;i<launch->instruction_count;++i) {
        const XrXirInstruction *op=&launch->instructions[i];if (op->op!=XR_XIR_BRANCH) continue;
        CHECK(op->targets[0] || op->targets[1]);XrJsonValue *reply=disassembly_page(&fixture,i,0,1,false);
        const char *text=xjson_get_string(xjson_array_get(disassembly_items(reply,1),0),"instruction");
        char expected[80];snprintf(expected,sizeof(expected),"targets=(%u,%u)",op->targets[0],op->targets[1]);
        CHECK(strstr(text,expected) && strstr(text,"type_arguments=(0,0)") && strstr(text,"BRANCH"));
        xjson_free(reply);capture.count=0;found=true;break;
    }
    CHECK(found);disassembly_close(&fixture);
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");
    bool regression=argc==2 && !strcmp(argv[1],"--overflow-regression");CHECK(argc==1 || resources || regression);
    DisassemblyFixture fixture;disassembly_open(&fixture,XR_DAP_FIXTURE);
    if (regression) {
        XrJsonValue *reply=disassembly_request(&fixture,"{\"memoryReference\":\"f4294967296:i0\",\"instructionCount\":1}",false);xjson_free(reply);
        disassembly_close(&fixture);return 0;
    }
    disassembly_rejections(&fixture);disassembly_metadata(&fixture);disassembly_stopped(&fixture);disassembly_faults(&fixture,resources);
    disassembly_transport_failure(&fixture);
    request(fixture.adapter,"{\"seq\":9,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && fixture.adapter->terminal);capture.count=0;
    XrJsonValue *reply=disassembly_page(&fixture,0,0,2,false);disassembly_items(reply,2);xjson_free(reply);
    disassembly_close(&fixture);disassembly_branch();
    puts("Numeric live instruction reference, invalid padding, cross-function symbols, real Source branch fields, bounded output and physical0: PASS");return 0;
}
