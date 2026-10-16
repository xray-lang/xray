/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_global_core_helpers.inc.c - Genuine module frames and controlled writes
 */
typedef struct GlobalFixture { XdapAdapter *adapter;XrCompileResources *ledger;XrXirDomain *domain;XrCompileResourceStats baseline; } GlobalFixture;
static XrXirStatus global_message(void *context,const char *bytes,size_t length) {
    Capture *out=context;CHECK(out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);message->bytes[length]=0;
    message->size=length;++out->seq;return XR_XIR_OK;
}
static GlobalFixture global_open(const char *file) {
    GlobalFixture fixture={0};memset(&capture,0,sizeof(capture));XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&fixture.ledger)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_stats(fixture.ledger,&fixture.baseline)==XR_COMPILE_RESOURCE_OK);
    XrSourceCompilerConfig source={{fixture.ledger,xr_xir_compile_default_limits()},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&source,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);xr_source_compile_diagnostic_free(&diagnostic);
    XrXirDomainBudgetControls budget={1048576,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&budget,&fixture.domain)==XR_XIR_VALUE_OK);
    XdapAdapterConfig config={.observer_metadata=fixture.domain,.write=global_message,.context=&capture,.message_limit=16384};
    CHECK(xr_xir_instance_config_init(&config.instance,sizeof(config.instance))==XR_XIR_CALL_READY);
    config.instance.metadata_limit=config.instance.value_limit=config.instance.call_limit=1048576;
    config.instance.requested_value_limit=config.instance.requested_call_limit=1048576;
    config.instance.work_limit=128000000;config.instance.poll_limit=10000;config.instance.depth_limit=64;
    CHECK(xdap_adapter_new(&compiler,&config,&fixture.adapter)==XR_XIR_OK && !compiler);
    request(fixture.adapter,"{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}");
    char message[1024];snprintf(message,sizeof(message),"{\"seq\":2,\"type\":\"request\",\"command\":\"launch\",\"arguments\":{\"program\":\"%s\"}}",file);
    request(fixture.adapter,message);XrJsonValue *reply=last_response("launch",true);xjson_free(reply);capture.count=0;return fixture;
}
static void global_pause_at(GlobalFixture *fixture,uint32_t line) {
    char request_text[1024];snprintf(request_text,sizeof(request_text),
        "{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"" XR_DAP_GLOBAL_LIB_FIXTURE "\"},\"breakpoints\":[{\"line\":%u}]}}",line);
    request(fixture->adapter,request_text);
    request(fixture->adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    XrXirStatus status=xdap_adapter_step(fixture->adapter,1000);
    if (status!=XR_XIR_OK || !fixture->adapter->snapshot || fixture->adapter->running) {
        fprintf(stderr,"global stop status=%u running=%u epoch=%llu\n",status,(unsigned)fixture->adapter->running,
            (unsigned long long)xdap_current_stopped_epoch(fixture->adapter->launch->control));
        for (uint32_t i=0;i<capture.count;++i) fprintf(stderr,"%s\n",capture.messages[i].bytes);
    }
    CHECK(status==XR_XIR_OK && fixture->adapter->snapshot && !fixture->adapter->running);capture.count=0;
}
static void global_pause(GlobalFixture *fixture) { global_pause_at(fixture,5); }
static XrXirVmDebugFrameInfo global_frame(const XrXirVmDebugSnapshot *snapshot,uint32_t frame) {
    XrXirVmDebugFrameInfo info={0};XrXirValue name={0};
    CHECK(xr_xir_vm_debug_snapshot_frame(snapshot,frame,&info,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);return info;
}
static uint32_t global_slot(const XdapLaunch *launch,const XrXirVmDebugSnapshot *snapshot,uint32_t frame,const char *name) {
    XrXirVmDebugFrameInfo info=global_frame(snapshot,frame);CHECK(info.function<launch->function_count);
    uint32_t module=launch->functions[info.function].module;CHECK(module<launch->module_count);
    uint32_t query=launch->modules[module].query_module,found=UINT32_MAX;
    const XrXirSourceView *view=xr_xir_compile_source_product_view(launch->source);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(launch->source);
    for (uint32_t g=0;g<debug->global_count;++g) {
        const XrXirSourceDeclaration *declaration=&view->declarations[debug->globals[g].declaration-1];
        if (declaration->range.module==query && !strcmp(declaration->name,name)) { CHECK(found==UINT32_MAX);found=debug->globals[g].slot; }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void global_number(const XrXirVmDebugSnapshot *snapshot,uint32_t frame,uint32_t slot,int64_t expected) {
    XrXirValue value={0};CHECK(xir_vm_debug_snapshot_slot(snapshot,frame,slot,&value)==XR_XIR_VALUE_OK);
    CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==expected);xr_xir_value_drop(&value);
}
static void global_return(GlobalFixture *fixture,uint64_t epoch,const char *expected) {
    XdapCurrent *control=fixture->adapter->launch->control;
    CHECK(xdap_current_continue(control,epoch,XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);XdapCurrentResult result={0};
    for (uint32_t i=0;i<32;++i) {
        CHECK(xdap_current_poll(control,1000,&result)==XR_XIR_VALUE_OK);
        if (result.state) break;xdap_current_result_drop(&result);
    }
    CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_RETURNED);xdap_current_result_drop(&result);
    bool output=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
        if (event && !strcmp(event,"output")) { CHECK(!output && !strcmp(xjson_get_string(xjson_get_object(message,"body"),"output"),expected));output=true; }
        xjson_free(message);
    }
    CHECK(output);
}
static void global_finish(GlobalFixture *fixture) {
    CHECK(!xr_xir_domain_budget_stats(fixture->domain).metadata_live);xr_xir_domain_drop(fixture->domain);
    XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==fixture->baseline.live_bytes);xr_compile_resources_release(fixture->ledger);CHECK(!runtime_live && !runtime_bytes);
}
typedef struct GlobalWriteTarget { uint64_t epoch;uint32_t frame,slot; } GlobalWriteTarget;
static XrXirValueStatus global_write(GlobalFixture *fixture,const GlobalWriteTarget *target,
    const XrXirValuePath *path,const XrXirValue *value,XrXirVmDebugSnapshot **fresh) {
    XdapCurrent *control=fixture->adapter->launch->control;
    XrXirVmDebugFrameInfo info=global_frame(fixture->adapter->snapshot,target->frame);
    CHECK(xdap_current_write_slot_begin(control,target->epoch,info.id,target->slot,path,value)==XR_XIR_VALUE_OK);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(control,1,&result)==XR_XIR_VALUE_OK && result.state==XDAP_CURRENT_STOPPED);
    bool visited=false;XrXirValueStatus status=xdap_current_write_finish(control,&visited,fresh);CHECK(visited);xdap_current_result_drop(&result);return status;
}
