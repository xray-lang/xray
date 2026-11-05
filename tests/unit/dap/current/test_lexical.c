/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lexical.c - Real lexical names and owned stop values
 */
#define main reference_lexical_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_lexical.h"
#include "app/dap/xdap_dispatch_internal.h"
typedef struct LexicalFixture { XdapAdapter *adapter;XrCompileResources *ledger;XrXirDomain *domain;XrCompileResourceStats baseline; } LexicalFixture;
static XrXirStatus lexical_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;
    message->size=length;++out->seq;return XR_XIR_OK;
}
static LexicalFixture lexical_open(const char *file) {
    LexicalFixture fixture={0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture.ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture.ledger,&fixture.baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture.ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,NULL,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture.domain,.write=lexical_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);
    request(fixture.adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\",\"arguments\":{\"supportsVariablePaging\":true}}");
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    request(fixture.adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);capture.count=0;return fixture;
}
static void lexical_finish(LexicalFixture *fixture) {
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void lexical_dispose(LexicalFixture *fixture) { CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);lexical_finish(fixture); }
static void lexical_start(XdapAdapter *adapter,uint32_t line) {
    char message[1024];snprintf(message,sizeof(message),
        "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_LEXICAL_FIXTURE "\"},\"breakpoints\":[{\"line\":%u}]}}",line);
    request(adapter,message);request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);capture.count=0;
}
static uint32_t lexical_scope(XdapAdapter *adapter) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":5,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%u}}",adapter->frames[0].handle);
    request(adapter,message);XrJsonValue *reply=last_response("scopes",true);
    XrJsonValue *scope=xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"scopes"),0);
    CHECK(!strcmp(xjson_get_string(scope,"name"),"Locals"));uint32_t handle=(uint32_t)xjson_get_int(scope,"variablesReference");
    CHECK(handle);xjson_free(reply);capture.count=0;return handle;
}
static XrJsonValue *lexical_variables(XdapAdapter *adapter,uint32_t scope,uint32_t start,uint32_t count) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":6,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%u,\"start\":%u,\"count\":%u}}",scope,start,count);
    request(adapter,message);XrJsonValue *reply=last_response("variables",true);capture.count=0;return reply;
}
static XrJsonValue *lexical_find(XrJsonValue *reply,const char *name) {
    XrJsonValue *variables=xjson_get_array(xjson_get_object(reply,"body"),"variables"),*found=NULL;
    for (int i=0;i<xjson_array_len(variables);++i) {
        XrJsonValue *value=xjson_array_get(variables,i);
        if (!strcmp(xjson_get_string(value,"name"),name)) { CHECK(!found);found=value; }
    }
    return found;
}
static void lexical_expect(XrJsonValue *reply,const char *name,const char *expected) {
    XrJsonValue *value=lexical_find(reply,name);CHECK(value && !strcmp(xjson_get_string(value,"value"),expected));
}
static void lexical_next(XdapAdapter *adapter) {
    request(adapter,"{\"seq\":7,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":1,\"granularity\":\"line\"}}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);capture.count=0;
}
static void lexical_normal(void) {
    LexicalFixture fixture=lexical_open(XR_DAP_LEXICAL_FIXTURE);XdapAdapter *adapter=fixture.adapter;lexical_start(adapter,2);
    XrJsonValue *reply=lexical_variables(adapter,lexical_scope(adapter),0,0);
    CHECK(xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables"))==1 && lexical_find(reply,"value") && !lexical_find(reply,"outer"));xjson_free(reply);
    for (uint32_t i=0;i<5;++i) lexical_next(adapter);
    uint32_t scope=lexical_scope(adapter);reply=lexical_variables(adapter,scope,0,0);
    CHECK(xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables"))==5);
    lexical_expect(reply,"value","20");lexical_expect(reply,"outer","22");lexical_expect(reply,"total","21");lexical_expect(reply,"nothing","()");
    XrJsonValue *text=xjson_get(lexical_find(reply,"text"),"value");CHECK(text && text->string_len==13 && !memcmp(text->as.string,"owned\0lexical",13));xjson_free(reply);
    reply=lexical_variables(adapter,scope,1,1);
    CHECK(xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables"))==1 && lexical_find(reply,"total"));xjson_free(reply);
    XdapLexicalView view={0};CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&view)==XR_XIR_OK);
    XrXirValue escaped={0};bool retained=false;uint32_t text_binding=0;
    for (uint32_t b=view.environment;b;b=view.function->bindings[b-1].parent) {
        const char *name=NULL;size_t length=0;XrXirValue value={0};CHECK(xdap_lexical_read(&view,b,&value,&name,&length)==XR_XIR_VALUE_OK);
        if (length==4 && !memcmp(name,"text",4)) { escaped=value;retained=true;text_binding=b; }else xr_xir_value_drop(&value);
    }
    CHECK(retained);const char *name=(const char *)(uintptr_t)1;size_t length=777;XrXirValue empty={0};
    CHECK(xdap_lexical_read(&view,view.function->binding_count+1,&empty,&name,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!empty.type && !empty.payload && name==(const char *)(uintptr_t)1 && length==777);
    lexical_next(adapter);CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&view)==XR_XIR_OK);
    CHECK(xdap_lexical_read(&view,text_binding,&empty,&name,&length)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(!empty.type && !empty.payload && name==(const char *)(uintptr_t)1 && length==777);
    reply=lexical_variables(adapter,lexical_scope(adapter),0,0);
    CHECK(xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables"))==3);
    lexical_expect(reply,"outer","21");lexical_expect(reply,"total","22");
    CHECK(!lexical_find(reply,"text") && !lexical_find(reply,"nothing") && !lexical_find(reply,"done"));xjson_free(reply);
    lexical_next(adapter);reply=lexical_variables(adapter,lexical_scope(adapter),0,0);lexical_expect(reply,"done","22");xjson_free(reply);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);const char *bytes=NULL;
    CHECK(xr_xir_string_view(&escaped,&bytes,&length) && length==13 && !memcmp(bytes,"owned\0lexical",13));xr_xir_value_drop(&escaped);lexical_finish(&fixture);
}
static size_t lexical_fault(size_t fail) {
    LexicalFixture fixture=lexical_open(XR_DAP_LEXICAL_FIXTURE);XdapAdapter *adapter=fixture.adapter;lexical_start(adapter,8);
    uint32_t scope=lexical_scope(adapter);char message[256];snprintf(message,sizeof(message),
        "{\"seq\":6,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%u}}",scope);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,strlen(message));size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) CHECK(status==XR_XIR_OK);else CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !capture.count);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running && !adapter->value_count);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes && after.work>before.work && after.allocated_bytes>=before.allocated_bytes);
    lexical_dispose(&fixture);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    lexical_normal();size_t sites=lexical_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t point=0;point<sites;++point) lexical_fault(point);
    else { lexical_fault(0);if (sites>1) lexical_fault(sites-1); }
    printf("Real lexical names, shadowing, block exit, Cell/Unit/NUL values, paging, owned escape; variables sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
