/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dispatch.c - Real JSON requests drive Source/VM stops and binary output
 */
#include "app/dap/xdap_dispatch.h"
#include "base/xjson.h"
#include "../../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../../xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
typedef struct Message { char bytes[16384];size_t size; } Message;
typedef struct Capture { Message messages[128];uint32_t count,seq;XdapAdapter *adapter;bool reentry; } Capture;
static Capture capture;
static XrXirStatus capture_message(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;message->size=length;
    XrJsonValue *json=xjson_parse(bytes,length);CHECK(json);CHECK(xjson_get_int(json,"seq")==++out->seq);xjson_free(json);
    if (out->reentry) {
        CHECK(xdap_adapter_free(out->adapter)==XR_XIR_CALL_BUSY);
        CHECK(xdap_adapter_request(out->adapter,"{}",2)==XR_XIR_BAD_STRUCTURE);
    }
    return XR_XIR_OK;
}
static XrJsonValue *last_response(const char *command,bool success) {
    for (uint32_t i=capture.count;i;--i) {
        Message *message=&capture.messages[i-1];XrJsonValue *json=xjson_parse(message->bytes,message->size);CHECK(json);
        if (!strcmp(xjson_get_string(json,"type"),"response") && !strcmp(xjson_get_string(json,"command"),command)) {
            if(xjson_get_bool(json,"success")!=success)
                fprintf(stderr,"Unexpected %s response (success=%u): %s\n",command,(unsigned)success,message->bytes);
            CHECK(xjson_get_bool(json,"success")==success);return json;
        }
        xjson_free(json);
    }
    CHECK(false);return NULL;
}
static void request(XdapAdapter *adapter,const char *text) {
    CHECK(xdap_adapter_request(adapter,text,strlen(text))==XR_XIR_OK);
}
static uint32_t event_count(const char *event) {
    uint32_t count=0;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *json=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(json);
        const char *actual=xjson_get_string(json,"event");if (actual && !strcmp(actual,event)) ++count;
        xjson_free(json);
    }
    return count;
}
int main(void) {
    xr_test_suppress_dialogs();XrXirCompileContext context={0};
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_FINITE,8388608,true};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats baseline={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig compiler_config={context,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,
        xr_source_compile_default_manifest_limits()};XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&compiler_config,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=domain,.write=capture_message,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    XdapAdapter *adapter=NULL;CHECK(xdap_adapter_new(&compiler,&config,&adapter)==XR_XIR_OK && !compiler);
    capture.adapter=adapter;capture.reentry=true;
    request(adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"supportsVariableType\":true,\"supportsVariablePaging\":true}}");
    CHECK(event_count("initialized")==1);
    XrJsonValue *reply=last_response("initialize",true);xjson_free(reply);
    char message[4096];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",XR_DAP_FIXTURE);
    request(adapter,message);reply=last_response("launch",true);xjson_free(reply);
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"leaf\"}]}}");
    reply=last_response("setFunctionBreakpoints",true);CHECK(xjson_get_bool(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"breakpoints"),0),"verified"));xjson_free(reply);
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && !xdap_adapter_running(adapter));CHECK(event_count("stopped")==1);
    request(adapter,"{\"seq\":5,\"type\":\"request\",\"command\":\"stackTrace\",\"arguments\":{\"threadId\":1}}");
    reply=last_response("stackTrace",true);XrJsonValue *frames=xjson_get_array(xjson_get_object(reply,"body"),"stackFrames");
    CHECK(xjson_array_len(frames)==2);XrJsonValue *frame=xjson_array_get(frames,0);
    CHECK(!strcmp(xjson_get_string(frame,"name"),"leaf") && xjson_get_int(frame,"line")==2 && xjson_get_int(frame,"column")==9);
    CHECK(xjson_get_int(xjson_array_get(frames,1),"line")==7);
    XrJsonValue *source=xjson_get_object(frame,"source");
    CHECK(!strcmp(xjson_get_string(source,"name"),"wire_fixture.xr") && strstr(xjson_get_string(source,"path"),"wire_fixture.xr"));
    int64_t handle=xjson_get_int(frame,"id");char position[64];snprintf(position,sizeof(position),"%s",xjson_get_string(frame,"instructionPointerReference"));xjson_free(reply);
    snprintf(message,sizeof(message),"{\"seq\":6,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%lld}}",(long long)handle);
    request(adapter,message);reply=last_response("scopes",true);
    int64_t scope=xjson_get_int(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"scopes"),0),"variablesReference");xjson_free(reply);
    snprintf(message,sizeof(message),"{\"seq\":7,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%lld}}",(long long)scope);
    request(adapter,message);reply=last_response("variables",true);
    XrJsonValue *variables=xjson_get_array(xjson_get_object(reply,"body"),"variables");CHECK(xjson_array_len(variables)==1);
    CHECK(!strcmp(xjson_get_string(xjson_array_get(variables,0),"value"),"20"));xjson_free(reply);
    snprintf(message,sizeof(message),"{\"seq\":8,\"type\":\"request\",\"command\":\"disassemble\",\"arguments\":{\"memoryReference\":\"%s\",\"instructionCount\":2}}",position);
    request(adapter,message);reply=last_response("disassemble",true);
    CHECK(xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"instructions"))==2);xjson_free(reply);
    request(adapter,"{\"seq\":9,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1,\"granularity\":\"instruction\"}}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("stopped")==2);
    snprintf(message,sizeof(message),"{\"seq\":10,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%lld}}",(long long)handle);
    request(adapter,message);reply=last_response("scopes",false);xjson_free(reply);
    request(adapter,"{\"seq\":11,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("terminated")==1 && event_count("exited")==1);
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && event_count("terminated")==1);
    bool output=false;
    for (uint32_t i=0;i<capture.count;++i) {
        reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) { XrJsonValue *bytes=xjson_get(xjson_get_object(reply,"body"),"output");
            CHECK(bytes->string_len==12 && !memcmp(bytes->as.string,"owned\0value\n",12));output=true; }
        xjson_free(reply);
    }
    CHECK(output);request(adapter,"{\"seq\":12,\"type\":\"request\",\"command\":\"disconnect\"}");
    CHECK(xdap_adapter_closed(adapter));capture.reentry=false;CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    xr_source_compile_diagnostic_free(&diagnostic);CHECK(!xr_xir_domain_budget_stats(domain).metadata_live);xr_xir_domain_drop(domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.held_bytes==baseline.held_bytes);xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes);
    puts("Real DAP JSON launch/configure/function stop/stack/variables/disassembly/step/output/lifetime: PASS");return 0;
}
