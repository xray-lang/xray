/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_scalar_object_runtime.h - Exact scalar tokens and Boolean admission
 */
#ifndef XIR_JSON_SCALAR_OBJECT_RUNTIME_H
#define XIR_JSON_SCALAR_OBJECT_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_array.h"
typedef struct JsonScalarExpected {bool some;uint32_t count;const char *items[3];} JsonScalarExpected;
static XrXirValue json_scalar_call(XrXirProgram *program,uint32_t entry,const char *bytes,size_t length) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};CHECK(xr_xir_string_new(domain,bytes,length,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,&argument,1)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);XrXirCallStatus status;
    do {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;}while(status==XR_XIR_CALL_READY);
    CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return result;
}
static void json_scalar_expected(const XrXirValue *value,JsonScalarExpected expected) {
    bool some=false;const XrXirValue *array=NULL;CHECK(xr_xir_value_valid(value) && xr_xir_nullable_view(value,&some,&array) && some==expected.some);
    if(!some){CHECK(!array);return;}
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.domain=domain,.work=65536,.scratch_bytes=65536};int64_t count=-1;
    CHECK(array && xr_xir_array_len(array,&admission,&count)==XR_XIR_VALUE_OK && count==expected.count);
    for(int64_t i=0;i<count;++i) {
        XrXirValue field={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(array,i,&admission,&field,&fault)==XR_XIR_VALUE_OK);
        const char *bytes=NULL;size_t length=0;CHECK(field.type==XR_XIR_STRING && xr_xir_string_view(&field,&bytes,&length));
        CHECK(expected.items[i] && length==strlen(expected.items[i]) && !memcmp(bytes,expected.items[i],length));xr_xir_value_drop(&field);
    }
    xr_xir_domain_drop(domain);
}
static void json_scalar_case(XrXirProgram *program,uint32_t entry,const char *text,size_t size,JsonScalarExpected expected) {
    XrXirValue result=json_scalar_call(program,entry,text,size);json_scalar_expected(&result,expected);xr_xir_value_drop(&result);
}
static void json_scalar_boolean(XrXirProgram *program,uint32_t entry,const char *text,bool accepted,bool expected) {
    XrXirValue result=json_scalar_call(program,entry,text,strlen(text));bool some=false;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(&result,&some,&payload) && some==accepted);
    if(some)CHECK(payload && payload->type==XR_XIR_BOOL && !payload->reserved && payload->payload==(int64_t)expected);else CHECK(!payload);
    xr_xir_value_drop(&result);
}
static void json_scalar_oracles(XrXirProgram *program,const uint32_t entries[8]) {
    const struct {const char *text,*label,*active,*count;} accepted[]={
        {"{\"label\":\"x\",\"active\":true,\"count\":42}","\"x\"","true","42"},
        {" \t{\"count\":1.25E+999999,\"active\":false,\"la\\u0062el\":\"汉😀\"}\r\n","\"汉😀\"","false","1.25E+999999"},
        {"{\"label\":\"\\u6c49\\ud83d\\ude00\",\"active\":null,\"count\":-0}","\"\\u6c49\\ud83d\\ude00\"","null","-0"},
        {"{\"label\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\",\"active\":true,\"count\":9223372036854775808}","\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"","true","9223372036854775808"}};
    size_t cases=0;
    for(unsigned i=0;i<sizeof(accepted)/sizeof(*accepted);++i) {
        const char *s=accepted[i].text;
        json_scalar_case(program,entries[1],s,strlen(s),(JsonScalarExpected){true,3,{accepted[i].label,accepted[i].active,accepted[i].count}});
        json_scalar_case(program,entries[2],s,strlen(s),(JsonScalarExpected){true,3,{accepted[i].count,accepted[i].label,accepted[i].active}});cases+=2;
    }
    const char *rejected[]={""," ","null","[]","{}","{\"label\":\"x\"}",
        "{\"label\":\"x\",\"active\":true}","{\"label\":\"x\",\"active\":true,\"extra\":42}",
        "{\"label\":\"x\",\"active\":true,\"count\":42,\"count\":43}",
        "{\"label\":\"x\",\"la\\u0062el\":\"y\",\"active\":true,\"count\":42}",
        "{\"label\":\"x\",\"active\":True,\"count\":42}","{\"label\":\"x\",\"active\":truex,\"count\":42}",
        "{\"label\":\"\\ud800\",\"active\":true,\"count\":42}","{\"label\":\"x\",\"active\":true,\"count\":01}",
        "{\"label\":\"x\",\"active\":true,\"count\":+1}","{\"label\":\"x\",\"active\":true,\"count\":1e}",
        "{\"label\":\"x\",\"active\":true,\"count\":[]}","{\"label\":\"x\",\"active\":true,\"count\":{}}",
        "{\"label\":\"x\",\"active\":true,\"count\":42,}","{\"label\":\"x\",\"active\":true,\"count\":42}x"};
    for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);++i){json_scalar_case(program,entries[1],rejected[i],strlen(rejected[i]),(JsonScalarExpected){0});++cases;}
    const char *valid=accepted[0].text;
    for(size_t n=0;n<strlen(valid);++n){json_scalar_case(program,entries[1],valid,n,(JsonScalarExpected){0});++cases;}
    for(unsigned c=0;c<128;++c) {
        char text[128];int n=snprintf(text,sizeof(text),"{\"label\":\"x\",%c\"active\":true,\"count\":42}",c);CHECK(n>0 && n<(int)sizeof(text));
        bool space=c==9 || c==10 || c==13 || c==32;
        json_scalar_case(program,entries[1],text,(size_t)n,(JsonScalarExpected){space,space?3u:0u,{"\"x\"","true","42"}});++cases;
    }
    const char *unicode="{\"\\ud83d\\ude00\":null,\"\\u6c49\":\"汉\"}";
    json_scalar_case(program,entries[3],unicode,strlen(unicode),(JsonScalarExpected){true,2,{"\"汉\"","null",NULL}});++cases;
    json_scalar_case(program,entries[4]," \r{ }\n",6,(JsonScalarExpected){true,0,{NULL}});++cases;
    json_scalar_case(program,entries[4],"{\"label\":0}",11,(JsonScalarExpected){0});++cases;
    json_scalar_case(program,entries[5],"{\"label\":0}",11,(JsonScalarExpected){0});++cases;
    json_scalar_boolean(program,entries[7],"true",true,true);json_scalar_boolean(program,entries[7],"false",true,false);cases+=2;
    const char *bad_bool[]={"","True","False","TRUE","FALSE","null","0","1"," true","true ","false\n","truefalse","tru","fals","\"true\""};
    for(unsigned i=0;i<sizeof(bad_bool)/sizeof(*bad_bool);++i){json_scalar_boolean(program,entries[7],bad_bool[i],false,false);++cases;}
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entries[0],NULL,0)==XR_XIR_CALL_READY);XrXirCallStatus status;
    do {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;}while(status==XR_XIR_CALL_READY);
    XrXirValue result={0};CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==42);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    printf("Ordinary scalar object:%zu independent exact tokens/Boolean/Unicode/schema cases and entry42\n",cases);
}
#endif // XIR_JSON_SCALAR_OBJECT_RUNTIME_H
