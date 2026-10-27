/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_evaluate_visibility.inc.c - Reserved names retain exact live scope
 */
#include "xir/xxir_source_query_internal.h"
static const char visibility_source_text[]="fn probe()->i64 {\n const before:i64=7\n if (before==7) {\n const held:i64=8\n fn len(value:Array<i64>)->i64 {print(\"BAD\");return 99}\n const after:i64=9\n }\n return before\n}\nprint(probe())\n";
static size_t visibility_copy(EvaluateFixture *fixture,const XrXirVmDebugFrameInfo *info,
    uint32_t pending,size_t fail,XrXirSourceSnapshot **escaped) {
    XdapAdapter *adapter=fixture->adapter;const XrXirCompileContext *context=xr_source_compiler_context(adapter->compiler);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(adapter->launch->source);
    const XrXirSourceDebugFunction *function=&debug->functions[info->function];
    XrXirSourceProductPacketView packet={0};XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_source_product_packet(adapter->launch->source,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(context,xr_xir_compile_source_product_view(adapter->launch->source),
        xr_xir_compile_source_product_construction(adapter->launch->source),&snapshot)==XR_XIR_OK);
    XrXirSourceDebugFunction *functions=NULL;XrXirSourceDebugBinding *bindings=NULL;
    size_t fb=(size_t)debug->function_count*sizeof(*functions),bb=(size_t)function->binding_count*sizeof(*bindings);
    CHECK(xr_compile_resources_alloc(fixture->ledger,fb,(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_alloc(fixture->ledger,bb,(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
    memcpy(functions,debug->functions,fb);memcpy(bindings,function->bindings,bb);
    functions[info->function].bindings=bindings;XrXirSourceDebugView candidate=*debug;candidate.functions=functions;
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);XrXirSourceDebugBinding preserved=bindings[pending-1];
    CHECK(preserved.flags==XR_XIR_SOURCE_DEBUG_UNAVAILABLE && preserved.value==UINT32_MAX);
    size_t blocks=runtime_live,bytes=runtime_bytes;XrCompileResourceStats before={0},after={0};
    if (fail==SIZE_MAX) for (uint32_t bad=0;bad<5;++bad) {
        bindings[pending-1]=preserved;
        if (bad==0) bindings[pending-1].flags|=XR_XIR_SOURCE_DEBUG_UNIT;
        else if (bad==1) bindings[pending-1].flags|=XR_XIR_SOURCE_DEBUG_CELL;
        else if (bad==2) bindings[pending-1].value=0;
        else if (bad==3) bindings[pending-1].flags=8;
        else bindings[pending-1].declaration=function->declaration;
        CHECK(xr_compile_resources_stats(fixture->ledger,&before)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_xir_compile_source_snapshot_debug_copy(snapshot,&candidate,module)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_debug(snapshot) && runtime_live==blocks && runtime_bytes==bytes);
        CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    }
    bindings[pending-1]=preserved;size_t begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xr_xir_compile_source_snapshot_debug_copy(snapshot,&candidate,module);
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail!=SIZE_MAX) CHECK(status==XR_XIR_OUT_OF_MEMORY && !xr_xir_compile_source_snapshot_debug(snapshot) && runtime_live==blocks && runtime_bytes==bytes);
    else {
        CHECK(status==XR_XIR_OK);const XrXirSourceDebugView *owned=xr_xir_compile_source_snapshot_debug(snapshot);
        CHECK(owned && owned->functions!=functions && owned->functions[info->function].bindings!=bindings);
        memset(bindings,0,bb);memset(functions,0,fb);
        CHECK(!memcmp(&owned->functions[info->function].bindings[pending-1],&preserved,sizeof(preserved)));
    }
    xr_compile_resources_free(bindings);xr_compile_resources_free(functions);xr_xir_compile_artifact_free(checked);
    if (escaped && status==XR_XIR_OK) *escaped=snapshot;else xr_xir_compile_source_snapshot_free(snapshot);
    return sites;
}
static void evaluate_visibility(bool resources) {
    EvaluateFixture fixture=evaluate_intrinsic_open(visibility_source_text);XdapAdapter *adapter=fixture.adapter;uint32_t phases=0;
    XrXirSourceSnapshot *escaped=NULL;uint32_t escaped_function=0,escaped_binding=0;size_t sites=0;
    for (uint32_t step=0;step<16 && phases!=15;++step) {
        CHECK(adapter->snapshot && !adapter->running);XdapLexicalView lexical={0};
        CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
        XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
        CHECK(xr_xir_vm_debug_snapshot_frame(adapter->snapshot,0,&info,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
        const XrXirSourceDebugPoint *point=&lexical.function->points[info.instruction];uint32_t selected=0;
        for (uint32_t b=lexical.environment;b;b=lexical.function->bindings[b-1].parent) {
            const XrXirSourceDebugBinding *binding=&lexical.function->bindings[b-1];
            const XrXirSourceDeclaration *declaration=&lexical.source->declarations[binding->declaration-1];
            if (!strcmp(declaration->name,"len")) { selected=b;break; }
        }
        uint32_t frame=adapter->frames[0].handle;
        if (point->range.line<=3 || point->range.line==8) {
            CHECK(!selected);evaluate_request(adapter,"len([1,2])",frame,"2");phases|=point->range.line==8 ? 8 : 1;
        } else if (point->range.line==4 || point->range.line==5) {
            CHECK(selected && lexical.function->bindings[selected-1].flags==XR_XIR_SOURCE_DEBUG_UNAVAILABLE);
            XrXirValue value={0};const char *output_name=NULL;size_t length=0;
            CHECK(xdap_lexical_read(&lexical,selected,&value,&output_name,&length)==XR_XIR_VALUE_UNSUPPORTED && !value.type && !value.payload && !output_name && !length);
            evaluate_request(adapter,"len([1,2])",frame,NULL);evaluate_request(adapter,"len",frame,NULL);
            XrXirVmDebugSnapshot *preserved=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;char denied[256];
            snprintf(denied,sizeof(denied),"{\"seq\":19,\"type\":\"request\",\"command\":\"setVariable\",\"arguments\":{\"variablesReference\":%u,\"name\":\"len\",\"value\":\"1\"}}",adapter->frames[0].scope);
            request(adapter,denied);XrJsonValue *reply=last_response("setVariable",false);xjson_free(reply);capture.count=0;
            CHECK(adapter->snapshot==preserved && adapter->stop_epoch==epoch && !adapter->running);phases|=2;
            if (!escaped) {
                sites=visibility_copy(&fixture,&info,selected,SIZE_MAX,&escaped);CHECK(sites);
                if (resources) for (size_t i=0;i<sites;++i) visibility_copy(&fixture,&info,selected,i,NULL);
                else { visibility_copy(&fixture,&info,selected,0,NULL);if (sites>1) visibility_copy(&fixture,&info,selected,sites-1,NULL); }
                escaped_function=info.function;escaped_binding=selected;
            }
        } else {
            CHECK(point->range.line==6 && selected && !lexical.function->bindings[selected-1].flags);
            XrXirValue value={0};const char *output_name=NULL;size_t length=0;
            CHECK(xdap_lexical_read(&lexical,selected,&value,&output_name,&length)==XR_XIR_VALUE_OK && value.type && length==3 && !memcmp(output_name,"len",3));
            xr_xir_value_drop(&value);evaluate_request(adapter,"len([1,2])",frame,NULL);phases|=4;
        }
        CHECK(!capture.count);
        if (phases==15) break;
        char message[256];uint64_t thread=xr_xir_vm_debug_snapshot_stopped_thread(adapter->snapshot);
        snprintf(message,sizeof(message),"{\"seq\":20,\"type\":\"request\",\"command\":\"next\",\"arguments\":{\"threadId\":%llu}}",(unsigned long long)thread);
        request(adapter,message);capture.count=0;CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot);capture.count=0;
    }
    CHECK(phases==15 && escaped);char message[256];
    snprintf(message,sizeof(message),"{\"seq\":21,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":%llu}}",
        (unsigned long long)xr_xir_vm_debug_snapshot_stopped_thread(adapter->snapshot));request(adapter,message);capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);bool output=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *json=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(json);
        const char *event=xjson_get_string(json,"event");
        if (event && !strcmp(event,"output")) { CHECK(!output && !strcmp(xjson_get_string(xjson_get_object(json,"body"),"output"),"7\n"));output=true; }
        xjson_free(json);
    }
    CHECK(output && xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(escaped);
    const XrXirSourceDebugBinding *binding=&debug->functions[escaped_function].bindings[escaped_binding-1];
    const XrXirSourceView *source=xr_xir_compile_source_snapshot_view(escaped);
    CHECK(binding->flags==XR_XIR_SOURCE_DEBUG_UNAVAILABLE && binding->value==UINT32_MAX && !strcmp(source->declarations[binding->declaration-1].name,"len"));
    xr_xir_compile_source_snapshot_free(escaped);evaluate_finish(&fixture);
    printf("Live lexical before/pending/ready/after Core len scopes; no early value authority; five malformed copies; escaped metadata; sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");
}

static size_t visibility_source_fault(size_t fail) {
    EvaluateFixture fixture=evaluate_open(XR_DAP_CALL_FIXTURE);XdapAdapter *adapter=fixture.adapter;evaluate_start(adapter);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
    XrSourceCompilerText text={NULL,"dap-visibility-source-fault",visibility_source_text,sizeof(visibility_source_text)-1};
    XdapLaunchRequest launch_request={.compiler=adapter->compiler,.text=&text,.observer_metadata=fixture.domain,.instance=adapter->config.instance};
    XdapLaunch *candidate=NULL;XrSourceCompileDiagnostic diagnostic={0};size_t begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_launch_build(&launch_request,&candidate,&diagnostic);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail!=SIZE_MAX) CHECK(status==XR_XIR_OUT_OF_MEMORY && !candidate);
    else {
        CHECK(status==XR_XIR_OK && candidate);const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(candidate->source);uint32_t unavailable=0;
        for (uint32_t f=0;f<debug->function_count;++f) for (uint32_t b=0;b<debug->functions[f].binding_count;++b)
            if (debug->functions[f].bindings[b].flags==XR_XIR_SOURCE_DEBUG_UNAVAILABLE) ++unavailable;
        CHECK(unavailable);
    }
    xr_source_compile_diagnostic_free(&diagnostic);CHECK(xdap_launch_free(candidate)==XR_XIR_CALL_READY);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running && !capture.count);
    char message[256];snprintf(message,sizeof(message),"{\"seq\":24,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":%llu}}",
        (unsigned long long)xr_xir_vm_debug_snapshot_stopped_thread(adapter->snapshot));request(adapter,message);capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);bool output=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *json=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(json);const char *event=xjson_get_string(json,"event");
        if (event && !strcmp(event,"output")) { CHECK(!output && !strcmp(xjson_get_string(xjson_get_object(json,"body"),"output"),"44\n"));output=true; }
        xjson_free(json);
    }
    CHECK(output);evaluate_dispose(&fixture);return sites;
}
static void evaluate_visibility_source_failures(bool resources) {
    size_t sites=visibility_source_fault(SIZE_MAX);CHECK(sites);
    if (resources) for (size_t i=0;i<sites;++i) visibility_source_fault(i);
    else { visibility_source_fault(0);if (sites>1) visibility_source_fault(sites-1); }
    printf("Reserved-name Source construction retains original stopped target/Source44; sites=%zu injection=%s physical=0/0 PASS\n",sites,resources ? "ALL" : "FIRST_LAST_ONLY");
}
