/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_globals_helpers.inc.c - Actual DAP module scopes and owned responses
 */
static uint32_t globals_scope(XdapAdapter *adapter,uint32_t frame) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":20,\"type\":\"request\",\"command\":\"scopes\",\"arguments\":{\"frameId\":%u}}",adapter->frames[frame].handle);
    request(adapter,message);XrJsonValue *reply=last_response("scopes",true),*scopes=xjson_get_array(xjson_get_object(reply,"body"),"scopes");uint32_t result=0;
    for (int i=0;i<xjson_array_len(scopes);++i) {
        XrJsonValue *scope=xjson_array_get(scopes,i);
        if (!strcmp(xjson_get_string(scope,"name"),"Globals")) { CHECK(!result);result=(uint32_t)xjson_get_int(scope,"variablesReference"); }
    }
    CHECK(result==adapter->frames[frame].global_scope && result);xjson_free(reply);capture.count=0;return result;
}
static XrJsonValue *globals_variables(XdapAdapter *adapter,uint32_t handle,uint32_t start,uint32_t count,bool success) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":21,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%u,\"start\":%u,\"count\":%u}}",handle,start,count);
    request(adapter,message);XrJsonValue *reply=last_response("variables",success);capture.count=0;return reply;
}
static XrJsonValue *globals_find(XrJsonValue *reply,const char *name) {
    XrJsonValue *variables=xjson_get_array(xjson_get_object(reply,"body"),"variables"),*found=NULL;
    for (int i=0;i<xjson_array_len(variables);++i) {
        XrJsonValue *variable=xjson_array_get(variables,i);
        if (!strcmp(xjson_get_string(variable,"name"),name)) { CHECK(!found);found=variable; }
    }
    return found;
}
static void globals_expect(XrJsonValue *reply,const char *name,const char *expected) {
    XrJsonValue *value=globals_find(reply,name);CHECK(value && !strcmp(xjson_get_string(value,"value"),expected));
}
static XrJsonValue *globals_set(XdapAdapter *adapter,uint32_t reference,const char *name,const char *value,bool success) {
    char message[512];snprintf(message,sizeof(message),"{\"seq\":22,\"type\":\"request\",\"command\":\"setVariable\",\"arguments\":{\"variablesReference\":%u,\"name\":\"%s\",\"value\":\"%s\"}}",reference,name,value);
    request(adapter,message);XrJsonValue *reply=last_response("setVariable",success);capture.count=0;return reply;
}
static XrJsonValue *globals_evaluate(XdapAdapter *adapter,uint32_t frame,const char *expression,bool success) {
    char message[512];snprintf(message,sizeof(message),"{\"seq\":23,\"type\":\"request\",\"command\":\"evaluate\",\"arguments\":{\"frameId\":%u,\"expression\":\"%s\"}}",adapter->frames[frame].handle,expression);
    request(adapter,message);XrJsonValue *reply=last_response("evaluate",success);capture.count=0;return reply;
}
static void globals_resume(GlobalFixture *fixture,const char *expected) {
    XdapAdapter *adapter=fixture->adapter;uint64_t thread=xr_xir_vm_debug_snapshot_stopped_thread(adapter->snapshot);
    char message[256];snprintf(message,sizeof(message),"{\"seq\":24,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":%llu}}",(unsigned long long)thread);
    request(adapter,message);XrJsonValue *reply=last_response("continue",true);xjson_free(reply);
    for (uint32_t i=0;i<32 && !adapter->terminal;++i) CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK);
    CHECK(adapter->terminal && !adapter->snapshot);bool output=false;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *entry=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(entry,"event");
        if (event && !strcmp(event,"output")) { CHECK(!output && !strcmp(xjson_get_string(xjson_get_object(entry,"body"),"output"),expected));output=true; }
        xjson_free(entry);
    }
    CHECK(output);capture.count=0;
}
