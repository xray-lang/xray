/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_globals.c - Source-backed Globals over the real DAP request path
 */
#define main reference_globals_dispatch_main
#include "test_dispatch.c"
#undef main
#include "xir/xxir_vm_debug_internal.h"
#include "app/dap/xdap_dispatch_internal.h"
#include "test_global_core_helpers.inc.c"
#include "test_globals_helpers.inc.c"
#include "test_globals_faults.inc.c"
static void globals_root(void) {
    GlobalFixture fixture=global_open(XR_DAP_GLOBAL_ROOT_FIXTURE);XdapAdapter *adapter=fixture.adapter;global_pause(&fixture);
    uint32_t library=globals_scope(adapter,0),scope=globals_scope(adapter,1);CHECK(library!=scope);
    XrJsonValue *reply=globals_variables(adapter,library,0,0,true);globals_expect(reply,"shared","19");
    CHECK(!globals_find(reply,"changing") && !globals_find(reply,"before"));xjson_free(reply);
    reply=globals_variables(adapter,scope,0,0,true);globals_expect(reply,"shared","73");globals_expect(reply,"changing","11");
    globals_expect(reply,"nothing","()");CHECK(!globals_find(reply,"late") && !globals_find(reply,"before"));
    uint32_t values=(uint32_t)xjson_get_int(globals_find(reply,"values"),"variablesReference");
    uint32_t saved=(uint32_t)xjson_get_int(globals_find(reply,"snapshot"),"variablesReference");CHECK(values && saved && values!=saved);
    XrJsonValue *title=xjson_get(globals_find(reply,"title"),"value");CHECK(title && title->string_len==12 && !memcmp(title->as.string,"global\0value",12));xjson_free(reply);
    reply=globals_variables(adapter,scope,1,1,true);CHECK(xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables"))==1);globals_expect(reply,"changing","11");xjson_free(reply);
    reply=globals_evaluate(adapter,1,"shared",true);CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"result"),"99"));xjson_free(reply);
    reply=globals_evaluate(adapter,1,"changing+shared",true);CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"result"),"110"));xjson_free(reply);
    reply=globals_evaluate(adapter,0,"changing",false);xjson_free(reply);
    reply=globals_evaluate(adapter,1,"values",true);uint32_t pure=(uint32_t)xjson_get_int(xjson_get_object(reply,"body"),"variablesReference");CHECK(pure && pure!=values);xjson_free(reply);
    uint64_t epoch=adapter->stop_epoch;reply=globals_set(adapter,pure,"[1]","77",false);xjson_free(reply);
    reply=globals_set(adapter,saved,"[1]","77",false);xjson_free(reply);reply=globals_set(adapter,scope,"shared","99",false);xjson_free(reply);
    reply=globals_set(adapter,scope,"changing","true",false);xjson_free(reply);CHECK(adapter->stop_epoch==epoch);
    uint32_t slot=global_slot(adapter->launch,adapter->snapshot,1,"changing");XrXirVmDebugSnapshot *old=NULL;
    CHECK(xdap_current_snapshot(adapter->launch->control,&old)==XR_XIR_VALUE_OK);
    reply=globals_set(adapter,scope,"changing","changing+88",true);CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"value"),"99"));xjson_free(reply);CHECK(adapter->stop_epoch==++epoch);
    reply=globals_variables(adapter,values,0,0,false);xjson_free(reply);reply=globals_variables(adapter,scope,0,0,true);
    globals_expect(reply,"changing","99");values=(uint32_t)xjson_get_int(globals_find(reply,"values"),"variablesReference");saved=(uint32_t)xjson_get_int(globals_find(reply,"snapshot"),"variablesReference");xjson_free(reply);
    reply=globals_set(adapter,values,"[1]","77",true);xjson_free(reply);CHECK(adapter->stop_epoch==++epoch);
    reply=globals_set(adapter,scope,"nothing","nothing",true);CHECK(!strcmp(xjson_get_string(xjson_get_object(reply,"body"),"value"),"()"));
    xjson_free(reply);CHECK(adapter->stop_epoch==++epoch);global_number(old,1,slot,11);
    globals_resume(&fixture,"296\n");reply=globals_variables(adapter,scope,0,0,false);xjson_free(reply);
    request(adapter,"{\"seq\":25,\"type\":\"request\",\"command\":\"restart\"}");reply=last_response("restart",true);xjson_free(reply);capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot);capture.count=0;uint32_t restarted=globals_scope(adapter,1);CHECK(restarted!=scope);
    reply=globals_variables(adapter,restarted,0,0,true);globals_expect(reply,"changing","11");xjson_free(reply);
    reply=globals_variables(adapter,scope,0,0,false);xjson_free(reply);globals_resume(&fixture,"151\n");
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);global_number(old,1,slot,11);xr_xir_vm_debug_snapshot_drop(old);global_finish(&fixture);
}
static void globals_child(void) {
    GlobalFixture fixture=global_open(XR_DAP_GLOBAL_CHILD_FIXTURE);XdapAdapter *adapter=fixture.adapter;global_pause(&fixture);
    uint32_t scope=globals_scope(adapter,0),caller=globals_scope(adapter,1);XrJsonValue *reply=globals_variables(adapter,scope,0,0,true);
    globals_expect(reply,"shared","19");CHECK(!globals_find(reply,"changing"));xjson_free(reply);
    reply=globals_variables(adapter,caller,0,0,true);CHECK(!xjson_array_len(xjson_get_array(xjson_get_object(reply,"body"),"variables")));xjson_free(reply);
    uint64_t epoch=adapter->stop_epoch;reply=globals_set(adapter,caller,"changing","99",false);xjson_free(reply);CHECK(adapter->stop_epoch==epoch);
    globals_resume(&fixture,"25\n");CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);global_finish(&fixture);
}
static void globals_borrow(void) {
    GlobalFixture fixture=global_open(XR_DAP_GLOBAL_BORROW_FIXTURE);XdapAdapter *adapter=fixture.adapter;global_pause_at(&fixture,8);
    uint32_t scope=globals_scope(adapter,1);XrJsonValue *reply=globals_variables(adapter,scope,0,0,true);
    CHECK(!globals_find(reply,"changing"));xjson_free(reply);uint64_t epoch=adapter->stop_epoch;
    reply=globals_set(adapter,scope,"changing","99",false);xjson_free(reply);CHECK(adapter->stop_epoch==epoch);
    reply=globals_set(adapter,adapter->frames[0].scope,"value","99",true);xjson_free(reply);CHECK(adapter->stop_epoch==epoch+1);
    reply=globals_variables(adapter,scope,0,0,true);CHECK(!globals_find(reply,"changing"));xjson_free(reply);
    global_return(&fixture,adapter->stop_epoch,"99\n");CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);global_finish(&fixture);
}
static void globals_types(void) {
    GlobalFixture fixture=global_open(XR_DAP_GLOBAL_TYPES_FIXTURE);XdapAdapter *adapter=fixture.adapter;global_pause(&fixture);
    uint32_t scope=globals_scope(adapter,1);uint64_t epoch=adapter->stop_epoch;
    const char *names[]={"pair","optional","choice","callback"};
    const char *values[]={"incomingPair","incomingOptional","incomingChoice","incomingCallback"};
    for (unsigned i=0;i<4;++i) {
        XrJsonValue *reply=globals_set(adapter,scope,names[i],values[i],true);xjson_free(reply);CHECK(adapter->stop_epoch==++epoch);
    }
    XrJsonValue *reply=globals_variables(adapter,scope,0,0,true);uint32_t box=(uint32_t)xjson_get_int(globals_find(reply,"box"),"variablesReference");
    uint32_t pair=(uint32_t)xjson_get_int(globals_find(reply,"pair"),"variablesReference");CHECK(box && pair);xjson_free(reply);
    reply=globals_variables(adapter,pair,0,0,true);globals_expect(reply,"[0]","99");XrJsonValue *text=xjson_get(globals_find(reply,"[1]"),"value");
    CHECK(text && text->string_len==9 && !memcmp(text->as.string,"new\0value",9));xjson_free(reply);
    reply=globals_set(adapter,box,"value","99",true);xjson_free(reply);CHECK(adapter->stop_epoch==++epoch);
    globals_resume(&fixture,"520\n");CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);global_finish(&fixture);
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();CHECK(argc==2);
    if (!strcmp(argv[1],"--root")) globals_root();else if (!strcmp(argv[1],"--child")) globals_child();
    else if (!strcmp(argv[1],"--types")) globals_types();
    else if (!strcmp(argv[1],"--faults") || !strcmp(argv[1],"--resources")) globals_faults(!strcmp(argv[1],"--resources"));
    else { CHECK(!strcmp(argv[1],"--borrow"));globals_borrow(); }
    puts("DAP Globals: actual modules, paging, shadowed Source evaluation, writes, CoW, worker/loan denial and restart PASS");return 0;
}
