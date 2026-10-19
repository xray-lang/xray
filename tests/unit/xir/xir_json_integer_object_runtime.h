/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_integer_object_runtime.h - Exact schema and decoded key expectations
 */
#ifndef XIR_JSON_INTEGER_OBJECT_RUNTIME_H
#define XIR_JSON_INTEGER_OBJECT_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_array.h"
#include <inttypes.h>
typedef struct JsonObjectExpected {bool some;uint32_t count;int64_t first,second;} JsonObjectExpected;
static XrXirValue json_object_call(XrXirProgram *program,uint32_t entry,const char *bytes,size_t length) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};CHECK(xr_xir_string_new(domain,bytes,length,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,&argument,1)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);
    XrXirCallStatus status;
    do {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;}while(status==XR_XIR_CALL_READY);
    CHECK(status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return result;
}
static void json_object_expected(const XrXirValue *value,JsonObjectExpected expected) {
    bool some=false;const XrXirValue *array=NULL;
    CHECK(xr_xir_value_valid(value) && xr_xir_nullable_view(value,&some,&array) && some==expected.some);
    if(!some){CHECK(!array);return;}
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.work=65536,.scratch_bytes=65536};
    int64_t count=-1;CHECK(array && xr_xir_array_len(array,&admission,&count)==XR_XIR_VALUE_OK && count==expected.count);
    for(uint32_t i=0;i<expected.count;++i) {
        XrXirValue field={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(array,i,&admission,&field,&fault)==XR_XIR_VALUE_OK);
        CHECK(field.type==XR_XIR_I64 && !field.reserved && field.payload==(i?expected.second:expected.first));
        xr_xir_value_drop(&field);
    }
}
static void json_object_case(XrXirProgram *program,uint32_t entry,const char *text,size_t size,JsonObjectExpected expected) {
    XrXirValue result=json_object_call(program,entry,text,size);json_object_expected(&result,expected);xr_xir_value_drop(&result);
}
static void json_object_oracles(XrXirProgram *program,const uint32_t entries[6]) {
    const struct {const char *text;int64_t left,right;} accepted[]={
        {"{\"left\":1,\"right\":2}",1,2},{"{\"right\":2,\"left\":1}",1,2},
        {" \t\r\n{ \"right\" : -9, \"le\\u0066t\" : 7 }\r\n",7,-9},
        {"{\"left\":-9223372036854775808,\"right\":9223372036854775807}",INT64_MIN,INT64_MAX},
        {"{\"left\":-0,\"right\":0}",0,0}};
    size_t cases=0;
    for(unsigned i=0;i<sizeof(accepted)/sizeof(*accepted);++i) {
        const char *s=accepted[i].text;
        json_object_case(program,entries[1],s,strlen(s),(JsonObjectExpected){true,2,accepted[i].left,accepted[i].right});
        json_object_case(program,entries[2],s,strlen(s),(JsonObjectExpected){true,2,accepted[i].right,accepted[i].left});cases+=2;
    }
    const char *rejected[]={""," ","{}","[]","null","{\"left\":1}","{\"right\":2}",
        "{\"left\":1,\"right\":2,\"extra\":3}","{\"left\":1,\"left\":2}",
        "{\"left\":1,\"le\\u0066t\":2,\"right\":3}","{\"LEFT\":1,\"right\":2}",
        "{\"left\":1,\"right\":2,}","{,\"left\":1,\"right\":2}","{\"left\" 1,\"right\":2}",
        "{\"left\":1 \"right\":2}","{\"left\":1,,\"right\":2}","{\"left\":1,\"right\":2}x",
        "{\"left\":true,\"right\":2}","{\"left\":null,\"right\":2}","{\"left\":[],\"right\":2}",
        "{\"left\":{},\"right\":2}","{\"left\":\"1\",\"right\":2}","{\"left\":1.0,\"right\":2}",
        "{\"left\":1e0,\"right\":2}","{\"left\":01,\"right\":2}","{\"left\":+1,\"right\":2}",
        "{\"left\":9223372036854775808,\"right\":2}","{\"left\":-9223372036854775809,\"right\":2}",
        "{\"left\\ud800\":1,\"right\":2}","{\"left\":1,\"right\":2}{\"left\":3,\"right\":4}"};
    for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        json_object_case(program,entries[1],rejected[i],strlen(rejected[i]),(JsonObjectExpected){0});++cases;
    }
    const char *valid="{\"left\":1,\"right\":2}";
    for(size_t n=0;n<strlen(valid);++n){json_object_case(program,entries[1],valid,n,(JsonObjectExpected){0});++cases;}
    for(unsigned c=0;c<128;++c) {
        char text[64];int n=snprintf(text,sizeof(text),"{\"left\":1,%c\"right\":2}",c);CHECK(n>0 && n<(int)sizeof(text));
        bool space=c==9 || c==10 || c==13 || c==32;
        json_object_case(program,entries[1],text,(size_t)n,(JsonObjectExpected){space,space?2u:0u,1,2});++cases;
    }
    const char *unicode="{\"\\ud83d\\ude00\":-3,\"\\u6c49\":4}";
    json_object_case(program,entries[3],unicode,strlen(unicode),(JsonObjectExpected){true,2,4,-3});++cases;
    const char *empty[]={"{}"," \n{ \t}\r"};
    for(unsigned i=0;i<2;++i){json_object_case(program,entries[4],empty[i],strlen(empty[i]),(JsonObjectExpected){true,0,0,0});++cases;}
    json_object_case(program,entries[4],"{\"left\":1}",10,(JsonObjectExpected){0});++cases;
    json_object_case(program,entries[5],"{\"left\":1}",10,(JsonObjectExpected){0});++cases;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entries[0],NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==42);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    printf("Ordinary exact integer object: %zu independent cases, decoded/reordered keys, strict schema and entry42\n",cases);
}
#endif // XIR_JSON_INTEGER_OBJECT_RUNTIME_H
