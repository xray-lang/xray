/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_globals_faults.inc.c - Global requests preserve live owners before commit
 */
typedef struct GlobalsSaved {
    XrXirVmDebugSnapshot *snapshot;
    XrXirVmDebugFrameInfo frame;
    XdapStopFrame frames[64];
    XdapValueRef values[128];
    XrCompileResourceStats compiler;
    size_t live,bytes;
    uint32_t count,next,depth;
} GlobalsSaved;
static GlobalsSaved globals_save(GlobalFixture *fixture) {
    XdapAdapter *adapter=fixture->adapter;GlobalsSaved saved={0};
    CHECK(xdap_current_snapshot(adapter->launch->control,&saved.snapshot)==XR_XIR_VALUE_OK);
    saved.frame=global_frame(saved.snapshot,1);saved.count=adapter->value_count;
    saved.next=adapter->next_handle;saved.depth=adapter->depth;
    memcpy(saved.frames,adapter->frames,sizeof(saved.frames));memcpy(saved.values,adapter->values,sizeof(saved.values));
    CHECK(xr_compile_resources_stats(fixture->ledger,&saved.compiler)==XR_COMPILE_RESOURCE_OK);
    saved.live=runtime_live;saved.bytes=runtime_bytes;return saved;
}
static void globals_unchanged(GlobalFixture *fixture,const GlobalsSaved *saved,bool query) {
    XdapAdapter *adapter=fixture->adapter;XrCompileResourceStats after={0};
    CHECK(adapter->snapshot==saved->snapshot && adapter->stop_epoch==saved->frame.epoch && !adapter->running);
    CHECK(adapter->depth==saved->depth && adapter->value_count==saved->count);
    CHECK(!memcmp(adapter->frames,saved->frames,sizeof(saved->frames)) && !memcmp(adapter->values,saved->values,sizeof(saved->values)));
    CHECK(query ? adapter->next_handle>=saved->next : adapter->next_handle==saved->next);
    XrXirVmDebugFrameInfo frame=global_frame(adapter->snapshot,1);
    CHECK(frame.id==saved->frame.id && frame.thread==saved->frame.thread && frame.instruction==saved->frame.instruction && frame.epoch==saved->frame.epoch);
    CHECK(runtime_live==saved->live && runtime_bytes==saved->bytes);
    CHECK(xr_compile_resources_stats(fixture->ledger,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==saved->compiler.live_bytes &&
        after.work>=saved->compiler.work && after.allocated_bytes>=saved->compiler.allocated_bytes);
}
static void globals_prior(GlobalFixture *fixture,const XrXirVmDebugSnapshot *snapshot,unsigned mode) {
    XdapAdapter *adapter=fixture->adapter;const char *name=mode==3 ? "pair" : mode==2 ? "values" : "changing";
    uint32_t slot=global_slot(adapter->launch,snapshot,1,name);XrXirValue value={0};
    CHECK(xir_vm_debug_snapshot_slot(snapshot,1,slot,&value)==XR_XIR_VALUE_OK);
    if (mode==2 || mode==3) {
        XdapValueView view={&value,fixture->domain,snapshot,1};XdapValueChildInfo child={0};XrXirValue element={0};
        CHECK(xdap_value_child(&view,mode==2 ? 1 : 0,&element,&child)==XR_XIR_VALUE_OK);
        CHECK(element.type==XR_XIR_I64 && element.payload==(mode==2 ? 20 : 11));xr_xir_value_drop(&element);
    } else CHECK(value.type==XR_XIR_I64 && value.payload==11);
    xr_xir_value_drop(&value);
}
static size_t globals_fault(size_t fail,unsigned mode) {
    GlobalFixture fixture=global_open(mode==3 ? XR_DAP_GLOBAL_TYPES_FIXTURE : XR_DAP_GLOBAL_ROOT_FIXTURE);
    XdapAdapter *adapter=fixture.adapter;global_pause(&fixture);uint32_t scope=globals_scope(adapter,1),reference=scope;
    XrJsonValue *reply=globals_variables(adapter,scope,mode ? 0 : 2,mode ? 0 : 1,true);
    if (mode==2) reference=(uint32_t)xjson_get_int(globals_find(reply,"values"),"variablesReference");
    xjson_free(reply);CHECK(reference && adapter->value_count);
    char message[512];
    if (!mode) snprintf(message,sizeof(message),"{\"seq\":30,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%u}}",scope);
    else snprintf(message,sizeof(message),"{\"seq\":30,\"type\":\"request\",\"command\":\"setVariable\",\"arguments\":{\"variablesReference\":%u,\"name\":\"%s\",\"value\":\"%s\"}}",
        reference,mode==3 ? "pair" : mode==2 ? "[1]" : "changing",mode==3 ? "incomingPair" : "shared");
    GlobalsSaved saved=globals_save(&fixture);size_t start=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : start+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,strlen(message));size_t sites=runtime_attempts-start;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) {
        CHECK(status==XR_XIR_OK && adapter->stop_epoch==saved.frame.epoch+(mode!=0));
        reply=last_response(mode ? "setVariable" : "variables",true);xjson_free(reply);capture.count=0;
    } else {
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"global request mode=%u fault=%zu sites=%zu status=%u\n",mode,fail,sites,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !capture.count);globals_unchanged(&fixture,&saved,!mode);
        if (!mode) {
            uint32_t spent=adapter->next_handle;reply=globals_variables(adapter,scope,0,0,true);xjson_free(reply);
            for (uint32_t i=saved.count;i<adapter->value_count;++i) CHECK(adapter->values[i].handle>=spent);
        }
    }
    globals_prior(&fixture,saved.snapshot,mode);
    const char *output=mode==3 ? (fail==SIZE_MAX ? "168\n" : "80\n") :
        fail==SIZE_MAX && mode ? (mode==2 ? "230\n" : "239\n") : "151\n";
    globals_resume(&fixture,output);CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    xr_xir_vm_debug_snapshot_drop(saved.snapshot);global_finish(&fixture);return sites;
}
static void globals_faults(bool all) {
    for (unsigned mode=0;mode<4;++mode) {
        size_t sites=globals_fault(SIZE_MAX,mode);CHECK(sites);
        if (all) for (size_t point=0;point<sites;++point) globals_fault(point,mode);
        else { globals_fault(0,mode);if (sites>1) globals_fault(sites-1,mode); }
        printf("Globals request mode=%u allocation sites=%zu injection=%s original Source, snapshot, refs, spent handles and physical0 PASS\n",
            mode,sites,all ? "ALL" : "FIRST_LAST_ONLY");
    }
}
