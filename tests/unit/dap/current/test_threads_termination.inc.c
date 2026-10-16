/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_threads_termination.inc.c - Refused detach and transactional whole-target termination
 */
static void threads_termination_continue(XdapAdapter *adapter) {
    if (!adapter->snapshot) return;
    char message[256];snprintf(message,sizeof(message),
        "{\"seq\":12,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":%llu}}",
        (unsigned long long)xr_xir_vm_debug_snapshot_stopped_thread(adapter->snapshot));
    request(adapter,message);XrJsonValue *reply=last_response("continue",true);xjson_free(reply);
}
static void threads_termination_pause(XdapAdapter *adapter,bool paused) {
    if (!paused) return;
    request(adapter,"{\"seq\":6,\"type\":\"request\",\"command\":\"pause\",\"arguments\":{\"threadId\":1}}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
}
static void threads_termination_refusals(bool paused) {
    ThreadsFixture fixture;uint64_t ids[64];threads_pause_running(&fixture,ids);XdapAdapter *adapter=fixture.adapter;
    threads_termination_pause(adapter,paused);
    const char *values[]={"false","null","1","\"true\""};
    for (uint32_t i=0;i<sizeof(values)/sizeof(*values);++i) {
        capture.count=0;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;
        char message[256];snprintf(message,sizeof(message),
            "{\"seq\":7,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{\"terminateDebuggee\":%s}}",values[i]);
        request(adapter,message);XrJsonValue *reply=last_response("disconnect",false);xjson_free(reply);
        CHECK(!adapter->closed && !adapter->terminal && adapter->snapshot==snapshot && adapter->stop_epoch==epoch);
        CHECK(adapter->running!=paused && !event_count("output") && !event_count("terminated"));
        threads_protocol(&fixture,ids,3);
        if (!paused) CHECK(xdap_adapter_step(adapter,16)==XR_XIR_OK && adapter->running && !adapter->snapshot);
    }
    threads_termination_continue(adapter);capture.count=0;
    for (uint32_t step=0;step<10000 && !adapter->terminal;++step) CHECK(xdap_adapter_step(adapter,1)==XR_XIR_OK);
    CHECK(adapter->terminal && adapter->outcome.status==XR_XIR_CALL_RETURNED && event_count("output")==1);
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *json=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(json);
        const char *event=xjson_get_string(json,"event");
        if (event && !strcmp(event,"output"))
            CHECK(!strcmp(xjson_get_string(xjson_get_object(json,"body"),"output"),"33\n"));
        xjson_free(json);
    }
    threads_close(&fixture);
}
static size_t threads_termination_fault(uint32_t mode,bool paused,size_t fail) {
    const char *requests[]={
        "{\"seq\":8,\"type\":\"request\",\"command\":\"terminate\"}",
        "{\"seq\":8,\"type\":\"request\",\"command\":\"disconnect\"}",
        "{\"seq\":8,\"type\":\"request\",\"command\":\"disconnect\",\"arguments\":{\"terminateDebuggee\":true}}"
    };
    CHECK(mode<3);ThreadsFixture fixture;uint64_t ids[64];threads_pause_running(&fixture,ids);
    XdapAdapter *adapter=fixture.adapter;threads_termination_pause(adapter,paused);capture.count=0;
    XdapLaunch *launch=adapter->launch;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;
    uint64_t epoch=adapter->stop_epoch;uint32_t sequence=adapter->seq;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    adapter->config.write=threads_pause_capture;size_t begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,requests[mode],strlen(requests[mode]));
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail!=SIZE_MAX) {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !capture.count && adapter->seq==sequence);
        CHECK(!adapter->closed && !adapter->terminal && adapter->launch==launch && adapter->snapshot==snapshot);
        CHECK(adapter->stop_epoch==epoch && adapter->running!=paused);
        CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes && after.allocated_bytes>=before.allocated_bytes && after.work>before.work);
        threads_protocol(&fixture,ids,3);threads_termination_continue(adapter);
        CHECK(xdap_adapter_step(adapter,16)==XR_XIR_OK && adapter->running && !adapter->snapshot && !adapter->terminal);
        capture.count=0;request(adapter,requests[mode]);
    } else CHECK(status==XR_XIR_OK && capture.count==1 && adapter->seq==sequence+1);
    XrJsonValue *reply=last_response(mode ? "disconnect" : "terminate",true);xjson_free(reply);
    CHECK(adapter->closed==(mode!=0) && !adapter->snapshot && !adapter->stop_epoch);
    uint32_t count=77;CHECK(xdap_current_threads(adapter->launch->control,ids,64,&count)==XR_XIR_VALUE_OK && !count);
    XdapCurrentResult result={0};CHECK(xdap_current_poll(adapter->launch->control,1,&result)==XR_XIR_VALUE_OK);
    CHECK(result.state==XDAP_CURRENT_ENDED && result.outcome.status==XR_XIR_CALL_CANCELLED);
    xdap_current_result_drop(&result);
    CHECK(!event_count("output"));threads_close(&fixture);return sites;
}
static void threads_termination(bool resources) {
    threads_termination_refusals(false);threads_termination_refusals(true);
    for (uint32_t mode=0;mode<3;++mode) for (uint32_t paused=0;paused<2;++paused) {
        size_t sites=threads_termination_fault(mode,paused!=0,SIZE_MAX);CHECK(sites);
        if (resources) for (size_t point=0;point<sites;++point) threads_termination_fault(mode,paused!=0,point);
        else { threads_termination_fault(mode,paused!=0,0);if (sites>1) threads_termination_fault(mode,paused!=0,sites-1); }
        printf("Whole-target termination mode=%u paused=%u sites=%zu injection=%s; response OOM preserves real root/children PASS\n",
            mode,paused,sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
    puts("Explicit false/invalid disconnect preserves running/stopped Source and final33; true/default stop drains all children; physical0/0 PASS");
}
