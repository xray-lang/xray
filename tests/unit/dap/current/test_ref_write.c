/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_ref_write.c - DAP writes respect real projected parameter loans
 */
#define XR_DAP_CHILD_ENTRY reference_ref_child_main
#include "test_child_write.c"
#undef XR_DAP_CHILD_ENTRY
#include "xir/xxir_cell_owner_internal.h"
#include "xir/xxir_array.h"
static WriteFixture ref_open(bool object) {
    const char *file=object ? XR_DAP_REF_CLASS_FIXTURE : XR_DAP_REF_VALUES_FIXTURE;
    WriteFixture fixture=write_open(file);char message[2048];
    snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%u}]}}",file,object ? 10 : 6);
    request(fixture.adapter,message);request(fixture.adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(fixture.adapter,1000)==XR_XIR_OK && fixture.adapter->snapshot && !fixture.adapter->running);capture.count=0;return fixture;
}
static void ref_numbers(XrXirValue *array,XrXirDomain *domain,int64_t first,int64_t second) {
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(array),.domain=domain,.work=65536,.scratch_bytes=65536};
    XrXirFaultDetail fault={0};XrXirValue value={0};
    CHECK(xr_xir_array_get(array,0,&admission,&value,&fault)==XR_XIR_VALUE_OK && value.type==XR_XIR_I64 && value.payload==first);xr_xir_value_drop(&value);
    CHECK(xr_xir_array_get(array,1,&admission,&value,&fault)==XR_XIR_VALUE_OK && value.type==XR_XIR_I64 && value.payload==second);xr_xir_value_drop(&value);
}
static uint32_t ref_operand(XdapAdapter *adapter,XrXirValue *cell,bool projection) {
    XdapLexicalView lexical={0};CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,0,&lexical)==XR_XIR_OK);
    for (uint32_t b=lexical.environment;b;b=lexical.function->bindings[b-1].parent) {
        const XrXirSourceDebugBinding *binding=&lexical.function->bindings[b-1];
        if (strcmp(lexical.source->declarations[binding->declaration-1].name,"values")) continue;
        CHECK(binding->flags&XR_XIR_SOURCE_DEBUG_CELL);
        CHECK(xr_xir_vm_debug_snapshot_value(adapter->snapshot,0,binding->value,cell)==XR_XIR_VALUE_OK && xr_xir_cell_is_projection(cell)==projection);return binding->value;
    }
    CHECK(false);return UINT32_MAX;
}
static void ref_caller_hidden(XdapAdapter *adapter) {
    XdapLexicalView lexical={0};CHECK(xdap_lexical_open(adapter->launch,adapter->snapshot,1,&lexical)==XR_XIR_OK);bool root=false;
    for (uint32_t b=lexical.environment;b;b=lexical.function->bindings[b-1].parent) {
        const XrXirSourceDebugBinding *binding=&lexical.function->bindings[b-1];
        if (strcmp(lexical.source->declarations[binding->declaration-1].name,"root")) continue;
        XrXirValue unavailable={0};CHECK(binding->flags&XR_XIR_SOURCE_DEBUG_CELL);
        CHECK(xr_xir_vm_debug_snapshot_cell_value(adapter->snapshot,1,binding->value,&unavailable)==XR_XIR_VALUE_UNSUPPORTED);root=true;
    }
    CHECK(root);char message[256];origin_message(message,sizeof(message),adapter->frames[1].scope);request(adapter,message);
    XrJsonValue *reply=last_response("variables",true),*values=xjson_get_array(xjson_get_object(reply,"body"),"variables");bool saved=false;
    for (int i=0;i<xjson_array_len(values);++i) {
        const char *name=xjson_get_string(xjson_array_get(values,i),"name");CHECK(strcmp(name,"root"));if (!strcmp(name,"saved")) saved=true;
    }
    CHECK(saved);xjson_free(reply);capture.count=0;
}
static void ref_output(XdapAdapter *adapter,bool object,bool changed) {
    request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);
    const char *expected[]={changed ? "100\n" : "11\n","11\n","13\n",changed ? "117\n" : "24\n",changed ? "100\n" : "11\n","37\n","11\n",changed ? "100\n" : "11\n"};
    uint32_t found=0,count=object ? 8 : 7;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *message=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(message,"event");
        if (event && !strcmp(event,"output")) { CHECK(found<count);CHECK(!strcmp(xjson_get_string(xjson_get_object(message,"body"),"output"),expected[found++])); }
        xjson_free(message);
    }
    CHECK(found==count);
}
static size_t ref_case(bool object,size_t fail) {
    WriteFixture fixture=ref_open(object);XdapAdapter *adapter=fixture.adapter;XrXirValue cell={0};uint32_t operand=ref_operand(adapter,&cell,true);
    XrXirValuePath facts={0};CHECK(xir_cell_path_view(&cell,&facts)==XR_XIR_VALUE_OK && facts.count==(object ? 1u : 2u));
    XrXirVmDebugSnapshot *old=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&old)==XR_XIR_VALUE_OK);
    XrXirValue prior={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(old,0,operand,&prior)==XR_XIR_VALUE_OK);ref_numbers(&prior,fixture.domain,11,13);
    uint32_t scope=adapter->frames[0].scope;uint64_t epoch=adapter->stop_epoch;XrXirVmDebugFrameInfo original=write_frame(old);
    if (!object) ref_caller_hidden(adapter);
    child_set(adapter,adapter->frames[1].scope,object ? "box" : "root",object ? "box" : "saved",false);
    CHECK(adapter->stop_epoch==epoch && adapter->snapshot==old);capture.count=0;
    char message[512];size_t length=child_message(message,sizeof(message),scope,"values","incoming");uint32_t before_refs=adapter->value_count,next=adapter->next_handle;
    size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;runtime_fail_at=fail==SIZE_MAX ? SIZE_MAX : begin+fail;
    XrXirStatus status=xdap_adapter_request(adapter,message,length);size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (fail==SIZE_MAX) {
        CHECK(status==XR_XIR_OK && adapter->stop_epoch==epoch+1 && adapter->value_count==1);
        XrJsonValue *reply=last_response("setVariable",true);uint32_t result=(uint32_t)xjson_get_int(xjson_get_object(reply,"body"),"variablesReference");xjson_free(reply);capture.count=0;
        CHECK(result==adapter->values[0].handle && adapter->values[0].origin.operand==operand);
        XrXirValue current={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(adapter->snapshot,0,operand,&current)==XR_XIR_VALUE_OK);
        ref_numbers(&current,fixture.domain,100,13);xr_xir_value_drop(&current);
        CHECK(!child_set(adapter,result,"[1]","right",true) && adapter->stop_epoch==epoch+2);
        XrXirVmDebugFrameInfo same=write_frame(adapter->snapshot);CHECK(same.id==original.id && same.instruction==original.instruction && same.thread==original.thread);
        if (!object) ref_caller_hidden(adapter);
    } else {
        CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && adapter->snapshot==old && adapter->stop_epoch==epoch);
        CHECK(runtime_live==blocks && runtime_bytes==bytes && adapter->value_count==before_refs && adapter->next_handle==next);
    }
    ref_numbers(&prior,fixture.domain,11,13);ref_output(adapter,object,fail==SIZE_MAX);
    CHECK(xdap_adapter_free(adapter)==XR_XIR_CALL_READY);ref_numbers(&prior,fixture.domain,11,13);
    CHECK(xir_cell_path_view(&cell,&facts)==XR_XIR_VALUE_OK && facts.count==(object ? 1u : 2u));
    xr_xir_value_drop(&cell);xr_xir_value_drop(&prior);xr_xir_vm_debug_snapshot_drop(old);write_finish(&fixture);return sites;
}
static void ref_whole_aliases(void) {
    WriteFixture fixture=write_open(XR_DAP_REF_WHOLE_FIXTURE);XdapAdapter *adapter=fixture.adapter;char message[2048];
    snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":6}]}}",XR_DAP_REF_WHOLE_FIXTURE);
    request(adapter,message);request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);capture.count=0;
    XrXirValue cell={0};ref_operand(adapter,&cell,false);uint32_t frames[64],slots[64],count=0;
    for (uint32_t f=0;f<adapter->depth;++f) {
        XrXirVmDebugFrameInfo info={0};XrXirValue name={0};CHECK(xr_xir_vm_debug_snapshot_frame(adapter->snapshot,f,&info,&name)==XR_XIR_VALUE_OK);xr_xir_value_drop(&name);
        for (uint32_t v=0;v<info.value_count;++v) {
            XrXirValue value={0};if (xr_xir_vm_debug_snapshot_value(adapter->snapshot,f,v,&value)!=XR_XIR_VALUE_OK) continue;
            if (xr_xir_cell_is_projection(&value) && xr_xir_cell_same_owner(&cell,&value)) {
                XrXirValuePath path={0};CHECK(xir_cell_path_view(&value,&path)==XR_XIR_VALUE_OK);
                if (path.count==1 && path.steps[0].kind==XR_XIR_PATH_INDEX && path.steps[0].selector==0) {
                    XrXirValue number={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(adapter->snapshot,f,v,&number)==XR_XIR_VALUE_OK && number.payload==11);
                    xr_xir_value_drop(&number);CHECK(count<64);frames[count]=f;slots[count++]=v;
                }
            }
            xr_xir_value_drop(&value);
        }
    }
    CHECK(count);uint32_t scope=adapter->frames[0].scope;uint64_t epoch=adapter->stop_epoch;
    CHECK(!child_set(adapter,scope,"values","empty",true));CHECK(adapter->stop_epoch==epoch+1);
    for (uint32_t i=0;i<count;++i) { XrXirValue unavailable={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(adapter->snapshot,frames[i],slots[i],&unavailable)==XR_XIR_VALUE_UNSUPPORTED); }
    uint32_t restored=child_set(adapter,scope,"values","incoming",true);CHECK(restored && adapter->stop_epoch==epoch+2);
    for (uint32_t i=0;i<count;++i) {
        XrXirValue number={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(adapter->snapshot,frames[i],slots[i],&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==100);xr_xir_value_drop(&number);
    }
    CHECK(!child_set(adapter,restored,"[1]","right",true));
    request(adapter,"{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":1}}");capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);
    const char *expected[]={"100\n","11\n","13\n","117\n","100\n","17\n"};uint32_t found=0;
    for (uint32_t i=0;i<capture.count;++i) {
        XrJsonValue *reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) { CHECK(found<6 && !strcmp(xjson_get_string(xjson_get_object(reply,"body"),"output"),expected[found++])); }
        xjson_free(reply);
    }
    CHECK(found==6);xr_xir_value_drop(&cell);write_dispose(&fixture);
    printf("Whole-root loan: actual readable descendant aliases=%u, bounds unavailable then restored100, physical=0/0 PASS\n",count);
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    ref_whole_aliases();
    for (uint32_t kind=0;kind<2;++kind) {
        size_t sites=ref_case(kind!=0,SIZE_MAX);CHECK(sites);
        for (size_t i=0;i<sites;++i) if (resources || !i || i==sites-1) ref_case(kind!=0,i);
        printf("Actual projected ref write: class=%u sites=%zu injection=%s old11/current100-17/caller-denied/owned=0 PASS\n",kind,sites,resources ? "ALL" : "FIRST_LAST_ONLY");
    }
    return 0;
}
