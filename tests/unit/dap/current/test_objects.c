/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_objects.c - Caller-bound object reads and independently owned permissions
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_type_arena.h"
static XdapAdapter *objects_setup(XrCompileResources **ledger,XrXirDomain **domain,XrCompileResourceStats *baseline) {
    memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(*ledger,baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{*ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=*domain,.write=capture_message,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    XdapAdapter *adapter=NULL;CHECK(xdap_adapter_new(&compiler,&config,&adapter)==XR_XIR_OK && !compiler);
    capture.adapter=adapter;capture.reentry=true;
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"supportsVariableType\":true,\"supportsVariablePaging\":true}}");
    request(adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_OBJECTS_FIXTURE "\"}}");
    XrJsonValue *reply=last_response("launch",true);xjson_free(reply);
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"leaf\"}]}}");
    return adapter;
}
static void objects_dispose(XdapAdapter *adapter,XrCompileResources *ledger,XrXirDomain *domain,XrCompileResourceStats baseline) {
    XrXirCallStatus status=xdap_adapter_free(adapter);CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_OOM);
    CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(ledger);CHECK(!runtime_live && !runtime_bytes);
}
static XrJsonValue *objects_variables(XdapAdapter *adapter,uint64_t handle) {
    char message[256];snprintf(message,sizeof(message),
        "{\"seq\":10,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%llu}}",(unsigned long long)handle);
    request(adapter,message);return last_response("variables",true);
}
static uint64_t objects_reference(XdapAdapter *adapter,uint64_t frame,uint64_t expected) {
    char message[256];snprintf(message,sizeof(message),
        "{\"seq\":11,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%llu}}",(unsigned long long)frame);
    request(adapter,message);XrJsonValue *reply=last_response("scopes",true);
    uint64_t scope=(uint64_t)xjson_get_int(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"scopes"),0),"variablesReference");xjson_free(reply);
    reply=objects_variables(adapter,scope);XrJsonValue *variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");uint64_t handle=0;
    if (frame==adapter->frames[0].handle) {
        CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,1),"type"),"Choice"));
        CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,2),"type"),"i64?"));
        CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,3),"type"),"i64"));
    }
    if (frame==adapter->frames[1].handle) {
        bool callable=false;
        for (int i=0;i<xjson_array_len(variables);++i) if (!strcmp(xjson_get_string(xjson_array_get(variables,i),"type"),"function")) callable=true;
        CHECK(callable);
    }
    for (int i=0;i<xjson_array_len(variables);++i) {
        XrJsonValue *value=xjson_array_get(variables,i);
        if ((uint64_t)xjson_get_int(value,"namedVariables")==expected) {
            CHECK(!strcmp(xjson_get_string(value,"type"),"Vault"));handle=(uint64_t)xjson_get_int(value,"variablesReference");break;
        }
    }
    xjson_free(reply);CHECK(handle);return handle;
}
static size_t objects_capture_fi(size_t point) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=objects_setup(&ledger,&domain,&baseline);size_t begin=runtime_attempts;
    runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;
    XrXirCallStatus started=xdap_current_start(adapter->launch->control,adapter->launch->entry,NULL,0);XdapCurrentResult result={0};
    if (started==XR_XIR_CALL_READY) {
        CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK);
        if (point==SIZE_MAX) CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch);
        else CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM && !result.outcome.value.type);
    } else CHECK(point!=SIZE_MAX && started==XR_XIR_CALL_OOM);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(point==SIZE_MAX || sites>point);
    xdap_current_result_drop(&result);objects_dispose(adapter,ledger,domain,baseline);return sites;
}
static void objects_children_fi(XdapAdapter *adapter,uint64_t handle) {
    char message[256];snprintf(message,sizeof(message),
        "{\"seq\":12,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%llu}}",(unsigned long long)handle);
    size_t sites=0;uint32_t before_refs=adapter->value_count;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    for (size_t point=0;!point || point<=sites;++point) {
        XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(adapter->resources,&before)==XR_COMPILE_RESOURCE_OK);
        XrXirDomainBudgetStats metadata=xr_xir_domain_budget_stats(adapter->config.observer_metadata);
        XrXirDomainStats values=xr_xir_domain_stats(adapter->config.observer_metadata);
        size_t begin=runtime_attempts;runtime_fail_at=point ? begin+point-1 : SIZE_MAX;
        XrXirStatus status=xdap_adapter_request(adapter,message,strlen(message));size_t count=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
        if (!point) { CHECK(status==XR_XIR_OK);sites=count;CHECK(sites); }
        else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY && count>=point && adapter->value_count==before_refs);
            XrCompileResourceStats failed={0};CHECK(xr_compile_resources_stats(adapter->resources,&failed)==XR_COMPILE_RESOURCE_OK);
            CHECK(failed.live_bytes==before.live_bytes && failed.work>=before.work && failed.allocated_bytes>=before.allocated_bytes);
            CHECK(xr_xir_domain_budget_stats(adapter->config.observer_metadata).metadata_live==metadata.metadata_live);
            CHECK(xr_xir_domain_stats(adapter->config.observer_metadata).live_bytes==values.live_bytes);
            request(adapter,message);
        }
        CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch);
        XrJsonValue *reply=last_response("variables",true);CHECK(xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables"))==5);xjson_free(reply);
        while (adapter->value_count>before_refs) {
            XdapValueRef *ref=&adapter->values[--adapter->value_count];xr_xir_value_drop(&ref->value);*ref=(XdapValueRef){0};
        }
        CHECK(xr_xir_domain_budget_stats(adapter->config.observer_metadata).metadata_live==metadata.metadata_live);
        CHECK(xr_xir_domain_stats(adapter->config.observer_metadata).live_bytes==values.live_bytes);
    }
    printf("Object child response full FI: %zu allocation sites, original ledgers/rollback/epoch PASS\n",sites);
}
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);xr_test_suppress_dialogs();
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=objects_setup(&ledger,&domain,&baseline);
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    XrXirStatus stepped=xdap_adapter_step(adapter,1000);
    if (stepped!=XR_XIR_OK || event_count("stopped")!=1)
        fprintf(stderr,"Object debug start: status=%u outcome=%u terminal=%d stopped=%u\n",(unsigned)stepped,(unsigned)adapter->outcome.status,(int)adapter->terminal,event_count("stopped"));
    CHECK(stepped==XR_XIR_OK && event_count("stopped")==1);
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    XrJsonValue *reply=last_response("stackTrace",true);XrJsonValue *frames=xjson_get_array(xjson_get_object(reply,"body"),"stackFrames");CHECK(xjson_array_len(frames)>=3);
    uint64_t frame_ids[3];for (uint32_t i=0;i<3;++i) frame_ids[i]=(uint64_t)xjson_get_int(xjson_array_get(frames,i),"id");xjson_free(reply);
    uint64_t outside=objects_reference(adapter,frame_ids[0],3),inside=objects_reference(adapter,frame_ids[1],5);
    uint64_t caller=objects_reference(adapter,frame_ids[2],3);CHECK(outside!=inside && inside!=caller && outside!=caller);
    const XdapValueRef *refs[3]={0};uint64_t handles[]={outside,inside,caller};
    for (uint32_t i=0;i<adapter->value_count;++i) for (uint32_t j=0;j<3;++j) if (adapter->values[i].handle==handles[j]) refs[j]=&adapter->values[i];
    CHECK(refs[0] && refs[1] && refs[2] && refs[0]->value.payload==refs[1]->value.payload && refs[1]->value.payload==refs[2]->value.payload);
    if (resources) objects_children_fi(adapter,inside);
    for (uint32_t j=0;j<3;++j) {
        reply=objects_variables(adapter,handles[j]);XrJsonValue *variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");CHECK(xjson_array_len(variables)==(j==1 ? 5 : 3));
        CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,0),"name"),"visible"));
        CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,0),"value"),"11"));
        if (j==1) { CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,1),"name"),"secret"));CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,2),"name"),"seal")); }
        CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,j==1 ? 3 : 1),"type"),"Array<Parcel>"));
        CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,j==1 ? 4 : 2),"type"),"Parcel"));
        uint64_t nested=(uint64_t)xjson_get_int(xjson_array_get(variables,j==1 ? 3 : 1),"variablesReference");xjson_free(reply);CHECK(nested);
        reply=objects_variables(adapter,nested);variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");CHECK(xjson_array_len(variables)==1);
        uint64_t parcel=(uint64_t)xjson_get_int(xjson_array_get(variables,0),"variablesReference");xjson_free(reply);CHECK(parcel);
        reply=objects_variables(adapter,parcel);variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");CHECK(xjson_array_len(variables)==2);
        CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,0),"name"),"label") && !strcmp(xjson_get_string(xjson_array_get(variables,0),"value"),"owned"));xjson_free(reply);
    }
    XrXirVmDebugSnapshot *snapshot=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&snapshot)==XR_XIR_VALUE_OK && snapshot==adapter->snapshot);
    XrXirValue escaped={0};CHECK(xr_xir_value_copy(&refs[1]->value,&escaped)==XR_XIR_VALUE_OK);bool allowed=true;
    CHECK(xr_xir_vm_debug_snapshot_member(snapshot,0,&escaped,1,&allowed)==XR_XIR_VALUE_OK && !allowed);
    CHECK(xr_xir_vm_debug_snapshot_member(snapshot,1,&escaped,1,&allowed)==XR_XIR_VALUE_OK && allowed);
    CHECK(xr_xir_vm_debug_snapshot_type_visible(snapshot,0,&escaped,(XrXirType)escaped.type,&allowed)==XR_XIR_VALUE_OK && allowed);
    CHECK(xr_xir_vm_debug_snapshot_type_visible(snapshot,UINT32_MAX,&escaped,(XrXirType)escaped.type,&allowed)==XR_XIR_VALUE_BAD_ARGUMENT && allowed);
    CHECK(xr_xir_vm_debug_snapshot_type_visible(snapshot,1,&escaped,XR_XIR_I64,&allowed)==XR_XIR_VALUE_UNSUPPORTED && allowed);
    CHECK(xr_xir_vm_debug_snapshot_member(snapshot,UINT32_MAX,&escaped,1,&allowed)==XR_XIR_VALUE_BAD_ARGUMENT && allowed);
    CHECK(xr_xir_vm_debug_snapshot_member(snapshot,1,&escaped,UINT32_MAX,&allowed)==XR_XIR_VALUE_BOUNDS && allowed);
    XrXirValue foreign={0};CHECK(xr_xir_string_new(domain,"foreign",7,&foreign)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_snapshot_member(snapshot,1,&foreign,0,&allowed)==XR_XIR_VALUE_BAD_ARGUMENT && allowed);xr_xir_value_drop(&foreign);
    request(adapter,"{\"seq\":20,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("terminated")==1);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);adapter=NULL;
    CHECK(xr_xir_vm_debug_snapshot_member(snapshot,1,&escaped,1,&allowed)==XR_XIR_VALUE_OK && allowed);
    XdapValueView view={&escaped,domain,snapshot,1};XrXirValue secret={0};const char *name=NULL;size_t length=0;
    XdapTypeText type={0};CHECK(xdap_value_type_text(&view,&type)==XR_XIR_VALUE_OK && type.length==5 && !memcmp(type.bytes,"Vault",5));
    CHECK(xdap_value_child(&view,1,&secret,&name,&length)==XR_XIR_VALUE_OK && length==6 && !memcmp(name,"secret",6));
    CHECK(secret.type==XR_XIR_I64 && secret.payload==22);xr_xir_value_drop(&secret);xr_xir_value_drop(&escaped);xr_xir_vm_debug_snapshot_drop(snapshot);
    CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(ledger);CHECK(!runtime_live && !runtime_bytes);
    if (resources) { size_t sites=objects_capture_fi(SIZE_MAX);CHECK(sites);for (size_t i=0;i<sites;++i) objects_capture_fi(i);printf("Object snapshot full FI: %zu allocation sites, physical=0/0 PASS\n",sites); }
    puts("Caller private/protected permissions, same-object frame isolation, nested fields and escaped snapshot: PASS");return 0;
}
