/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_write.c - Actual paused Call authority and prepared snapshot writes
 */
#define main reference_write_dispatch_main
#include "test_dispatch.c"
#undef main
#include "xir/xxir_vm_debug_internal.h"
#include "app/dap/xdap_dispatch_internal.h"
typedef struct WriteFixture { XdapAdapter *adapter;XrCompileResources *ledger;XrXirDomain *domain;XrCompileResourceStats baseline; } WriteFixture;
static XrXirStatus write_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;
    message->size=length;++out->seq;return XR_XIR_OK;
}
static WriteFixture write_open(const char *file) {
    WriteFixture fixture={0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture.ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture.ledger,&fixture.baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture.ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture.domain,.write=write_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);
    request(fixture.adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    request(fixture.adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);capture.count=0;return fixture;
}
static void write_finish(WriteFixture *fixture) {
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void write_dispose(WriteFixture *fixture) { CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);write_finish(fixture); }
#include "app/dap/xdap_lexical.h"
static void write_pause(WriteFixture *fixture) {
    request(fixture->adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_CALL_FIXTURE "\"},\"breakpoints\":[{\"line\":4}]}}");
    request(fixture->adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(fixture->adapter,1000)==XR_XIR_OK && fixture->adapter->snapshot && !fixture->adapter->running);capture.count=0;
}
static uint32_t write_operand(XdapAdapter *adapter,const XrXirVmDebugSnapshot *snapshot,const char *name,int64_t expected) {
    XdapLexicalView lexical={0};CHECK(xdap_lexical_open(adapter->launch,snapshot,0,&lexical)==XR_XIR_OK);
    for (uint32_t b=lexical.environment;b;b=lexical.function->bindings[b-1].parent) {
        const XrXirSourceDebugBinding *binding=&lexical.function->bindings[b-1];
        const char *text=lexical.source->declarations[binding->declaration-1].name;
        if (strcmp(text,name)) continue;
        XrXirValue value={0};const char *read_name=NULL;size_t length=0;
        CHECK(xdap_lexical_read(&lexical,b,&value,&read_name,&length)==XR_XIR_VALUE_OK);
        CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==expected);xr_xir_value_drop(&value);return binding->value;
    }
    CHECK(false);return UINT32_MAX;
}
static XrXirVmDebugFrameInfo write_frame(const XrXirVmDebugSnapshot *snapshot) {
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&info,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);return info;
}
static void write_return(WriteFixture *fixture,uint64_t epoch,const char *expected) {
    CHECK(xdap_current_continue(fixture->adapter->launch->control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(fixture->adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK);
    CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED);xdap_current_result_drop(&result);
    bool output=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
        if (event && !strcmp(event,"output")) { CHECK(!output && !strcmp(xjson_get_string(xjson_get_object(message,"body"),"output"),expected));output=true; }
        xjson_free(message);
    }
    CHECK(output);
}
static void write_normal(void) {
    WriteFixture fixture=write_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;XdapCurrent *control=adapter->launch->control;
    XrXirValue replacement={XR_XIR_I64,0,100};CHECK(xdap_current_write_begin(control,1,1,0,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
    write_pause(&fixture);XrXirVmDebugFrameInfo original=write_frame(adapter->snapshot);
    uint32_t operand=write_operand(adapter,adapter->snapshot,"doubled",40);uint64_t epoch=adapter->stop_epoch;
    CHECK(xdap_current_write_begin(control,epoch+1,original.id,operand,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_write_begin(control,epoch,UINT64_MAX,operand,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_write_begin(control,epoch,original.id,UINT32_MAX,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_write_begin(control,epoch,original.id,0,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
    XrXirValue bad={XR_XIR_BOOL,0,1};CHECK(xdap_current_write_begin(control,epoch,original.id,operand,&bad)==XR_XIR_VALUE_OK);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED && result.debug_epoch==epoch);
    bool visited=false;XrXirVmDebugSnapshot *fresh=NULL;
    CHECK(xdap_current_write_finish(control,&visited,&fresh)==XR_XIR_VALUE_BAD_ARGUMENT && visited && !fresh);xdap_current_result_drop(&result);
    write_operand(adapter,adapter->snapshot,"doubled",40);
    CHECK(xdap_current_write_begin(control,epoch,original.id,operand,&replacement)==XR_XIR_VALUE_OK);
    CHECK(xdap_current_write_begin(control,epoch,original.id,operand,&replacement)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_continue(control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(xdap_current_poll(control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED && result.debug_epoch==epoch+1);
    visited=false;CHECK(xdap_current_write_finish(control,&visited,&fresh)==XR_XIR_VALUE_OK && visited && fresh);
    XrXirVmDebugFrameInfo changed=write_frame(fresh);CHECK(changed.id==original.id && changed.instruction==original.instruction && changed.thread==original.thread && changed.epoch==epoch+1);
    write_operand(adapter,fresh,"doubled",100);write_operand(adapter,adapter->snapshot,"doubled",40);
    CHECK(xdap_current_continue(control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_BAD_ARGUMENT);
    xdap_current_result_drop(&result);write_return(&fixture,epoch+1,"104\n");
    XrXirVmDebugSnapshot *old=NULL;CHECK(xr_xir_vm_debug_snapshot_depth(adapter->snapshot)==3);
    CHECK(xdap_current_snapshot(control,&old)==XR_XIR_VALUE_BAD_ARGUMENT && !old);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    XrXirValue retained={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(fresh,0,operand,&retained)==XR_XIR_VALUE_OK && retained.payload==100);
    xr_xir_value_drop(&retained);xr_xir_vm_debug_snapshot_drop(fresh);write_finish(&fixture);
}
static void write_revoked(void) {
    for (uint32_t cancel=0;cancel<2;++cancel) {
        WriteFixture fixture=write_open(XR_DAP_CALL_FIXTURE);write_pause(&fixture);XdapAdapter *adapter=fixture.adapter;
        XrXirVmDebugFrameInfo frame=write_frame(adapter->snapshot);uint32_t operand=write_operand(adapter,adapter->snapshot,"doubled",40);
        XrXirValue replacement={XR_XIR_I64,0,100};CHECK(xdap_current_write_begin(adapter->launch->control,adapter->stop_epoch,frame.id,operand,&replacement)==XR_XIR_VALUE_OK);
        if (cancel) CHECK(xdap_current_cancel(adapter->launch->control)==XR_XIR_CALL_CANCEL_REQUESTED);
        bool visited=true;XrXirVmDebugSnapshot *fresh=NULL;
        CHECK(xdap_current_write_finish(adapter->launch->control,&visited,&fresh)==XR_XIR_VALUE_BAD_ARGUMENT && !visited && !fresh);
        write_operand(adapter,adapter->snapshot,"doubled",40);
        if (!cancel) write_return(&fixture,adapter->stop_epoch,"44\n");write_dispose(&fixture);
    }
}
static size_t write_fault(size_t fail) {
    WriteFixture fixture=write_open(XR_DAP_CALL_FIXTURE);write_pause(&fixture);XdapAdapter *adapter=fixture.adapter;
    XrXirVmDebugFrameInfo frame=write_frame(adapter->snapshot);uint32_t operand=write_operand(adapter,adapter->snapshot,"doubled",40);
    XrXirValue replacement={XR_XIR_I64,0,100};uint64_t epoch=adapter->stop_epoch;
    CHECK(xdap_current_write_begin(adapter->launch->control,epoch,frame.id,operand,&replacement)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(fixture.domain);size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;XdapCurrentResult result={0};
    XrXirValueStatus driver=xdap_current_poll(adapter->launch->control,1,&result);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    bool visited=false;XrXirVmDebugSnapshot *fresh=NULL;XrXirValueStatus status=xdap_current_write_finish(adapter->launch->control,&visited,&fresh);
    CHECK(driver==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED && visited && !capture.count);
    if (fail==SIZE_MAX) { CHECK(status==XR_XIR_VALUE_OK && fresh && result.debug_epoch==epoch+1);write_operand(adapter,fresh,"doubled",100); }
    else {
        if (status!=XR_XIR_VALUE_OOM) fprintf(stderr,"write fault site=%zu status=%u sites=%zu\n",fail,status,sites);
        CHECK(status==XR_XIR_VALUE_OOM && sites>fail && !fresh && result.debug_epoch==epoch);
        CHECK(runtime_live==live && runtime_bytes==bytes);XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture.domain);
        CHECK(after.metadata_live==before.metadata_live && after.requested_bytes>=before.requested_bytes && after.work>before.work);
    }
    write_operand(adapter,adapter->snapshot,"doubled",40);xdap_current_result_drop(&result);
    if (fresh) xr_xir_vm_debug_snapshot_drop(fresh);write_return(&fixture,fail==SIZE_MAX ? epoch+1 : epoch,fail==SIZE_MAX ? "104\n" : "44\n");
    write_dispose(&fixture);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    write_normal();write_revoked();size_t sites=write_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t i=0;i<sites;++i) write_fault(i);else { write_fault(0);if (sites>1) write_fault(sites-1); }
    printf("Actual paused Call write100, unchanged PC, stale/readonly/type/abort/cancel checks, owned snapshots and actual104; sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
