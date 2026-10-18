/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_threads_pause.inc.c - Transactional all-stop pause from a real child
 */
static XrXirStatus threads_pause_capture(void *context,const char *bytes,size_t length) {
    ThreadsFixture *fixture=context;Capture *out=&capture;
    CHECK(fixture->adapter && out->count<128 && length<sizeof(out->messages[0].bytes));
    Message *message=&out->messages[out->count++];memcpy(message->bytes,bytes,length);
    message->bytes[length]=0;message->size=length;++out->seq;
    CHECK(xdap_adapter_free(fixture->adapter)==XR_XIR_CALL_BUSY);
    CHECK(xdap_adapter_request(fixture->adapter,"{}",2)==XR_XIR_BAD_STRUCTURE);return XR_XIR_OK;
}
static void threads_pause_running(ThreadsFixture *fixture,uint64_t *ids) {
    threads_open(fixture);XdapAdapter *adapter=fixture->adapter;
    XrJsonValue *initialized=last_response("initialize",true),*body=xjson_get_object(initialized,"body");
    CHECK(xjson_get_bool(body,"supportsPauseRequest") && xjson_get_bool(body,"supportsEvaluateForHovers"));xjson_free(initialized);
    request(adapter,"{\"seq\":3,\"type\":\"request\",\"command\":\"configurationDone\"}");
    uint32_t count=0;
    for (uint32_t step=0;step<10000 && !adapter->terminal;++step) {
        CHECK(xdap_adapter_step(adapter,1)==XR_XIR_OK);
        XrXirValueStatus status=xdap_current_threads(adapter->launch->control,ids,64,&count);
        if (status==XR_XIR_VALUE_UNSUPPORTED) continue;
        CHECK(status==XR_XIR_VALUE_OK);if (count<3) continue;
        CHECK(count==3 && ids[0]==1 && ids[1]>1 && ids[2]>ids[1]);
        CHECK(adapter->running && !adapter->snapshot && !adapter->stop_epoch);capture.count=0;return;
    }
    CHECK(false);
}
static size_t threads_pause_fault(size_t fail) {
    ThreadsFixture fixture;uint64_t ids[64];threads_pause_running(&fixture,ids);XdapAdapter *adapter=fixture.adapter;
    request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"pause\",\"arguments\":{\"threadId\":2147483647}}");
    XrJsonValue *reply=last_response("pause",false);xjson_free(reply);
    CHECK(xdap_adapter_step(adapter,16)==XR_XIR_OK && adapter->running && !adapter->snapshot);
    capture.count=0;uint32_t sequence=adapter->seq;const char *reason=adapter->stop_reason;
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(fixture.ledger,&before)==XR_COMPILE_RESOURCE_OK);
    char message[256];snprintf(message,sizeof(message),
        "{\"seq\":9,\"type\":\"request\",\"command\":\"pause\",\"arguments\":{\"threadId\":%llu}}",(unsigned long long)ids[1]);
    adapter->config.write=threads_pause_capture;size_t begin=runtime_attempts;
    runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,strlen(message));
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes && after.work>before.work && after.allocated_bytes>=before.allocated_bytes);
    CHECK(adapter->running && !adapter->snapshot && !adapter->stop_epoch && !adapter->value_count);
    if (fail==SIZE_MAX) CHECK(status==XR_XIR_OK && capture.count==1 && adapter->seq==sequence+1);
    else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !capture.count);
        CHECK(adapter->seq==sequence && adapter->stop_reason==reason);
        CHECK(xdap_adapter_step(adapter,16)==XR_XIR_OK && adapter->running && !adapter->snapshot);
        request(adapter,message);
    }
    reply=last_response("pause",true);CHECK((uint32_t)xjson_get_int(reply,"seq")==capture.seq);xjson_free(reply);
    adapter->config.write=threads_capture;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);
    bool stopped=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *json=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(json);
        const char *event=xjson_get_string(json,"event");
        if (event && !strcmp(event,"stopped")) {
            XrJsonValue *body=xjson_get_object(json,"body");
            CHECK(!strcmp(xjson_get_string(body,"reason"),"pause") && xjson_get_bool(body,"allThreadsStopped"));stopped=true;
        }
        xjson_free(json);
    }
    CHECK(stopped);uint64_t epoch=adapter->stop_epoch;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;
    request(adapter,message);reply=last_response("pause",false);xjson_free(reply);
    CHECK(adapter->snapshot==snapshot && adapter->stop_epoch==epoch && !adapter->running);
    uint64_t thread=xr_xir_vm_debug_snapshot_stopped_thread(snapshot);
    snprintf(message,sizeof(message),"{\"seq\":10,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":%llu}}",
        (unsigned long long)thread);request(adapter,message);reply=last_response("continue",true);xjson_free(reply);
    for (uint32_t step=0;step<10000 && !adapter->terminal;++step) CHECK(xdap_adapter_step(adapter,1)==XR_XIR_OK);
    CHECK(adapter->terminal && adapter->outcome.status==XR_XIR_CALL_RETURNED && event_count("output")==1 && event_count("terminated")==1);
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *json=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);CHECK(json);
        const char *event=xjson_get_string(json,"event");
        if (event && !strcmp(event,"output")) CHECK(!strcmp(xjson_get_string(xjson_get_object(json,"body"),"output"),"33\n"));
        xjson_free(json);
    }
    snprintf(message,sizeof(message),"{\"seq\":11,\"type\":\"request\",\"command\":\"pause\",\"arguments\":{\"threadId\":%llu}}",
        (unsigned long long)ids[1]);request(adapter,message);reply=last_response("pause",false);xjson_free(reply);
    threads_close(&fixture);return sites;
}
