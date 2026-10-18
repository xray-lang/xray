/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_values.c - Real nested aggregate paging, ownership and stale references
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
static void values_faults(XdapAdapter *adapter,uint64_t handle) {
    const XrXirValue *parent=NULL;uint32_t frame=UINT32_MAX;
    for (uint32_t i=0;i<adapter->value_count;++i) if (adapter->values[i].handle==handle) {
        parent=&adapter->values[i].value;frame=adapter->values[i].frame;
    }
    CHECK(parent);XrXirDomain *domain=adapter->config.observer_metadata;size_t sites=0;
    XdapValueView view={parent,domain,adapter->snapshot,frame};
    for (size_t point=0;!point || point<=sites;++point) {
        XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(domain);
        XrXirDomainStats value_before=xr_xir_domain_stats(domain);
        size_t begin=runtime_attempts;runtime_fail_at=point ? begin+point-1 : SIZE_MAX;
        XrXirValue child={0};XdapValueChildInfo info={.name=(const char *)(uintptr_t)1,.name_length=777};
        XrXirValueStatus status=xdap_value_child(&view,0,&child,&info);
        size_t count=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
        if (!point) { CHECK(status==XR_XIR_VALUE_OK);sites=count;CHECK(sites); }
        else {
            CHECK(status==XR_XIR_VALUE_OOM && count>=point && !child.type && !child.payload);
            CHECK(info.name==(const char *)(uintptr_t)1 && info.name_length==777 && !info.selected);
            XrXirDomainBudgetStats failed=xr_xir_domain_budget_stats(domain);
            CHECK(failed.metadata_live==before.metadata_live && xr_xir_domain_stats(domain).live_bytes==value_before.live_bytes);
            CHECK(xdap_value_child(&view,0,&child,&info)==XR_XIR_VALUE_OK);
        }
        xr_xir_value_drop(&child);XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(domain);
        CHECK(after.metadata_live==before.metadata_live && xr_xir_domain_stats(domain).live_bytes==value_before.live_bytes);
        CHECK(after.work>=before.work && after.requested_bytes>=before.requested_bytes);
    }
    printf("Typed inline aggregate child full FI: %zu allocation sites, preserved output/name, original Domain retry and baselines PASS\n",sites);
}
static void values_request(XdapAdapter *adapter,uint32_t seq,uint64_t handle,uint64_t start,uint64_t count) {
    char message[512];snprintf(message,sizeof(message),
        "{\"seq\":%u,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%llu,\"start\":%llu,\"count\":%llu}}",
        seq,(unsigned long long)handle,(unsigned long long)start,(unsigned long long)count);request(adapter,message);
}
static void values_type_budget(XrXirDomain *domain) {
    const XrXirType scalar[]={XR_XIR_UNIT,XR_XIR_BOOL,XR_XIR_I8,XR_XIR_I16,XR_XIR_I32,XR_XIR_I64,
        XR_XIR_U8,XR_XIR_U16,XR_XIR_U32,XR_XIR_U64,XR_XIR_F32,XR_XIR_F64,XR_XIR_RUNE};
    const char *names[]={"()","bool","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","rune"};
    for (uint32_t i=0;i<sizeof(scalar)/sizeof(scalar[0]);++i) {
        XrXirValue value={(uint32_t)scalar[i],0,0};XdapValueView view={&value,domain,NULL,0};XdapTypeText text={0};
        CHECK(xdap_value_type_text(&view,&text)==XR_XIR_VALUE_OK && text.length==strlen(names[i]) && !memcmp(text.bytes,names[i],text.length));
    }
    XrXirDomainBudgetControls limits={1048576,1048576,1,1048576,1048576};XrXirDomain *limited=NULL;
    CHECK(xr_xir_domain_new_budgeted(1048576,&limits,&limited)==XR_XIR_VALUE_OK);
    XrXirValue value={0};XdapValueView view={&value,limited,NULL,0};XdapTypeText output;
    memset(&output,0x5a,sizeof(output));XdapTypeText before=output;
    CHECK(xdap_value_type_text(&view,&output)==XR_XIR_VALUE_LIMIT && !memcmp(&output,&before,sizeof(output)));
    CHECK(xdap_value_type_text(&view,&output)==XR_XIR_VALUE_LIMIT && !memcmp(&output,&before,sizeof(output)));xr_xir_domain_drop(limited);
}
int main(int argc,char **argv) {
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    xr_test_suppress_dialogs();XrXirCompileContext context={0};XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={context,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomain *domain=NULL;XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&domain)==XR_XIR_VALUE_OK);
    values_type_budget(domain);
    XdapAdapterConfig config={.observer_metadata=domain,.write=capture_message,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    XdapAdapter *adapter=NULL;CHECK(xdap_adapter_new(&compiler,&config,&adapter)==XR_XIR_OK);capture.adapter=adapter;capture.reentry=true;
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"supportsVariableType\":true,\"supportsVariablePaging\":true}}");
    char message[4096];snprintf(message,sizeof(message),
        "{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",XR_DAP_VALUES_FIXTURE);
    request(adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"leaf\"}]}}");
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==1);
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    reply=last_response("stackTrace",true);int64_t frame=xjson_get_int(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"stackFrames"),0),"id");xjson_free(reply);
    snprintf(message,sizeof(message),"{\"seq\":6,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%lld}}",(long long)frame);
    request(adapter,message);reply=last_response("scopes",true);
    uint64_t scope=(uint64_t)xjson_get_int(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"scopes"),0),"variablesReference");xjson_free(reply);
    values_request(adapter,7,scope,0,0);reply=last_response("variables",true);
    XrJsonValue *variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");CHECK(xjson_array_len(variables)==5);
    const char *types[]={"Array<Array<i64>>","string","(i64, string)","i64?","Array<Parcel>"};
    for (int i=0;i<5;++i) CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,i),"type"),types[i]));
    uint64_t matrix=(uint64_t)xjson_get_int(xjson_array_get(variables,0),"variablesReference");
    CHECK(matrix && xjson_get_int(xjson_array_get(variables,0),"indexedVariables")==2);
    uint64_t pair=(uint64_t)xjson_get_int(xjson_array_get(variables,2),"variablesReference");
    uint64_t optional=(uint64_t)xjson_get_int(xjson_array_get(variables,3),"variablesReference");
    uint64_t parcel=(uint64_t)xjson_get_int(xjson_array_get(variables,4),"variablesReference");CHECK(pair && optional && parcel);
    XrJsonValue *text=xjson_get(xjson_array_get(variables,1),"value");CHECK(text->string_len==11 && !memcmp(text->as.string,"owned\0value",11));xjson_free(reply);
    values_request(adapter,8,matrix,1,1);reply=last_response("variables",true);variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");
    CHECK(xjson_array_len(variables)==1 && !strcmp(xjson_get_string(xjson_array_get(variables,0),"name"),"[1]"));
    uint64_t row=(uint64_t)xjson_get_int(xjson_array_get(variables,0),"variablesReference");CHECK(row && row!=matrix);xjson_free(reply);
    values_request(adapter,9,row,0,0);reply=last_response("variables",true);variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");
    CHECK(xjson_array_len(variables)==2 && !strcmp(xjson_get_string(xjson_array_get(variables,0),"value"),"21") &&
        !strcmp(xjson_get_string(xjson_array_get(variables,1),"value"),"22"));xjson_free(reply);
    values_request(adapter,10,matrix,5,1);reply=last_response("variables",true);
    CHECK(!xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables")));xjson_free(reply);
    values_request(adapter,14,pair,0,0);reply=last_response("variables",true);variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");
    CHECK(xjson_array_len(variables)==2 && !strcmp(xjson_get_string(xjson_array_get(variables,0),"value"),"31") &&
        !strcmp(xjson_get_string(xjson_array_get(variables,1),"value"),"pair"));xjson_free(reply);
    values_request(adapter,15,optional,0,0);reply=last_response("variables",true);variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");
    CHECK(xjson_array_len(variables)==1 && !strcmp(xjson_get_string(xjson_array_get(variables,0),"name"),"value") &&
        !strcmp(xjson_get_string(xjson_array_get(variables,0),"value"),"7"));xjson_free(reply);
    values_request(adapter,16,parcel,0,0);reply=last_response("variables",true);variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");
    CHECK(xjson_array_len(variables)==1);uint64_t parcel_value=(uint64_t)xjson_get_int(xjson_array_get(variables,0),"variablesReference");
    CHECK(parcel_value);xjson_free(reply);
    values_request(adapter,17,parcel_value,0,0);reply=last_response("variables",true);variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");
    CHECK(xjson_array_len(variables)==2 && !strcmp(xjson_get_string(xjson_array_get(variables,0),"name"),"label") &&
        !strcmp(xjson_get_string(xjson_array_get(variables,0),"value"),"box") &&
        !strcmp(xjson_get_string(xjson_array_get(variables,1),"name"),"code") &&
        !strcmp(xjson_get_string(xjson_array_get(variables,1),"value"),"41"));xjson_free(reply);
    if (resources) values_faults(adapter,parcel);
    snprintf(message,sizeof(message),"{\"seq\":18,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%llu,\"filter\":\"named\"}}",(unsigned long long)matrix);
    request(adapter,message);reply=last_response("variables",true);
    CHECK(!xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables")));xjson_free(reply);
    snprintf(message,sizeof(message),"{\"seq\":19,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%llu,\"filter\":\"indexed\"}}",(unsigned long long)scope);
    request(adapter,message);reply=last_response("variables",true);
    CHECK(!xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables")));xjson_free(reply);
    request(adapter,"{\"seq\":20,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("terminated")==1);
    values_request(adapter,21,row,0,0);reply=last_response("variables",false);xjson_free(reply);
    values_request(adapter,22,matrix,0,0);reply=last_response("variables",false);xjson_free(reply);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes);
    puts("Real Source nested array paging, NUL bytes, owned child refs, resume retirement and physical baseline: PASS");return 0;
}
