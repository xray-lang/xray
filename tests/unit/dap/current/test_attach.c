/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_attach.c - Hosted Source attachment and rejected configuration ownership
 */
#define main reference_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
#include "base/xcompile_state.h"
static const char attach_request[]="{\"seq\":9,\"type\":\"request\",\"command\":\"attach\",\"arguments\":{\"program\":\"" XR_DAP_FIXTURE "\",\"args\":[],\"cwd\":\"\",\"env\":{}}}";
static XrXirStatus attach_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;message->size=length;
    ++out->seq;
    if (out->reentry) { CHECK(xdap_adapter_free(out->adapter)==XR_XIR_CALL_BUSY);
        CHECK(xdap_adapter_request(out->adapter,"{}",2)==XR_XIR_BAD_STRUCTURE); }
    return XR_XIR_OK;
}
static XdapAdapter *attach_setup(XrCompileResources **resources,XrXirDomain **domain,XrCompileResourceStats *baseline) {
    memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(*resources,baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{*resources,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=*domain,.write=attach_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    XdapAdapter *adapter=NULL;CHECK(xdap_adapter_new(&compiler,&config,&adapter)==XR_XIR_OK && !compiler);
    capture.adapter=adapter;capture.reentry=true;return adapter;
}
static size_t attach_fault_point=SIZE_MAX;
static size_t attach_sticky_prefixes,attach_retry_prefixes;
static void attach_reply(const char *command,bool success) {
    for (uint32_t i=capture.count;i;--i) {
        Message *message=&capture.messages[i-1];XrJsonValue *reply=xjson_parse(message->bytes,message->size);CHECK(reply);
        if (!strcmp(xjson_get_string(reply,"type"),"response") && !strcmp(xjson_get_string(reply,"command"),command)) {
            if (xjson_get_bool(reply,"success")!=success)
                fprintf(stderr,"Attach fault point %zu: expected success=%d, response=%s\n",attach_fault_point,(int)success,message->bytes);
            CHECK(xjson_get_bool(reply,"success")==success);xjson_free(reply);return;
        }
        xjson_free(reply);
    }
    CHECK(false);
}
static void attach_dispose(XdapAdapter *adapter,XrCompileResources *ledger,XrXirDomain *domain,XrCompileResourceStats baseline) {
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==baseline.live_bytes);xr_compile_resources_release(ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void attach_behavior(void) {
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=attach_setup(&ledger,&domain,&baseline);
    request(adapter,attach_request);attach_reply("attach",false);CHECK(!adapter->launch);
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    request(adapter,"{\"seq\":2,\"type\":\"request\",\"command\":\"attach\"}");attach_reply("attach",false);
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\"}");attach_reply("configurationDone",false);
    const char *settings[]={"\"args\":[\"ignored\"]","\"cwd\":\"C:/\"","\"env\":{\"P16_TEST\":\"ignored\"}","\"processId\":7","\"args\":null"};
    char message[4096];
    for (uint32_t i=0;i<sizeof(settings)/sizeof(settings[0]);++i) {
        for (uint32_t kind=0;kind<2;++kind) {
            const char *command=kind ? "launch" : "attach";
            snprintf(message,sizeof(message),"{\"seq\":4,\"type\":\"request\",\"command\":\"%s\",\"arguments\":{\"program\":\"%s\",%s}}",command,XR_DAP_FIXTURE,settings[i]);
            request(adapter,message);attach_reply(command,false);CHECK(!adapter->launch && !adapter->running && !adapter->configured);
        }
    }
    request(adapter,attach_request);XrJsonValue *reply=last_response("attach",true);
    CHECK(xjson_get_int(reply,"request_seq")==9);xjson_free(reply);
    CHECK(adapter->launch && !adapter->configured && !adapter->running && !event_count("output"));
    XdapLaunch *target=adapter->launch;request(adapter,attach_request);attach_reply("attach",false);CHECK(adapter->launch==target);
    request(adapter,"{\"seq\":10,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"leaf\"}]}}");
    request(adapter,"{\"seq\":11,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && event_count("stopped")==1);
    request(adapter,"{\"seq\":12,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    reply=last_response("stackTrace",true);XrJsonValue *frames=xjson_get_array(xjson_get_object(reply,"body"),"stackFrames");
    CHECK(xjson_array_len(frames)==2 && !strcmp(xjson_get_string(xjson_array_get(frames,0),"name"),"leaf"));
    int64_t frame=xjson_get_int(xjson_array_get(frames,0),"id");xjson_free(reply);
    XrXirValue value={0};CHECK(xr_xir_vm_debug_snapshot_value(adapter->snapshot,0,0,&value)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_I64 && value.payload==20);xr_xir_value_drop(&value);
    request(adapter,"{\"seq\":13,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && adapter->outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(event_count("output")==1 && event_count("exited")==1 && event_count("terminated")==1);
    bool output=false;
    for (uint32_t i=0;i<capture.count;++i) {
        reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) { XrJsonValue *bytes=xjson_get(xjson_get_object(reply,"body"),"output");
            CHECK(bytes->string_len==12 && !memcmp(bytes->as.string,"owned\0value\n",12));output=true; }
        xjson_free(reply);
    }
    CHECK(output);snprintf(message,sizeof(message),"{\"seq\":14,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%lld}}",(long long)frame);
    request(adapter,message);attach_reply("scopes",false);
    request(adapter,"{\"seq\":15,\"type\":\"request\",\"command\":\"restart\"}");attach_reply("restart",true);
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && event_count("stopped")==2);
    request(adapter,"{\"seq\":16,\"type\":\"request\",\"command\":\"terminate\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->outcome.status==XR_XIR_CALL_CANCELLED && event_count("output")==1);
    request(adapter,"{\"seq\":17,\"type\":\"request\",\"command\":\"disconnect\"}");CHECK(xdap_adapter_closed(adapter));
    attach_dispose(adapter,ledger,domain,baseline);puts("Hosted Source attach/configure/stop/owned value/resume/restart/cancel, rejected host options and physical 0/0: PASS");
}
static size_t attach_failure(size_t point) {
    attach_fault_point=point;
    XrCompileResources *ledger=NULL;XrXirDomain *domain=NULL;XrCompileResourceStats baseline={0};
    XdapAdapter *adapter=attach_setup(&ledger,&domain,&baseline);
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    XrCompileResourceStats before={0};CHECK(xr_compile_resources_stats(ledger,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirDomainBudgetStats domain_before=xr_xir_domain_budget_stats(domain);
    size_t begin=runtime_attempts;runtime_fail_at=point==SIZE_MAX ? SIZE_MAX : begin+point;
    XrXirStatus status=xdap_adapter_request(adapter,attach_request,sizeof(attach_request)-1);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (point==SIZE_MAX) { CHECK(status==XR_XIR_OK && adapter->launch);attach_reply("attach",true); }
    else {
        CHECK(sites>point && !adapter->launch && !adapter->configured && !adapter->running && !adapter->snapshot);
        CHECK(status==XR_XIR_OUT_OF_MEMORY || status==XR_XIR_OK);
        if (status==XR_XIR_OK) attach_reply("attach",false);
        CHECK(xr_xir_domain_budget_stats(domain).metadata_live==domain_before.metadata_live);
        XrSourceCompiler *compiler=adapter->compiler;XrCompileState *state=xr_source_compiler_state(compiler);
        XrCompileResourceStatus fatal=xr_compile_state_status(state);request(adapter,attach_request);
        CHECK(adapter->compiler==compiler && xr_source_compiler_resources(compiler)==ledger && xr_source_compiler_state(compiler)==state);
        if (fatal==XR_COMPILE_RESOURCE_OK) {
            attach_reply("attach",true);CHECK(adapter->launch);++attach_retry_prefixes;
        } else {
            CHECK(fatal==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);attach_reply("attach",false);
            CHECK(!adapter->launch && !adapter->configured && !adapter->running && !adapter->snapshot);
            CHECK(xr_compile_state_status(state)==fatal);++attach_sticky_prefixes;
        }
    }
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.allocated_bytes>=before.allocated_bytes && after.work>=before.work);
    attach_dispose(adapter,ledger,domain,baseline);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    attach_behavior();size_t sites=attach_failure(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t i=0;i<sites;++i) attach_failure(i);
    if (resources) CHECK(attach_retry_prefixes && attach_sticky_prefixes && attach_retry_prefixes+attach_sticky_prefixes==sites);
    printf("Hosted attach: %zu allocation sites, injection=%s, recoverable=%zu, sticky=%zu; original compiler/Domain and physical 0/0\n",
        sites,resources ? "ALL" : "NOT_RUN",attach_retry_prefixes,attach_sticky_prefixes);return 0;
}
