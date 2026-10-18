/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_wrapper_rebind.c - Whole immutable values change only through their Cells
 */
#define XR_DAP_CHILD_ENTRY reference_rebind_child_main
#include "test_child_write.c"
#undef XR_DAP_CHILD_ENTRY
#include "xir/xxir_tuple.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_enum.h"
static XrXirValue rebind_owned(XdapAdapter *adapter,const XrXirVmDebugSnapshot *snapshot,const char *name,uint32_t *operand) {
    XdapLexicalView lexical={0};CHECK(xdap_lexical_open(adapter->launch,snapshot,0,&lexical)==XR_XIR_OK);
    for (uint32_t b=lexical.environment;b;b=lexical.function->bindings[b-1].parent) {
        const XrXirSourceDebugBinding *binding=&lexical.function->bindings[b-1];
        if (strcmp(lexical.source->declarations[binding->declaration-1].name,name)) continue;
        XrXirValue value={0};const char *text=NULL;size_t size=0;
        CHECK(xdap_lexical_read(&lexical,b,&value,&text,&size)==XR_XIR_VALUE_OK);
        if (operand) *operand=binding->value;return value;
    }
    CHECK(false);return (XrXirValue){0};
}
static void rebind_pair(const XrXirValue *value,bool changed) {
    XrXirValue number={0},text={0};CHECK(xr_xir_tuple_get(value,0,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==(changed?99:11));
    CHECK(xr_xir_tuple_get(value,1,&text)==XR_XIR_VALUE_OK);const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&text,&bytes,&length) && length==(changed?9u:3u) && !memcmp(bytes,changed?"new\0value":"old",length));
    xr_xir_value_drop(&number);xr_xir_value_drop(&text);
}
static void rebind_optional(const XrXirValue *value,int stage) {
    bool some=false;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(value,&some,&payload));
    CHECK(some==(stage!=1));if (some) CHECK(payload && payload->type==XR_XIR_I64 && payload->payload==(stage<1?11:99));else CHECK(!payload);
}
static void rebind_choice(const XrXirValue *value,int stage) {
    uint32_t variant=UINT32_MAX;CHECK(xr_xir_enum_variant(value,&variant)==XR_XIR_VALUE_OK);
    uint32_t expected=stage==4 ? 1u : stage==5 ? 2u : 0u;CHECK(variant==expected);
    XrXirEnumBorrow borrow={0};CHECK(xr_xir_enum_borrow(value,&borrow)==XR_XIR_VALUE_OK && borrow.field_count==(expected==2 ? 0u : 1u));
    const char *names[]={"Number","Text","Empty"};CHECK(borrow.member.length==strlen(names[expected]) && !memcmp(borrow.member.bytes,names[expected],borrow.member.length));
    if (!expected) CHECK(borrow.fields[0].type==XR_XIR_I64 && borrow.fields[0].payload==(stage<3 ? 11 : 99));
    if (expected==1) {const char *text=NULL;size_t length=0;CHECK(xr_xir_string_view(&borrow.fields[0],&text,&length) && length==3 && !memcmp(text,"new",3));}
}
static void rebind_state(XdapAdapter *adapter,int stage) {
    XrXirValue pair=rebind_owned(adapter,adapter->snapshot,"pair",NULL),optional=rebind_owned(adapter,adapter->snapshot,"optional",NULL),choice=rebind_owned(adapter,adapter->snapshot,"choice",NULL);
    rebind_pair(&pair,stage>=0);rebind_optional(&optional,stage);rebind_choice(&choice,stage);
    xr_xir_value_drop(&pair);xr_xir_value_drop(&optional);xr_xir_value_drop(&choice);
}
static uint32_t rebind_reply(XdapAdapter *adapter,unsigned stage) {
    XrJsonValue *reply=last_response("setVariable",true),*body=xjson_get_object(reply,"body");uint32_t handle=(uint32_t)xjson_get_int(body,"variablesReference");
    bool empty=stage==1 || stage==5;CHECK((handle==0)==empty);
    if (!empty) CHECK(xjson_get_int(body,stage==2 ? "namedVariables" : "indexedVariables")==(!stage ? 2 : 1));
    xjson_free(reply);capture.count=0;if (empty) return 0;
    char message[256];origin_message(message,sizeof(message),handle);request(adapter,message);reply=last_response("variables",true);
    XrJsonValue *values=xjson_get_array(xjson_get_object(reply,"body"),"variables");CHECK(xjson_array_len(values)==(!stage ? 2 : 1));
    XrJsonValue *first=xjson_array_get(values,0);CHECK(!strcmp(xjson_get_string(first,"name"),stage==2 ? "value" : "[0]"));
    CHECK(!strcmp(xjson_get_string(first,"value"),stage==4 ? "new" : "99"));
    if (!stage) {XrJsonValue *text=xjson_get(xjson_array_get(values,1),"value");CHECK(text->string_len==9 && !memcmp(text->as.string,"new\0value",9));}
    xjson_free(reply);capture.count=0;return handle;
}
static size_t rebind_case(bool child,unsigned end,size_t fail) {
    CHECK(end<7);const char *file=child ? XR_DAP_REBIND_CHILD_FIXTURE : XR_DAP_REBIND_FIXTURE;WriteFixture fixture=write_open(file);XdapAdapter *adapter=fixture.adapter;char message[2048];
    snprintf(message,sizeof(message),"{\"seq\":3,\"type\":\"request\",\"command\":\"setBreakpoints\",\"arguments\":{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":19}]}}",file);request(adapter,message);
    request(adapter,"{\"seq\":4,\"type\":\"request\",\"command\":\"configurationDone\"}");
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->snapshot && !adapter->running);capture.count=0;
    XrXirVmDebugSnapshot *old=NULL;CHECK(xdap_current_snapshot(adapter->launch->control,&old)==XR_XIR_VALUE_OK);
    const char *names[]={"pair","optional","choice"};uint32_t operands[3];XrXirValue originals[3];
    for (uint32_t i=0;i<3;++i) originals[i]=rebind_owned(adapter,old,names[i],&operands[i]);
    XrXirVmDebugFrameInfo frame=write_frame(old);CHECK(child==(frame.thread!=1));uint32_t scope=adapter->frames[0].scope;
    const char *targets[]={"pair","optional","optional","choice","choice","choice","choice"};
    const char *sources[]={"incomingPair","absent","incomingOptional","incomingChoice","textChoice","emptyChoice","incomingChoice"};
    uint64_t initial=adapter->stop_epoch;size_t sites=0;uint32_t previous=0;
    child_set(adapter,scope,"beforePair","incomingPair",false);child_set(adapter,scope,"pair","incomingOptional",false);
    child_set(adapter,scope,"beforeOptional","incomingOptional",false);child_set(adapter,scope,"beforeChoice","incomingChoice",false);
    child_set(adapter,scope,"optional","incomingPair",false);child_set(adapter,scope,"choice","incomingOptional",false);
    CHECK(adapter->stop_epoch==initial);rebind_state(adapter,-1);
    for (unsigned stage=0;stage<=end;++stage) {
        XrXirVmDebugSnapshot *snapshot=adapter->snapshot;uint64_t epoch=adapter->stop_epoch;uint32_t refs=adapter->value_count,next=adapter->next_handle;
        CHECK(refs<=1);XdapValueRef kept={0};if (refs) memcpy(&kept,&adapter->values[0],sizeof(kept));
        XrCompileResourceStats compiler={0};CHECK(xr_compile_resources_stats(fixture.ledger,&compiler)==XR_COMPILE_RESOURCE_OK);
        size_t length=child_message(message,sizeof(message),scope,targets[stage],sources[stage]);size_t blocks=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;
        runtime_fail_at=stage==end && fail!=SIZE_MAX ? begin+fail : SIZE_MAX;
        XrXirStatus status=xdap_adapter_request(adapter,message,length);sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
        if (stage==end && fail!=SIZE_MAX) {
            CHECK(status==XR_XIR_OUT_OF_MEMORY && sites>fail && adapter->snapshot==snapshot && adapter->stop_epoch==epoch);
            CHECK(!capture.count);
            CHECK(runtime_live==blocks && runtime_bytes==bytes && adapter->value_count==refs && adapter->next_handle==next);
            if (refs) CHECK(!memcmp(&kept,&adapter->values[0],sizeof(kept)));
            XrCompileResourceStats after={0};CHECK(xr_compile_resources_stats(fixture.ledger,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==compiler.live_bytes && after.work>=compiler.work);
            rebind_state(adapter,(int)stage-1);break;
        }
        CHECK(status==XR_XIR_OK && adapter->stop_epoch==epoch+1);uint32_t returned=rebind_reply(adapter,stage);rebind_state(adapter,(int)stage);
        if (previous) {origin_message(message,sizeof(message),previous);request(adapter,message);XrJsonValue *retired=last_response("variables",false);xjson_free(retired);capture.count=0;}
        previous=returned;
        XrXirVmDebugFrameInfo same=write_frame(adapter->snapshot);CHECK(same.id==frame.id && same.instruction==frame.instruction && same.thread==frame.thread);
        if (returned) {child_set(adapter,returned,stage==2 ? "value" : "[0]",stage==4 ? "replacementText" : "7",false);CHECK(adapter->stop_epoch==epoch+1);rebind_state(adapter,(int)stage);}
    }
    rebind_pair(&originals[0],false);rebind_optional(&originals[1],-1);rebind_choice(&originals[2],-1);
    snprintf(message,sizeof(message),"{\"seq\":8,\"type\":\"request\",\"command\":\"continue\",\"arguments\":{\"threadId\":%llu}}",(unsigned long long)frame.thread);request(adapter,message);capture.count=0;
    CHECK(xdap_adapter_step(adapter,1000)==XR_XIR_OK && adapter->terminal);const int scores[]={66,154,142,242,330,234,224,330};
    char expected[32];snprintf(expected,sizeof(expected),"%d\n",scores[end+(fail==SIZE_MAX ? 1u : 0u)]);uint32_t outputs=0;
    for (uint32_t i=0;i<capture.count;++i) {XrJsonValue *reply=xjson_parse(capture.messages[i].bytes,capture.messages[i].size);const char *event=xjson_get_string(reply,"event");
        if (event && !strcmp(event,"output")) CHECK(!outputs++ && !strcmp(xjson_get_string(xjson_get_object(reply,"body"),"output"),expected));xjson_free(reply);}
    CHECK(outputs==1 && xdap_adapter_free(adapter)==XR_XIR_CALL_READY);
    for (uint32_t i=0;i<3;++i) {XrXirValue retained={0};CHECK(xr_xir_vm_debug_snapshot_cell_value(old,0,operands[i],&retained)==XR_XIR_VALUE_OK && retained.payload==originals[i].payload);xr_xir_value_drop(&retained);}
    rebind_pair(&originals[0],false);rebind_optional(&originals[1],-1);rebind_choice(&originals[2],-1);
    for (uint32_t i=0;i<3;++i) xr_xir_value_drop(&originals[i]);xr_xir_vm_debug_snapshot_drop(old);write_finish(&fixture);return sites;
}
int main(int argc,char **argv) {
    xr_test_suppress_dialogs();CHECK(argc==2 || argc==3);bool child=!strcmp(argv[1],"--child"),resources=argc==3;
    CHECK(child || !strcmp(argv[1],"--root"));CHECK(!resources || !strcmp(argv[2],"--resources"));
    for (unsigned stage=0;stage<7;++stage) {size_t sites=rebind_case(child,stage,SIZE_MAX);CHECK(sites);
        for (size_t i=0;i<sites;++i) if (resources || !i || i==sites-1) rebind_case(child,stage,i);
        printf("Whole wrapper: child=%u stage=%u sites=%zu injection=%s typed/NUL/immutable/old11/Source/PC/physical0 PASS\n",child,stage,sites,resources ? "ALL" : "FIRST_LAST_ONLY");}
    return 0;
}
