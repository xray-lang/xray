/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_cow.c - Paused ownership across actual array copy-on-write and teardown
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
typedef struct CowFixture {
    XrCompileResources *ledger;
    XrCompileResourceStats baseline;
    XrXirDomain *domain;
    XdapAdapter *adapter;
    uint32_t function,write_at,output_at;
} CowFixture;
static void cow_rules(CowFixture *fixture) {
    XdapLaunch *launch=fixture->adapter->launch;XdapFunctionSelection selection={.name="inspect",.length=7};
    XrXirVmDebugRule rule[32];uint32_t count=0;
    CHECK(xdap_launch_function_rules(launch,&selection,rule,32,&count)==XR_XIR_OK && count==1);
    fixture->function=rule[0].location.function;fixture->write_at=fixture->output_at=UINT32_MAX;
    const XdapCodeFunction *function=&launch->functions[fixture->function];
    for (uint32_t i=0;i<function->instructions;++i) {
        XrXirOp op=launch->instructions[function->first+i].op;
        if (op==XR_XIR_ARRAY_SET) { CHECK(fixture->write_at==UINT32_MAX);fixture->write_at=i; }
        if ((op==XR_XIR_OUTPUT || op==XR_XIR_PRINT) && fixture->output_at==UINT32_MAX) fixture->output_at=i;
    }
    if (fixture->write_at==UINT32_MAX || fixture->output_at==UINT32_MAX)
        fprintf(stderr,"CoW point selection: function=%u write=%u output=%u\n",fixture->function,fixture->write_at,fixture->output_at);
    CHECK(fixture->write_at!=UINT32_MAX && fixture->output_at>fixture->write_at && fixture->output_at!=UINT32_MAX);
    XrXirVmDebugLocation locations[]={{fixture->function,fixture->write_at},{fixture->function,fixture->output_at}};
    CHECK(xdap_current_breakpoints_replace(launch->control,locations,2)==XR_XIR_VALUE_OK);
}
static void cow_open(CowFixture *fixture) {
    *fixture=(CowFixture){0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
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
    request(fixture->adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"supportsVariableType\":true}}");
    request(fixture->adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_COW_FIXTURE "\"}}");
    XrJsonValue *reply=last_response("launch",true);xjson_free(reply);cow_rules(fixture);
    request(fixture->adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(fixture->adapter,1000)==XR_XIR_OK && event_count("stopped")==1);
}
static void cow_close(CowFixture *fixture) {
    XrXirCallStatus status=xdap_adapter_free(fixture->adapter);CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_OOM);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void cow_array(const XdapValueView *view,int64_t middle) {
    XdapValueShape shape={0};CHECK(xdap_value_shape(view,&shape)==XR_XIR_VALUE_OK && shape.indexed && shape.children==3);
    const int64_t expected[]={10,middle,30};
    for (uint32_t i=0;i<3;++i) {
        XrXirValue value={0};XdapValueChildInfo info={0};
        CHECK(xdap_value_child(view,i,&value,&info)==XR_XIR_VALUE_OK);
        CHECK(value.type==XR_XIR_I64 && value.payload==expected[i] && !info.name && !info.name_length);
        CHECK(info.selected && info.selector.kind==XR_XIR_PATH_INDEX && info.selector.selector==(int64_t)i);xr_xir_value_drop(&value);
    }
}
static uint32_t cow_values(CowFixture *fixture,XrXirVmDebugSnapshot *snapshot,XrXirValue arrays[2],XrXirValue *text) {
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0};uint32_t count=0;
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,0,&info,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    CHECK(info.function==fixture->function);
    const XdapCodeFunction *function=&fixture->adapter->launch->functions[fixture->function];uint32_t places=0;
    for (uint32_t i=0;i<fixture->write_at;++i) {
        XrXirOp op=fixture->adapter->launch->instructions[function->first+i].op;
        if (op!=XR_XIR_CELL_PLACE && op!=XR_XIR_SLOT_PLACE && op!=XR_XIR_FIELD_PLACE &&
            op!=XR_XIR_INDEX_PLACE && op!=XR_XIR_OBJECT_PLACE) continue;
        XrXirValue rejected={0};
        CHECK(xr_xir_vm_debug_snapshot_value(snapshot,0,function->parameters+i,&rejected)==XR_XIR_VALUE_BAD_ARGUMENT);
        CHECK(!rejected.type && !rejected.reserved && !rejected.payload);++places;
    }
    CHECK(places);
    for (uint32_t i=0;i<info.value_count;++i) {
        XrXirValue value={0};XrXirValueStatus status=xr_xir_vm_debug_snapshot_value(snapshot,0,i,&value);
        if (status==XR_XIR_VALUE_BAD_ARGUMENT) continue;CHECK(status==XR_XIR_VALUE_OK);
        XrXirValue cell={0};status=xr_xir_vm_debug_snapshot_cell_value(snapshot,0,i,&cell);
        if (status==XR_XIR_VALUE_OK) { xr_xir_value_drop(&value);value=cell; }
        else CHECK(status==XR_XIR_VALUE_UNSUPPORTED);
        XdapValueView view={&value,fixture->domain,snapshot,0};XdapTypeText type={0};
        CHECK(xdap_value_type_text(&view,&type)==XR_XIR_VALUE_OK);
        if (type.length==10 && !memcmp(type.bytes,"Array<i64>",10)) {
            bool seen=false;for (uint32_t j=0;j<count;++j) if (arrays[j].payload==value.payload) seen=true;
            if (!seen) { CHECK(count<2);arrays[count++]=value;value=(XrXirValue){0}; }
        } else if (value.type==XR_XIR_STRING && !text->type) {
            const char *bytes=NULL;size_t length=0;
            CHECK(xr_xir_string_view(&value,&bytes,&length) && length==11 && !memcmp(bytes,"owned\0value",11));
            *text=value;value=(XrXirValue){0};
        }
        xr_xir_value_drop(&value);
    }
    CHECK(text->type==XR_XIR_STRING);return count;
}
static uint64_t cow_reference(CowFixture *fixture) {
    XdapAdapter *adapter=fixture->adapter;char message[256];
    snprintf(message,sizeof(message),"{\"seq\":4,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%u}}",adapter->frames[0].handle);
    request(adapter,message);XrJsonValue *reply=last_response("scopes",true);
    uint64_t scope=(uint64_t)xjson_get_int(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"scopes"),0),"variablesReference");xjson_free(reply);
    snprintf(message,sizeof(message),"{\"seq\":5,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%llu}}",(unsigned long long)scope);
    request(adapter,message);reply=last_response("variables",true);uint64_t handle=0;
    XrJsonValue *values=xjson_get_array(xjson_get_object(reply,"body"),"variables");
    for (int i=0;i<xjson_array_len(values);++i) {
        XrJsonValue *value=xjson_array_get(values,i);
        if (!strcmp(xjson_get_string(value,"type"),"Array<i64>")) handle=(uint64_t)xjson_get_int(value,"variablesReference");
    }
    xjson_free(reply);CHECK(handle);return handle;
}
static void cow_retired(CowFixture *fixture,uint64_t handle) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":6,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%llu}}",(unsigned long long)handle);
    request(fixture->adapter,message);XrJsonValue *reply=last_response("variables",false);xjson_free(reply);
}
static void cow_case(uint32_t mode) {
    CowFixture fixture;cow_open(&fixture);XdapAdapter *adapter=fixture.adapter;
    XrXirVmDebugSnapshot *before=NULL,*after=NULL;XrXirValue old[2]={{0}},current[2]={{0}},title={0},later_title={0};
    CHECK(xdap_current_snapshot(adapter->launch->control,&before)==XR_XIR_VALUE_OK);
    CHECK(cow_values(&fixture,before,old,&title)==1);uint64_t retired=cow_reference(&fixture);
    if (mode!=1) {
        request(adapter,"{\"seq\":7,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");cow_retired(&fixture,retired);
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==2);
        CHECK(xdap_current_snapshot(adapter->launch->control,&after)==XR_XIR_VALUE_OK);
        CHECK(cow_values(&fixture,after,current,&later_title)==2);
        bool original=false,mutated=false;
        for (uint32_t i=0;i<2;++i) {
            bool same=current[i].payload==old[0].payload;original|=same;mutated|=!same;
            XdapValueView view={&current[i],fixture.domain,after,0};cow_array(&view,same ? 20 : 42);
        }
        CHECK(original && mutated);
    }
    if (!mode) {
        request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && adapter->outcome.status==XR_XIR_CALL_RETURNED);
        char output[64]={0};size_t used=0;
        for (uint32_t i=0;i<capture.count;++i) {
            XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
            if (event && !strcmp(event,"output")) { XrJsonValue *text=xjson_get(xjson_get_object(message,"body"),"output");
                CHECK(used+text->string_len<=sizeof(output));memcpy(output+used,text->as.string,text->string_len);used+=text->string_len; }
            xjson_free(message);
        }
        CHECK(used==18 && !memcmp(output,"42\n20\nowned\0value\n",18));
    } else if (mode==1) request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"terminate\"}");
    else if (mode==2) request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"disconnect\"}");
    else {
        request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"restart\"}");cow_retired(&fixture,retired);cow_rules(&fixture);
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==3);
        uint64_t fresh=cow_reference(&fixture);CHECK(fresh!=retired);cow_retired(&fixture,retired);
        request(adapter,"{\"seq\":9,\"type\":\"request\",\"command\":\"terminate\"}");
    }
    CHECK(mode==0 || !event_count("output"));
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);fixture.adapter=NULL;
    XdapValueView view={&old[0],fixture.domain,before,0};cow_array(&view,20);
    for (uint32_t i=0;i<2;++i) if (current[i].type) {
        view=(XdapValueView){&current[i],fixture.domain,after,0};cow_array(&view,current[i].payload==old[0].payload ? 20 : 42);xr_xir_value_drop(&current[i]);
    }
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&title,&bytes,&length) && length==11 && !memcmp(bytes,"owned\0value",11));
    xr_xir_value_drop(&old[0]);xr_xir_value_drop(&title);xr_xir_value_drop(&later_title);
    xr_xir_vm_debug_snapshot_drop(before);xr_xir_vm_debug_snapshot_drop(after);cow_close(&fixture);
}
static size_t cow_fault(size_t point) {
    CowFixture fixture;cow_open(&fixture);XdapAdapter *adapter=fixture.adapter;
    XrXirVmDebugSnapshot *before=NULL;XrXirValue old[2]={{0}},title={0};
    CHECK(xdap_current_snapshot(adapter->launch->control,&before)==XR_XIR_VALUE_OK);
    CHECK(cow_values(&fixture,before,old,&title)==1);uint64_t epoch=adapter->stop_epoch;
    CHECK(xdap_current_continue(adapter->launch->control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(fixture.domain);size_t begin=runtime_attempts;
    runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;XdapCurrentResult result={0};
    CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(point==SIZE_MAX || sites>point);
    if (point==SIZE_MAX) CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch>epoch);
    else CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM && !result.outcome.value.type && !result.debug_epoch);
    CHECK(adapter->snapshot==before && adapter->stop_epoch==epoch);
    XrXirDomainBudgetStats spent=xr_xir_domain_budget_stats(fixture.domain);
    CHECK(spent.work>=budget.work && spent.requested_bytes>=budget.requested_bytes);
    XdapValueView view={&old[0],fixture.domain,before,0};cow_array(&view,20);
    xdap_current_result_drop(&result);xr_xir_value_drop(&old[0]);xr_xir_value_drop(&title);xr_xir_vm_debug_snapshot_drop(before);
    cow_close(&fixture);return sites;
}
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);xr_test_suppress_dialogs();
    for (uint32_t i=0;i<4;++i) cow_case(i);
    size_t sites=cow_fault(SIZE_MAX);CHECK(sites);if (resources) for (size_t i=0;i<sites;++i) cow_fault(i);
    printf("Actual CoW mutation/next snapshot: %zu allocation sites, injection=%s, preserved old values and physical0/0\n",sites,resources ? "ALL" : "NOT_RUN");
    puts("Original Source CoW, non-value roots rejected, NUL output, retired refs and continue/cancel/disconnect/restart: PASS");return 0;
}
