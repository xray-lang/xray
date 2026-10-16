/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_evaluate.c - Source expressions over real paused frames
 */
#define main reference_evaluate_dispatch_main
#include "test_dispatch.c"
#undef main
#include "app/dap/xdap_dispatch_internal.h"
typedef struct EvaluateFixture { XdapAdapter *adapter;XrCompileResources *ledger;XrXirDomain *domain;XrCompileResourceStats baseline; } EvaluateFixture;
static XrXirStatus evaluate_write(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;
    message->size=length;++out->seq;return XR_XIR_OK;
}
static EvaluateFixture evaluate_open(const char *file) {
    EvaluateFixture fixture={0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture.ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture.ledger,&fixture.baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture.ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture.domain,.write=evaluate_write,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);
    request(fixture.adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    request(fixture.adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);capture.count=0;return fixture;
}
static void evaluate_finish(EvaluateFixture *fixture) {
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
static void evaluate_dispose(EvaluateFixture *fixture) { CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_READY);evaluate_finish(fixture); }
static void evaluate_start(XdapAdapter *adapter) {
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_CALL_FIXTURE "\"},\"breakpoints\":[{\"line\":4}]}}");
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);capture.count=0;
}
static void evaluate_context(XdapAdapter *adapter,const char *expression,uint32_t frame,const char *expected,const char *context) {
    char message[1024],option[128]={0};if (context) snprintf(option,sizeof(option),",\"context\":%s",context);
    snprintf(message,sizeof(message),"{\"seq\":5,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"%s\",\"frameId\":%u%s}}",expression,frame,option);
    XrXirStatus requested=xdap_adapter_request(adapter,message,strlen(message));
    if (requested!=XR_XIR_OK) fprintf(stderr,"Evaluate request [%s] status=%d\n",expression,(int)requested);
    if (requested!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(adapter->resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Evaluate work=%llu allocated=%llu live=%llu peak=%llu\n",
            (unsigned long long)stats.work,(unsigned long long)stats.allocated_bytes,
            (unsigned long long)stats.live_bytes,(unsigned long long)stats.peak_bytes);
    }
    CHECK(requested==XR_XIR_OK);
    XrJsonValue *last=capture.count ? xjson_parse(capture.messages[capture.count-1].bytes,capture.messages[capture.count-1].size) : NULL;
    if (!last || xjson_get_bool(last,"success")!=(expected!=NULL))
        fprintf(stderr,"Evaluate [%s] expected=%s response=%s\n",expression,expected ? expected : "DENIED",capture.count ? capture.messages[capture.count-1].bytes : "NONE");
    xjson_free(last);XrJsonValue *reply=last_response("evaluate",expected!=NULL);
    if (expected) CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"result"),expected));
    xjson_free(reply);capture.count=0;
}
static void evaluate_request(XdapAdapter *adapter,const char *expression,uint32_t frame,const char *expected) {
    evaluate_context(adapter,expression,frame,expected,NULL);
}
static void evaluate_normal(void) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;evaluate_start(adapter);
    if (adapter->depth!=3) fprintf(stderr,"Original call stack depth=%u\n",adapter->depth);
    CHECK(adapter->depth==3);uint32_t inner=adapter->frames[0].handle,outer=adapter->frames[1].handle;
    for (uint32_t i=0;i<2;++i) {
        XrXirVmDebugFrameInfo frame={0};XrXirValue name={0};
        CHECK(xr_xir_vm_debug_snapshot_frame(adapter->snapshot,i,&frame,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
        CHECK(!strcmp(adapter->launch->functions[frame.function].name,i ? "outer" : "inner"));
    }
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    evaluate_context(adapter,"doubled + 2",inner,"42","\"hover\"");
    evaluate_context(adapter,"label.contains(\\\"nn\\\")",inner,"true","\"hover\"");
    evaluate_context(adapter,"input + value",outer,"39","\"hover\"");
    evaluate_context(adapter,"doubled = 99",inner,NULL,"\"hover\"");
    evaluate_context(adapter,"print(value)",inner,NULL,"\"hover\"");
    evaluate_context(adapter,"doubled",inner,"40","\"watch\"");
    evaluate_context(adapter,"doubled",inner,"40","\"custom-client-context\"");
    evaluate_context(adapter,"doubled",inner,NULL,"7");evaluate_context(adapter,"doubled",inner,NULL,"null");
    evaluate_context(adapter,"len(label)",inner,"5","\"hover\"");
    evaluate_request(adapter,"len(11)",inner,NULL);evaluate_request(adapter,"len(label,label)",inner,NULL);
    evaluate_request(adapter,"len<i64>([1])",inner,NULL);evaluate_request(adapter,"len",inner,NULL);
    evaluate_request(adapter,"doubled + 2",inner,"42");evaluate_request(adapter,"doubled",inner,"40");
    evaluate_request(adapter,"value",inner,"20");evaluate_request(adapter,"input + value",outer,"39");
    evaluate_request(adapter,"doubled == 40 && value == 20",inner,"true");
    evaluate_request(adapter,"false && (1/0 == 0)",inner,"false");
    evaluate_request(adapter,"((doubled+2)*3-2)/2",inner,"62");
    evaluate_request(adapter,"doubled>1 ? doubled+2 : value",inner,"42");
    evaluate_request(adapter,"label",inner,"inner");evaluate_request(adapter,"(label)",inner,"inner");
    evaluate_request(adapter,"label + \\\"!\\\"",inner,"inner!");
    evaluate_request(adapter,"label == \\\"inner\\\"",inner,"true");
    evaluate_request(adapter,"label.length + 1",inner,NULL);
    evaluate_request(adapter,"label[0] == 'i'",inner,NULL);
    evaluate_request(adapter,"'i' == 'i'",inner,"true");
    evaluate_request(adapter,"1.25 + 0.5",inner,"1.75");
    evaluate_request(adapter,"[\\\"a\\\",\\\"b\\\"][1] + label",inner,"binner");
    evaluate_request(adapter,"label.contains(\\\"nn\\\")",inner,"true");
    evaluate_request(adapter,"label.indexOf(\\\"n\\\",2)",inner,"2");
    evaluate_request(adapter,"label.endsWith(\\\"er\\\")",inner,"true");
    evaluate_request(adapter,"label.contains(3)",inner,NULL);
    evaluate_request(adapter,"label.append(\\\"x\\\")",inner,NULL);
    evaluate_request(adapter,"value // trailing comment",inner,"20");
    const char *rejected[]={"input","missing","1 +","1; print(2)","doubled = 99","print(value)","doubled / 0"};
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) evaluate_request(adapter,rejected[i],inner,NULL);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running);
    evaluate_request(adapter,"doubled",inner,"40");
    request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal && event_count("output")==1);
    bool output=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(message);
        if (xjson_get_string(message,"event") && !strcmp(xjson_get_string(message,"event"),"output")) {
            CHECK(!strcmp(xjson_get_string(xjson_get_object(message,"body"),"output"),"44\n"));output=true;
        }
        xjson_free(message);
    }
    CHECK(output);capture.count=0;evaluate_request(adapter,"doubled",inner,NULL);evaluate_dispose(&fixture);
}
static size_t evaluate_fault(size_t fail) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;evaluate_start(adapter);
    char message[256];snprintf(message,sizeof(message),"{\"seq\":5,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"doubled+2\",\"context\":\"hover\",\"frameId\":%u}}",adapter->frames[0].handle);
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
    evaluate_dispose(&fixture);return sites;
}
#include "test_evaluate_data.inc.c"
#include "test_evaluate_intrinsic.inc.c"
#ifndef XDAP_EVALUATE_MAIN
#define XDAP_EVALUATE_MAIN main
#endif
int XDAP_EVALUATE_MAIN(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");
    bool managed_resources=argc==2 && !strcmp(argv[1],"--managed-resources");
    bool managed=managed_resources || (argc==2 && !strcmp(argv[1],"--managed"));CHECK(argc==1 || resources || managed);
    if (managed) { evaluate_data();evaluate_data_failures(managed_resources);puts("Source data expressions, exact bits/NUL/nested Nullable and producer disposal PASS");return 0; }
    evaluate_normal();evaluate_intrinsic_scopes();size_t sites=evaluate_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t i=0;i<sites;++i) evaluate_fault(i);
    else { evaluate_fault(0);if (sites>1) evaluate_fault(sites-1); }
    printf("Original call_stack Source evaluate42, doubled40 and final44 unchanged, caller scope/booleans/rejections/retired refs; sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");return 0;
}
