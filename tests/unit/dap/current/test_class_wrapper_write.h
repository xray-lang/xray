/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_class_wrapper_write.h - Immutable wrappers retain genuine identity origins
 */
#ifndef TEST_CLASS_WRAPPER_WRITE_H
#define TEST_CLASS_WRAPPER_WRITE_H
static size_t class_wrapper_case(uint32_t kind,size_t fail) {
    const char *names[]={"pair","optional","choice","mutablePair","mutableOptional","mutableChoice"};
    const XrXirValuePathKind kinds[]={XR_XIR_PATH_TUPLE_FIELD,XR_XIR_PATH_NULLABLE_VALUE,XR_XIR_PATH_ENUM_FIELD};
    CHECK(kind<12);uint32_t root_kind=kind%6,family=kind%3;
    WriteFixture fixture=class_open_at(kind<6 ? XR_DAP_CLASS_WRAPPER_FIXTURE : XR_DAP_CLASS_WRAPPER_CHILD_FIXTURE,18);XdapAdapter *adapter=fixture.adapter;
    XrXirValue before={0},box={0},wrapper={0};class_binding(adapter,"before",&before);class_binding(adapter,"box",&box);
    class_binding(adapter,names[root_kind],&wrapper);CHECK(before.type==XR_XIR_I64 && before.payload==11);
    XrXirValuePathStep step={kinds[family],(XrXirType)wrapper.type,0};XrXirValue observed={0};XrXirFaultDetail fault={0};
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(&wrapper),.domain=fixture.domain,.work=65536,.scratch_bytes=65536};
    uint64_t work=xr_xir_domain_budget_stats(fixture.domain).work;
    CHECK(xir_value_observe_child(&wrapper,&step,&admission,&observed,&fault)==XR_XIR_VALUE_OK);
    CHECK(observed.type==box.type && observed.payload==box.payload && xr_xir_domain_budget_stats(fixture.domain).work>work);
    xr_xir_value_drop(&observed);step.selector=-1;
    CHECK(xir_value_observe_child(&wrapper,&step,&admission,&observed,&fault)==XR_XIR_VALUE_BOUNDS);
    CHECK(!observed.type && !observed.payload && fault.code==430 && fault.index==-1);
    step.selector=0;admission.work=0;XrXirFaultDetail retained=fault;
    CHECK(xir_value_observe_child(&wrapper,&step,&admission,&observed,&fault)==XR_XIR_VALUE_LIMIT && !memcmp(&fault,&retained,sizeof(fault)));
    admission.work=65536;step.container=XR_XIR_I64;
    CHECK(xir_value_observe_child(&wrapper,&step,&admission,&observed,&fault)==XR_XIR_VALUE_BAD_ARGUMENT && !observed.type && !observed.payload);
    if (family) {
        XrXirValue absent={0};class_binding(adapter,family==1 ? "none" : "empty",&absent);
        step=(XrXirValuePathStep){kinds[family],(XrXirType)absent.type,0};admission.work=65536;
        CHECK(xir_value_observe_child(&absent,&step,&admission,&observed,&fault)==XR_XIR_VALUE_BOUNDS && !fault.length && !observed.type);
        xr_xir_value_drop(&absent);
    }
    uint32_t scope=adapter->frames[0].scope,root=origin_child(adapter,scope,names[root_kind]);
    const char *child=family==1 ? "value" : "[0]";uint32_t reference=origin_child(adapter,root,child);
    CHECK(origin_ref(adapter,reference)->origin.path.count==1 && origin_ref(adapter,reference)->origin.path.steps[0].kind==kinds[family]);
    child_set(adapter,root,child,"box",false);
    if (!family) child_set(adapter,root,"[1]","incoming",false);
    char message[512];snprintf(message,sizeof(message),"{\"seq\":6,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"expression\":\"%s\"}}",names[root_kind]);request(adapter,message);
    XrJsonValue *reply=last_response("evaluate",true);uint32_t pure=(uint32_t)xjson_get_int(xjson_get_object(reply,"body"),"variablesReference");xjson_free(reply);capture.count=0;
    CHECK(pure && pure!=root && !origin_ref(adapter,pure)->origin.epoch);
    uint32_t pure_box=origin_child(adapter,pure,child);CHECK(!origin_ref(adapter,pure_box)->origin.epoch);child_set(adapter,pure_box,"value","incoming",false);
    XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;XrXirVmDebugFrameInfo frame=write_frame(snapshot);
    size_t length=child_message(message,sizeof(message),reference,"value","incoming");uint32_t refs=adapter->value_count,next=adapter->next_handle;
    size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,length);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) {
        CHECK(status==XR_XIR_OK && adapter->stop_epoch==epoch+1);reply=last_response("setVariable",true);xjson_free(reply);capture.count=0;
        XrXirVmDebugFrameInfo same=write_frame(adapter->snapshot);CHECK(same.id==frame.id && same.instruction==frame.instruction && same.thread==frame.thread);
        origin_message(message,sizeof(message),reference);request(adapter,message);reply=last_response("variables",false);xjson_free(reply);capture.count=0;
    } else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && adapter->snapshot==snapshot && adapter->stop_epoch==epoch);
        CHECK(runtime_live==blocks && runtime_bytes==bytes && adapter->value_count==refs && adapter->next_handle==next);
    }
    CHECK((kind<6)==(frame.thread==1));
    snprintf(message,sizeof(message),"{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":%llu}}",(unsigned long long)frame.thread);request(adapter,message);capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);uint32_t outputs=0;
    for (uint32_t i=0;i<capture.count;++i) {
        reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) CHECK(!outputs++ && !strcmp(xjson_get_string(xjson_get_object(reply,"body"),"output"),fail==SIZE_MAX ? "123\n" : "35\n"));xjson_free(reply);
    }
    CHECK(outputs==1 && xdap_adapter_free(adapter)==XR_XIR_CALL_READY && before.payload==11);
    admission.work=65536;CHECK(xr_xir_class_get(&box,0,&admission,&observed)==XR_XIR_VALUE_OK && observed.payload==(fail==SIZE_MAX ? 99 : 11));xr_xir_value_drop(&observed);
    xr_xir_value_drop(&wrapper);xr_xir_value_drop(&box);xr_xir_value_drop(&before);write_finish(&fixture);return sites;
}
#endif // TEST_CLASS_WRAPPER_WRITE_H
