/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_exceptions.c - Real caught and uncaught throws preserve ordinary outcomes
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
#include "xir/xxir_enum.h"
static XrXirStatus exception_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;message->size=length;
    ++out->seq;return XR_XIR_OK;
}
static XdapAdapter *exception_setup(XrCompileResources **resources,XrXirDomain **domain,XrCompileResourceStats *baseline) {
    memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(*resources,baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{*resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=*domain,.write=exception_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    XdapAdapter *adapter=NULL;CHECK(xdap_adapter_new(&compiler,&config,&adapter)==XR_XIR_OK && !compiler);return adapter;
}
static void exception_reply(const char *command,bool success) { XrJsonValue *reply=last_response(command,success);xjson_free(reply); }
static size_t exception_capture_fi(size_t point) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=exception_setup(&ledger,&domain,&baseline);
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    request(adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"all\"]}}");
    const char launch[]="{\"seq\":3,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_UNCAUGHT_FIXTURE "\"}}";
    request(adapter,launch);exception_reply("launch",true);
    size_t begin=runtime_attempts;runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;
    XrXirCallStatus started=xdap_current_start(adapter->launch->control,adapter->launch->entry,NULL,0);
    XdapCurrentResult result={0};
    if (started==XR_XIR_CALL_READY) {
        CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK);
        if (point==SIZE_MAX) CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch && !result.outcome.wake);
        else CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM && !result.outcome.value.type);
    } else CHECK(point!=SIZE_MAX && started==XR_XIR_CALL_OOM);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(point==SIZE_MAX || sites>point);
    xdap_current_result_drop(&result);
    XrXirCallStatus disposed=xdap_adapter_free(adapter);
    CHECK(disposed==XR_XIR_CALL_READY || (point!=SIZE_MAX && disposed==XR_XIR_CALL_OOM));
    CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(ledger);CHECK(!runtime_live && !runtime_bytes);return sites;
}
static void exception_requests_fi(XdapAdapter *adapter) {
    const char *messages[]={
        "{\"seq\":30,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}",
        "{\"seq\":31,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[]}}"};
    for (uint32_t operation=0;operation<2;++operation) {
        size_t sites=0;
        for (size_t point=0;!point || point<=sites;++point) {
            CHECK(adapter->break_throw);
            XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(adapter->resources,&before)==XR_COMPILE_RESOURCE_OK);
            XrXirDomainBudgetStats domain=xr_xir_domain_budget_stats(adapter->config.observer_metadata);
            XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
            size_t begin=runtime_attempts;runtime_fail_at=point ? begin+point-1 : SIZE_MAX;
            XrXirStatus status=xdap_adapter_request(adapter,messages[operation],strlen(messages[operation]));
            size_t attempts=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
            if (!point) { CHECK(status==XR_XIR_OK);sites=attempts;CHECK(sites); }
            else { CHECK(status==XR_XIR_OUT_OF_MEMORY && attempts>=point && adapter->break_throw);
                request(adapter,messages[operation]); }
            CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running);
            if (operation==1) { CHECK(!adapter->break_throw);
                request(adapter,"{\"seq\":32,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"all\"]}}"); }
            XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(adapter->resources,&after)==XR_COMPILE_RESOURCE_OK);
            CHECK(after.live_bytes==before.live_bytes && after.work>=before.work && after.allocated_bytes>=before.allocated_bytes);
            XrXirDomainBudgetStats current=xr_xir_domain_budget_stats(adapter->config.observer_metadata);
            CHECK(current.metadata_live==domain.metadata_live && current.work>=domain.work);
        }
        printf("Exception request full FI %u: %zu sites, original owners/state/ledger retry PASS\n",operation,sites);
    }
}
static void exception_case(bool caught,bool enabled,bool resources) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=exception_setup(&ledger,&domain,&baseline);
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    XrJsonValue *reply=last_response("initialize",true);XrJsonValue *body=xjson_get_object(reply,"body");
    CHECK(xjson_get_bool(body,"supportsExceptionInfoRequest"));CHECK(xjson_array_len(xjson_get_array(body,"exceptionBreakpointFilters"))==1);xjson_free(reply);
    if (enabled) { request(adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"all\"]}}");exception_reply("setExceptionBreakpoints",true); }
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"uncaught\"]}}");
    exception_reply("setExceptionBreakpoints",false);CHECK(adapter->break_throw==enabled);
    request(adapter,"{\"seq\":33,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"all\",\"all\"]}}");exception_reply("setExceptionBreakpoints",false);
    request(adapter,"{\"seq\":34,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[7]}}");exception_reply("setExceptionBreakpoints",false);
    request(adapter,"{\"seq\":35,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\"}");exception_reply("setExceptionBreakpoints",false);
    request(adapter,"{\"seq\":36,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"all\"],\"filterOptions\":[{\"filterId\":\"all\",\"condition\":\"true\"}]}}");exception_reply("setExceptionBreakpoints",false);
    CHECK(adapter->break_throw==enabled);
    char message[4096];snprintf(message,sizeof(message),
        "{\"seq\":4,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",caught ? XR_DAP_CAUGHT_FIXTURE : XR_DAP_UNCAUGHT_FIXTURE);
    request(adapter,message);exception_reply("launch",true);
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"configurationDone\"}");
    request(adapter,"{\"seq\":37,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[]}}");exception_reply("setExceptionBreakpoints",false);CHECK(adapter->break_throw==enabled);
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK);
    XrXirVmDebugSnapshot *escaped=NULL;XrXirValue value={0};
    if (enabled) {
        CHECK(adapter->snapshot && !adapter->terminal && !adapter->wait_wake && event_count("stopped")==1 && !event_count("output"));
        for (uint32_t i=0;i<capture.count;++i) {
            reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
            if (event && !strcmp(event,"stopped")) CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"reason"),"exception"));xjson_free(reply);
        }
        request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}");
        reply=last_response("exceptionInfo",true);body=xjson_get_object(reply,"body");
        CHECK(!strcmp(xjson_get_string(body,"exceptionId"),"DapFailure.Expected") && !strcmp(xjson_get_string(body,"breakMode"),"always"));xjson_free(reply);
        request(adapter,"{\"seq\":7,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":2}}");exception_reply("exceptionInfo",false);
        request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"exceptionInfo\"}");exception_reply("exceptionInfo",false);
        CHECK(xdap_current_snapshot(adapter->launch->control,&escaped)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_vm_debug_snapshot_exception(escaped,&value)==XR_XIR_VALUE_OK);
        if (resources && caught) exception_requests_fi(adapter);
        request(adapter,"{\"seq\":9,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK);
    }
    CHECK(adapter->terminal && !adapter->running && !adapter->snapshot);
    CHECK(adapter->outcome.status==(caught ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_THROWN));
    CHECK(!adapter->outcome.wake && event_count("stopped")== (enabled ? 1u : 0u));
    CHECK(event_count("output")== (caught ? 1u : 0u) && event_count("terminated")==1 && event_count("exited")==1);
    if (caught) for (uint32_t i=0;i<capture.count;++i) {
        reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"output"),"caught\n"));xjson_free(reply);
    }
    request(adapter,"{\"seq\":10,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}");exception_reply("exceptionInfo",false);
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("terminated")==1);
    if (caught && enabled) {
        request(adapter,"{\"seq\":11,\"type\":\"request\",\"command\":\"restart\"}");exception_reply("restart",true);
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && event_count("stopped")==2);
        request(adapter,"{\"seq\":12,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}");exception_reply("exceptionInfo",true);
        request(adapter,"{\"seq\":13,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[]}}");exception_reply("setExceptionBreakpoints",true);CHECK(!adapter->break_throw);
        request(adapter,"{\"seq\":14,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}");exception_reply("exceptionInfo",true);
        request(adapter,"{\"seq\":15,\"type\":\"request\",\"command\":\"terminate\"}");exception_reply("terminate",true);
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && event_count("terminated")==2);
        CHECK(adapter->outcome.status==XR_XIR_CALL_CANCELLED && !adapter->outcome.value.type && event_count("output")==1);
        request(adapter,"{\"seq\":16,\"type\":\"request\",\"command\":\"restart\"}");exception_reply("restart",true);
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && adapter->outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(event_count("stopped")==2 && event_count("output")==2 && event_count("terminated")==3);
    }
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    if (escaped) { XrXirValue copy={0};CHECK(xr_xir_vm_debug_snapshot_exception(escaped,&copy)==XR_XIR_VALUE_OK);xr_xir_value_drop(&copy);
        XrXirEnumBorrow borrowed={0};CHECK(xr_xir_enum_borrow(&value,&borrowed)==XR_XIR_VALUE_OK);
        CHECK(borrowed.name.length==10 && !memcmp(borrowed.name.bytes,"DapFailure",10)); }
    xr_xir_vm_debug_snapshot_drop(escaped);xr_xir_value_drop(&value);
    CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(ledger);CHECK(!runtime_live && !runtime_bytes);
    printf("Real %s throw, filter %s, owned escape and physical 0/0 PASS\n",caught ? "caught" : "uncaught",enabled ? "all" : "disabled");
}
static void exception_panic(void) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=exception_setup(&ledger,&domain,&baseline);
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    request(adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"all\"]}}");
    const char launch[]="{\"seq\":3,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_PANIC_FIXTURE "\",\"stopOnEntry\":true}}";
    request(adapter,launch);exception_reply("launch",true);
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && event_count("stopped")==1);
    XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_exception(adapter->snapshot,&value)==XR_XIR_VALUE_UNSUPPORTED && !value.type);
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}");exception_reply("exceptionInfo",false);
    request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && adapter->outcome.status==XR_XIR_CALL_DIVIDE_BY_ZERO);
    CHECK(!adapter->outcome.value.type && xr_xir_call_panic_status(adapter->outcome.status) && event_count("stopped")==1);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(ledger);CHECK(!runtime_live && !runtime_bytes);
    puts("Entry has no exception; real numeric panic stays outside language throw filter: PASS");
}
static XdapAdapter *exception_erased_setup(XrCompileResources **ledger,XrXirDomain **domain,XrCompileResourceStats *baseline) {
    XdapAdapter *adapter=exception_setup(ledger,domain,baseline);
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    request(adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"setExceptionBreakpoints\",\"arguments\":{\"filters\":[\"all\"]}}");
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"" XR_DAP_PRIVATE_FIXTURE "\"}}");
    exception_reply("launch",true);request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==1);return adapter;
}
static void exception_erased_dispose(XdapAdapter *adapter,XrCompileResources *ledger,XrXirDomain *domain,XrCompileResourceStats baseline) {
    XrXirCallStatus status=xdap_adapter_free(adapter);CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_OOM);
    CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(ledger);CHECK(!runtime_live && !runtime_bytes);
}
static size_t exception_erased_fault(size_t point) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=exception_erased_setup(&ledger,&domain,&baseline);
    uint64_t epoch=adapter->stop_epoch;XrXirVmDebugSnapshot *old=adapter->snapshot;
    CHECK(xdap_current_continue(adapter->launch->control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(domain);size_t begin=runtime_attempts;
    runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;XdapCurrentResult result={0};
    CHECK(xdap_current_poll(adapter->launch->control,1000,&result)==XR_XIR_VALUE_OK);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;CHECK(point==SIZE_MAX || sites>point);
    if (point==SIZE_MAX) CHECK(result.state==XDAP_CURRENT_STOPPED && result.debug_epoch>epoch && !result.outcome.value.type);
    else CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_OOM && !result.outcome.value.type);
    CHECK(adapter->snapshot==old && adapter->stop_epoch==epoch);
    XrXirValue owned={0};CHECK(xr_xir_vm_debug_snapshot_exception(old,&owned)==XR_XIR_VALUE_OK);xr_xir_value_drop(&owned);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(domain);
    CHECK(after.work>=before.work && after.requested_bytes>=before.requested_bytes);
    xdap_current_result_drop(&result);exception_erased_dispose(adapter,ledger,domain,baseline);return sites;
}
static void exception_erased_case(bool resources) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=exception_erased_setup(&ledger,&domain,&baseline);
    const char *names[]={"Hidden.Bad","<private exception>"},*types[]={"Hidden","<private type>"};
    XrXirVmDebugSnapshot *escaped=NULL;XrXirValue concrete={0};
    for (uint32_t i=0;i<2;++i) {
        request(adapter,"{\"seq\":15,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
        XrJsonValue *stack=last_response("stackTrace",true),*frames=xjson_get_array(xjson_get_object(stack,"body"),"stackFrames");
        XrJsonValue *frame=xjson_array_get(frames,0),*source=xjson_get_object(frame,"source");
        const char *file=i ? "type_private_main.xr" : "type_private_lib.xr";
        CHECK(!strcmp(xjson_get_string(source,"name"),file) && strstr(xjson_get_string(source,"path"),file));
        CHECK(xjson_get_int(frame,"line")== (i ? 3 : 2) && xjson_get_int(frame,"column")== (i ? 43 : 26));
        xjson_free(stack);
        request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"exceptionInfo\",\"arguments\":{\"threadId\":1}}");
        XrJsonValue *reply=last_response("exceptionInfo",true),*body=xjson_get_object(reply,"body");
        CHECK(!strcmp(xjson_get_string(body,"exceptionId"),names[i]));
        CHECK(!strcmp(xjson_get_string(xjson_get_object(body,"details"),"typeName"),types[i]));xjson_free(reply);
        if (i) {
            CHECK(xdap_current_snapshot(adapter->launch->control,&escaped)==XR_XIR_VALUE_OK);
            CHECK(xr_xir_vm_debug_snapshot_exception(escaped,&concrete)==XR_XIR_VALUE_OK);
            if (resources) exception_requests_fi(adapter);
        }
        request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK);
    }
    CHECK(adapter->terminal && adapter->outcome.status==XR_XIR_CALL_THROWN && event_count("stopped")==2);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);XrXirEnumBorrow borrowed={0};
    CHECK(xr_xir_enum_borrow(&concrete,&borrowed)==XR_XIR_VALUE_OK && borrowed.field_count==1);
    CHECK(borrowed.fields[0].type==XR_XIR_I64 && borrowed.fields[0].payload==41);
    XdapValueView view={&concrete,domain,escaped,0};XdapTypeText display={0};
    CHECK(xdap_value_type_text(&view,&display)==XR_XIR_VALUE_OK && display.length==14 && !memcmp(display.bytes,"<private type>",14));
    xr_xir_value_drop(&concrete);xr_xir_vm_debug_snapshot_drop(escaped);
    CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(ledger);CHECK(!runtime_live && !runtime_bytes);
    size_t sites=exception_erased_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t i=0;i<sites;++i) exception_erased_fault(i);
    printf("Erased Error rethrow: two real stops, caller private names, escaped concrete payload; %zu capture sites injection=%s\n",sites,resources ? "ALL" : "NOT_RUN");
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    exception_case(true,true,resources);exception_case(false,true,false);exception_case(true,false,false);exception_case(false,false,false);
    exception_panic();exception_erased_case(resources);
    size_t sites=exception_capture_fi(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t i=0;i<sites;++i) exception_capture_fi(i);
    printf("Real throw execution and owned capture: %zu allocation sites, injection=%s\n",sites,resources ? "ALL" : "NOT_RUN");return 0;
}
