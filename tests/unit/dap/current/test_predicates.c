/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_predicates.c - Conditions and logpoints at actual Source stops
 */
#define main reference_predicate_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"

typedef struct PredicateFixture { XdapAdapter *adapter;XrCompileResources *ledger;XrXirDomain *domain;XrCompileResourceStats baseline; } PredicateFixture;
static XrXirStatus predicate_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;
    message->size=length;++out->seq;return XR_XIR_OK;
}
static PredicateFixture predicate_open(const char *file) {
    PredicateFixture fixture={0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture.ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture.ledger,&fixture.baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture.ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture.domain,.write=predicate_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);
    request(fixture.adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    XrJsonValue *initialized=last_response("initialize",true),*capabilities=xjson_get_object(initialized,"body");
    CHECK(xjson_get_bool(capabilities,"supportsConditionalBreakpoints") && xjson_get_bool(capabilities,"supportsLogPoints"));xjson_free(initialized);
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    request(fixture.adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);capture.count=0;return fixture;
}
static void predicate_close(PredicateFixture *fixture) {
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static size_t predicate_message(char *output,size_t capacity,const char *file,const char *entries) {
    int length=snprintf(output,capacity,"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[%s]}}",file,entries);
    CHECK(length>0 && (size_t)length<capacity);return (size_t)length;
}
static void predicate_set(XdapAdapter *adapter,const char *file,const char *entries,bool accepted) {
    char message[2048];predicate_message(message,sizeof(message),file,entries);request(adapter,message);
    XrJsonValue *reply=last_response("setBreakpoints",accepted);
    if (accepted && entries[0]) CHECK(xjson_get_bool(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"breakpoints"),0),"verified"));
    xjson_free(reply);capture.count=0;
}
static void predicate_run(XdapAdapter *adapter,const char *command) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":4,\"type\":\"request\",\"command\":\"%s\",\"arguments\":{\"threadId\":1}}",command);
    request(adapter,message);capture.count=0;
}
static void predicate_pump(XdapAdapter *adapter) {
    for (uint32_t i=0;i<64 && adapter->running;++i) {
        uint32_t next=adapter->next_handle;uint64_t epoch=adapter->stop_epoch;
        CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK);
        if (adapter->running) CHECK(!adapter->snapshot && !adapter->depth && adapter->next_handle==next && adapter->stop_epoch==epoch);
    }
    CHECK(!adapter->running);
}
static void predicate_value(XdapAdapter *adapter,const char *name,const char *expected) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":5,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"%s\"}}",name);
    request(adapter,message);XrJsonValue *reply=last_response("evaluate",true);
    CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"result"),expected));xjson_free(reply);
}
static void predicate_output(uint32_t number,const char *category,const char *expected,size_t bytes) {
    uint32_t found=0;bool matched=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *json=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(json);
        const char *event=xjson_get_string(json,"event");
        if (event && !strcmp(event,"output") && found++==number) {
            XrJsonValue *body=xjson_get_object(json,"body"),*output=xjson_get(body,"output");
            CHECK(!strcmp(xjson_get_string(body,"category"),category));
            CHECK(output && output->string_len==bytes && !memcmp(output->as.string,expected,bytes));matched=true;
        }
        xjson_free(json);
    }
    CHECK(matched);
}
static void predicate_conditions(void) {
    for (uint32_t hits=0;hits<2;++hits) {
        PredicateFixture fixture=predicate_open(XR_DAP_LOOP_FIXTURE);XdapAdapter *adapter=fixture.adapter;
        predicate_set(adapter,XR_DAP_LOOP_FIXTURE,hits ? "{\"line\":4,\"condition\":\"index == 2\",\"hitCondition\":\"==3\"}" :
            "{\"line\":4,\"condition\":\"index == 2 && sum == 1\"}",true);
        char *condition=adapter->source_breakpoints[0].predicate.condition;
        const char *invalid[]={"{\"line\":4,\"condition\":\"index = 9\"}","{\"line\":4,\"condition\":\"1; print(9)\"}",
            "{\"line\":4,\"logMessage\":\"bad {index\"}","{\"line\":4,\"logMessage\":\"{print(index)}\"}"};
        for (uint32_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
            predicate_set(adapter,XR_DAP_LOOP_FIXTURE,invalid[i],false);CHECK(adapter->source_breakpoints[0].predicate.condition==condition);
        }
        predicate_run(adapter,"configurationDone");predicate_pump(adapter);
        CHECK(adapter->snapshot && event_count("stopped")==1 && !event_count("output"));
        predicate_value(adapter,"index","2");predicate_value(adapter,"sum","1");
        predicate_run(adapter,"continue");predicate_pump(adapter);
        CHECK(adapter->terminal && event_count("stopped")==0 && event_count("output")==1);predicate_output(0,"stdout","6\n",2);
        predicate_close(&fixture);
    }
}
static void predicate_logs(void) {
    PredicateFixture fixture=predicate_open(XR_DAP_LOOP_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    predicate_set(adapter,XR_DAP_LOOP_FIXTURE,"{\"line\":4,\"logMessage\":\"i={index},sum={sum} {{ok}}\"}",true);
    predicate_run(adapter,"configurationDone");predicate_pump(adapter);
    CHECK(adapter->terminal && event_count("stopped")==0 && event_count("output")==5);
    const char *expected[]={"i=0,sum=0 {ok}\n","i=1,sum=0 {ok}\n","i=2,sum=1 {ok}\n","i=3,sum=3 {ok}\n"};
    for (uint32_t i=0;i<4;++i) predicate_output(i,"console",expected[i],strlen(expected[i]));predicate_output(4,"stdout","6\n",2);
    predicate_run(adapter,"restart");predicate_pump(adapter);CHECK(adapter->terminal && !event_count("stopped") && event_count("output")==5);
    predicate_set(adapter,XR_DAP_LOOP_FIXTURE,"{\"line\":4,\"condition\":\"index == 2\",\"logMessage\":\"sum={sum+index}\"}",true);
    predicate_run(adapter,"restart");predicate_pump(adapter);CHECK(adapter->terminal && event_count("output")==2);
    predicate_output(0,"console","sum=3\n",6);predicate_output(1,"stdout","6\n",2);predicate_close(&fixture);
    fixture=predicate_open(XR_DAP_FIXTURE);adapter=fixture.adapter;
    predicate_set(adapter,XR_DAP_FIXTURE,"{\"line\":4,\"logMessage\":\"{text}\"}",true);
    predicate_run(adapter,"configurationDone");predicate_pump(adapter);CHECK(adapter->terminal && !event_count("stopped"));
    predicate_output(0,"console","owned\0value\n",12);predicate_close(&fixture);
    fixture=predicate_open(XR_DAP_LOOP_FIXTURE);adapter=fixture.adapter;
    predicate_set(adapter,XR_DAP_LOOP_FIXTURE,"{\"line\":4,\"logMessage\":\"\"}",true);
    predicate_run(adapter,"configurationDone");predicate_pump(adapter);CHECK(adapter->terminal && !event_count("stopped") && event_count("output")==5);
    for (uint32_t i=0;i<4;++i) predicate_output(i,"console","\n",1);predicate_close(&fixture);
}
static void predicate_read_calls(void) {
    PredicateFixture fixture=predicate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    predicate_set(adapter,XR_DAP_CALL_FIXTURE,"{\"line\":4,\"condition\":\"label.contains(\\\"nn\\\") && value == 20\",\"logMessage\":\"match={label.indexOf(\\\"n\\\",2)}\"}",true);
    char *condition=adapter->source_breakpoints[0].predicate.condition;
    predicate_set(adapter,XR_DAP_CALL_FIXTURE,"{\"line\":4,\"condition\":\"label.append(\\\"x\\\")\"}",false);
    CHECK(adapter->source_breakpoints[0].predicate.condition==condition);
    predicate_run(adapter,"configurationDone");predicate_pump(adapter);
    CHECK(adapter->terminal && !event_count("stopped") && event_count("output")==2);
    predicate_output(0,"console","match=2\n",8);predicate_output(1,"stdout","44\n",3);
    predicate_close(&fixture);
}
static void predicate_errors(void) {
    const char *entries[]={"{\"line\":4,\"condition\":\"index\"}","{\"line\":4,\"condition\":\"missing == 0\"}",
        "{\"line\":4,\"logMessage\":\"missing={missing}\"}"};
    for (uint32_t i=0;i<3;++i) {
        PredicateFixture fixture=predicate_open(XR_DAP_LOOP_FIXTURE);XdapAdapter *adapter=fixture.adapter;
        predicate_set(adapter,XR_DAP_LOOP_FIXTURE,entries[i],true);predicate_run(adapter,"configurationDone");predicate_pump(adapter);
        CHECK(adapter->snapshot && event_count("stopped")==1 && event_count("output")==1);
        const char *error=i==2 ? "Logpoint expression could not be evaluated\n" : "Breakpoint condition requires an admitted boolean expression\n";
        predicate_output(0,"stderr",error,strlen(error));predicate_value(adapter,"index","0");predicate_value(adapter,"sum","0");
        predicate_set(adapter,XR_DAP_LOOP_FIXTURE,"",true);predicate_run(adapter,"continue");predicate_pump(adapter);
        CHECK(adapter->terminal);predicate_output(0,"stdout","6\n",2);predicate_close(&fixture);
    }
}
static void predicate_function_and_next(void) {
    PredicateFixture fixture=predicate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"inner\",\"condition\":\"value == 20\"}]}}");
    XrJsonValue *reply=last_response("setFunctionBreakpoints",true);
    CHECK(xjson_get_bool(xjson_array_get(xjson_get_array(xjson_get_object(reply,"body"),"breakpoints"),0),"verified"));xjson_free(reply);
    char *condition=adapter->function_predicates[0].condition;
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"inner\",\"condition\":\"print(value)\"}]}}");
    reply=last_response("setFunctionBreakpoints",false);xjson_free(reply);CHECK(adapter->function_predicates[0].condition==condition);
    predicate_run(adapter,"configurationDone");predicate_pump(adapter);CHECK(adapter->snapshot && adapter->depth==3 && event_count("stopped")==1);
    predicate_value(adapter,"value","20");
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"inner\",\"condition\":\"false\"}]}}");
    reply=last_response("setFunctionBreakpoints",true);xjson_free(reply);
    predicate_set(adapter,XR_DAP_CALL_FIXTURE,"{\"line\":4,\"logMessage\":\"{label}\"}",true);
    predicate_run(adapter,"restart");predicate_pump(adapter);
    CHECK(adapter->terminal && !event_count("stopped") && event_count("output")==2);
    predicate_output(0,"console","inner\n",6);predicate_output(1,"stdout","44\n",3);predicate_close(&fixture);
    fixture=predicate_open(XR_DAP_CALL_FIXTURE);adapter=fixture.adapter;
    predicate_set(adapter,XR_DAP_CALL_FIXTURE,"{\"line\":9},{\"line\":4,\"condition\":\"false\"}",true);
    predicate_run(adapter,"configurationDone");predicate_pump(adapter);CHECK(adapter->snapshot && adapter->depth==2);
    predicate_run(adapter,"next");predicate_pump(adapter);CHECK(adapter->snapshot && adapter->depth==2 && event_count("stopped")==1);
    XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};XrXirSourceDebugPoint point={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(adapter->snapshot,0,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
    CHECK(xdap_launch_source_point(adapter->launch,frame.function,frame.instruction,&point)==XR_XIR_OK && point.range.line==10);
    predicate_close(&fixture);
}
static size_t predicate_fault(size_t fail,unsigned mode) {
    bool probe=mode==1,functions=mode==2;
    PredicateFixture fixture=predicate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    predicate_set(adapter,XR_DAP_CALL_FIXTURE,"{\"line\":4}",true);predicate_run(adapter,"configurationDone");predicate_pump(adapter);
    CHECK(adapter->snapshot);capture.count=0;
    if (probe) predicate_set(adapter,XR_DAP_CALL_FIXTURE,"{\"line\":4,\"logMessage\":\"{label}\"}",true);
    if (functions) request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"inner\"}]}}");
    capture.count=0;
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;uint32_t next=adapter->next_handle;
    char *identity=functions ? adapter->function_breakpoints[0] : adapter->source_breakpoints[0].identity;XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    char message[1024];size_t length=predicate_message(message,sizeof(message),XR_DAP_CALL_FIXTURE,
        "{\"line\":4,\"condition\":\"value == 20\",\"logMessage\":\"{label}\"}");
    if (functions) { strcpy(message,"{\"seq\":3,\"type\":\"request\",\"command\":\"setFunctionBreakpoints\",\"arguments\":{\"breakpoints\":[{\"name\":\"inner\",\"condition\":\"value == 20\"}]}}");length=strlen(message); }
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;bool visible=true;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=probe ? xdap_predicates_probe(adapter,&visible) : xdap_adapter_request(adapter,message,length);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) { CHECK(status==XR_XIR_OK);if (probe) CHECK(!visible && event_count("output")==1); }
    else {
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"predicate fault probe=%u site=%zu status=%u sites=%zu\n",probe,fail,status,sites);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !capture.count && visible);
        CHECK((functions ? adapter->function_breakpoints[0] : adapter->source_breakpoints[0].identity)==identity && runtime_live==live && runtime_bytes==bytes);
    }
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && adapter->next_handle==next && !adapter->running);
    CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.work>before.work && after.allocated_bytes>=before.allocated_bytes);
    if (probe || fail!=SIZE_MAX) CHECK(after.live_bytes==before.live_bytes);
    predicate_close(&fixture);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    predicate_conditions();predicate_logs();predicate_read_calls();predicate_errors();predicate_function_and_next();
    size_t sites[3]={predicate_fault(SIZE_MAX,0),predicate_fault(SIZE_MAX,1),predicate_fault(SIZE_MAX,2)};
    for (unsigned mode=0;mode<3;++mode) {
        CHECK(sites[mode]);if (resources) for (size_t i=0;i<sites[mode];++i) predicate_fault(i,mode);
        else { predicate_fault(0,mode);if (sites[mode]>1) predicate_fault(sites[mode]-1,mode); }
    }
    printf("Source conditions/hits/logpoints, binary text, atomic replacement/restart, diagnostic stops and preserved next; source=%zu probe=%zu function=%zu injection=%s physical=0/0 PASS\n",sites[0],sites[1],sites[2],resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
