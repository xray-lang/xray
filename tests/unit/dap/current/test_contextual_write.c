/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_contextual_write.c - Real paused writes use their owned target types
 */
#define XR_DAP_SET_VARIABLE_ENTRY reference_contextual_set_main
#include "test_set_variable.c"
#undef XR_DAP_SET_VARIABLE_ENTRY
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
static XrJsonValue *contextual_set(XdapAdapter *adapter,uint32_t scope,const char *name,const char *text,bool success) {
    char message[1024];set_message(message,sizeof(message),scope,name,text);request(adapter,message);
    for (uint32_t i=capture.count;i;--i) {
        XrJsonValue *json=xjson_parse(capture.messages[i-1].bytes,capture.messages[i-1].size);CHECK(json);
        bool response=!strcmp(xjson_get_string(json,"type"),"response") && !strcmp(xjson_get_string(json,"command"),"setVariable");
        if (response && xjson_get_bool(json,"success")!=success)
            fprintf(stderr,"contextual %s = %s: %s\n",name,text,capture.messages[i-1].bytes);
        xjson_free(json);if (response) break;
    }
    CHECK(!event_count("stopped") && !event_count("output"));return last_response("setVariable",success);
}
static void contextual_scalar(XdapAdapter *adapter,const char *name,XrXirType type,int64_t payload) {
    XrXirValue value=set_owned(adapter,adapter->snapshot,name);CHECK(value.type==(uint32_t)type && value.payload==payload);xr_xir_value_drop(&value);
}
static void contextual_nullable(XdapAdapter *adapter,bool some,int64_t expected) {
    XrXirValue value=set_owned(adapter,adapter->snapshot,"optional");const XrXirValue *payload=NULL;bool present=false;
    CHECK(xr_xir_nullable_view(&value,&present,&payload) && present==some);
    if (some) CHECK(payload && payload->type==XR_XIR_I32 && payload->payload==expected);
    xr_xir_value_drop(&value);
}
static void contextual_array(XdapAdapter *adapter,uint32_t count,int64_t last) {
    XrXirValue value=set_owned(adapter,adapter->snapshot,"items");XdapValueView view={&value,adapter->config.observer_metadata,adapter->snapshot,0};
    XdapValueShape shape={0};CHECK(xdap_value_shape(&view,&shape)==XR_XIR_VALUE_OK && shape.children==count);
    if (count) { XrXirValue child={0};XdapValueChildInfo info={0};
        CHECK(xdap_value_child(&view,count-1,&child,&info)==XR_XIR_VALUE_OK && child.type==XR_XIR_I16 && child.payload==last);xr_xir_value_drop(&child); }
    xr_xir_value_drop(&value);
}
static uint32_t contextual_reference(XdapAdapter *adapter,const char *name) {
    char message[256];snprintf(message,sizeof(message),"{\"seq\":8,\"type\":\"request\",\"command\":\"variables\",\"arguments\":{\"variablesReference\":%u}}",adapter->frames[0].scope);
    request(adapter,message);XrJsonValue *reply=last_response("variables",true),*list=xjson_get_array(xjson_get_object(reply,"body"),"variables");
    uint32_t reference=0;
    for (int i=0;i<xjson_array_len(list);++i) { XrJsonValue *entry=xjson_array_get(list,i);
        if (!strcmp(xjson_get_string(entry,"name"),name)) reference=(uint32_t)xjson_get_int(entry,"variablesReference"); }
    xjson_free(reply);capture.count=0;CHECK(reference);return reference;
}
static void contextual_nested(XdapAdapter *adapter,bool outer,bool inner) {
    XrXirValue value=set_owned(adapter,adapter->snapshot,"nested");const XrXirValue *payload=NULL,*leaf=NULL;bool some=false,present=false;
    CHECK(xr_xir_nullable_view(&value,&some,&payload) && some==outer);
    if (outer) { CHECK(payload && xr_xir_nullable_view(payload,&present,&leaf) && present==inner);
        if (inner) CHECK(leaf && leaf->type==XR_XIR_I32 && leaf->payload==7); }
    xr_xir_value_drop(&value);
}
static void contextual_normal(void) {
    SetFixture fixture=set_open(XR_DAP_CONTEXT_WRITE_FIXTURE);XdapAdapter *adapter=fixture.adapter;
    set_pause(&fixture,XR_DAP_CONTEXT_WRITE_FIXTURE,9);uint32_t scope=adapter->frames[0].scope;
    XrXirVmDebugSnapshot *old=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&old)==XR_XIR_VALUE_OK);
    uint64_t epoch=adapter->stop_epoch;XrJsonValue *reply=contextual_set(adapter,scope,"small","127",true);xjson_free(reply);capture.count=0;
    contextual_scalar(adapter,"small",XR_XIR_I8,127);CHECK(adapter->stop_epoch==epoch+1);
    XrXirValue prior=set_owned(adapter,old,"small");CHECK(prior.type==XR_XIR_I8 && prior.payload==1);xr_xir_value_drop(&prior);
    const char *names[]={"small","count","decimal","optional","optional","items","items","pair"};
    const char *texts[]={"-128","255","1.25","null","7","[]","[6,7]","(-8,60000)"};
    for (uint32_t i=0;i<8;++i) {
        reply=contextual_set(adapter,scope,names[i],texts[i],true);xjson_free(reply);capture.count=0;
        XrXirValue before=set_owned(adapter,old,names[i]),after=set_owned(adapter,adapter->snapshot,names[i]);
        CHECK(before.type==after.type && xr_xir_value_arena(&before)==xr_xir_value_arena(&after));xr_xir_value_drop(&before);xr_xir_value_drop(&after);
        if (i==0) contextual_scalar(adapter,"small",XR_XIR_I8,-128);
        if (i==1) contextual_scalar(adapter,"count",XR_XIR_U8,255);
        if (i==2) contextual_scalar(adapter,"decimal",XR_XIR_F32,INT64_C(0x3fa00000));
        if (i==3 || i==4) contextual_nullable(adapter,i==4,7);
        if (i==5 || i==6) contextual_array(adapter,i==6 ? 2 : 0,7);
        if (i==7) { XrXirValue pair=set_owned(adapter,adapter->snapshot,"pair"),child={0};
            CHECK(xr_xir_tuple_get(&pair,0,&child)==XR_XIR_VALUE_OK && child.type==XR_XIR_I8 && child.payload==-8);xr_xir_value_drop(&child);
            CHECK(xr_xir_tuple_get(&pair,1,&child)==XR_XIR_VALUE_OK && child.type==XR_XIR_U16 && child.payload==60000);xr_xir_value_drop(&child);xr_xir_value_drop(&pair); }
    }
    const char *nested_names[]={"nested","nested","optional","nested","nested","optional"};
    const char *nested_texts[]={"null","optional","null","optional","null","7"};
    for (uint32_t i=0;i<6;++i) {
        reply=contextual_set(adapter,scope,nested_names[i],nested_texts[i],true);xjson_free(reply);capture.count=0;
        if (i==0 || i==4) contextual_nested(adapter,false,false);
        if (i==1) contextual_nested(adapter,true,true);
        if (i==2 || i==3) contextual_nested(adapter,true,i==2);
    }
    uint32_t array=contextual_reference(adapter,"items");reply=contextual_set(adapter,array,"[1]","300",true);xjson_free(reply);capture.count=0;contextual_array(adapter,2,300);
    uint32_t optional=contextual_reference(adapter,"optional"),pair_ref=contextual_reference(adapter,"pair");
    epoch=adapter->stop_epoch;XrXirVmDebugSnapshot *wrapper_stop=adapter->snapshot;
    reply=contextual_set(adapter,optional,"value","9",false);xjson_free(reply);capture.count=0;contextual_nullable(adapter,true,7);
    reply=contextual_set(adapter,pair_ref,"[0]","9",false);xjson_free(reply);capture.count=0;
    CHECK(adapter->stop_epoch==epoch && adapter->snapshot==wrapper_stop);
    reply=contextual_set(adapter,adapter->frames[0].global_scope,"shared","65535",true);xjson_free(reply);capture.count=0;
    const char *bad_names[]={"small","count","decimal","optional","items","pair"};
    const char *bad_texts[]={"128","-1","true","()","[true]","(128,4)"};
    epoch=adapter->stop_epoch;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;
    for (uint32_t i=0;i<6;++i) { reply=contextual_set(adapter,scope,bad_names[i],bad_texts[i],false);xjson_free(reply);capture.count=0;CHECK(adapter->stop_epoch==epoch && adapter->snapshot==snapshot); }
    reply=contextual_set(adapter,adapter->frames[0].global_scope,"shared","65536",false);xjson_free(reply);capture.count=0;
    CHECK(adapter->stop_epoch==epoch && adapter->snapshot==snapshot);set_resume(adapter);
    set_output(0,"-128\n",5);set_output(1,"255\n",4);set_output(2,"1.25\n",5);set_output(3,"false\n",6);set_output(4,"2\n",2);set_output(5,"-8\n",3);set_output(6,"65535\n",6);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);prior=(XrXirValue){0};
    CHECK(xr_xir_vm_debug_snapshot_depth(old)>=2);xr_xir_vm_debug_snapshot_drop(old);set_finish(&fixture);
    puts("Contextual i8/u8/f32/nested Nullable/Array/tuple/global/children and exact output; readonly wrapper payloads and seven bad RHS preserve stopped state; escaped prior snapshot physical0 PASS");
}
static size_t contextual_fault(size_t fail,uint32_t kind) {
    SetFixture fixture=set_open(XR_DAP_CONTEXT_WRITE_FIXTURE);XdapAdapter *adapter=fixture.adapter;set_pause(&fixture,XR_DAP_CONTEXT_WRITE_FIXTURE,9);
    const char *names[]={"small","optional","items"},*texts[]={"127","null","[]"};CHECK(kind<3);
    char message[512];size_t length=set_message(message,sizeof(message),adapter->frames[0].scope,names[kind],texts[kind]);
    uint64_t epoch=adapter->stop_epoch;XrXirVmDebugSnapshot *snapshot=adapter->snapshot;
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,length);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) { CHECK(status==XR_XIR_OK && adapter->stop_epoch==epoch+1);XrJsonValue *reply=last_response("setVariable",true);xjson_free(reply); }
    else { if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"contextual fault kind=%u point=%zu status=%u sites=%zu\n",kind,fail,status,sites);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && !capture.count && adapter->stop_epoch==epoch && adapter->snapshot==snapshot);
        CHECK(runtime_live==live && runtime_bytes==bytes); }
    contextual_scalar(adapter,"small",XR_XIR_I8,fail==SIZE_MAX && kind==0 ? 127 : 1);
    contextual_nullable(adapter,!(fail==SIZE_MAX && kind==1),4);contextual_array(adapter,fail==SIZE_MAX && kind==2 ? 0 : 2,2);
    set_resume(adapter);set_output(0,fail==SIZE_MAX && kind==0 ? "127\n" : "1\n",fail==SIZE_MAX && kind==0 ? 4 : 2);
    set_output(3,fail==SIZE_MAX && kind==1 ? "true\n" : "false\n",fail==SIZE_MAX && kind==1 ? 5 : 6);
    set_output(4,fail==SIZE_MAX && kind==2 ? "0\n" : "2\n",2);set_dispose(&fixture);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();uint32_t selected=3;
    if (argc==2) for (uint32_t k=0;k<3;++k) if (!strcmp(argv[1],(const char *const[]){"--scalar-resources","--nullable-resources","--array-resources"}[k])) selected=k;
    CHECK(argc==1 || (argc==2 && selected<3));if (argc==1) contextual_normal();
    for (uint32_t k=0;k<3;++k) { if (selected<3 && selected!=k) continue;
        size_t sites=contextual_fault(SIZE_MAX,k);CHECK(sites);
        if (selected<3) for (size_t p=0;p<sites;++p) contextual_fault(p,k);
        else { contextual_fault(0,k);if (sites>1) contextual_fault(sites-1,k); }
        printf("Contextual RHS kind=%u sites=%zu injection=%s; target preserved on failure, exact old/new output, original owners physical0 PASS\n",k,sites,selected<3 ? "ALL" : "FIRST_LAST_ONLY");
    }
    return 0;
}
