/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_typed_decode_runtime.h - Independent heterogeneous init observations
 */
#ifndef XIR_JSON_TYPED_DECODE_RUNTIME_H
#define XIR_JSON_TYPED_DECODE_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
typedef struct JsonTypedKeys {const char *label,*active,*count;} JsonTypedKeys;
typedef struct JsonTypedExpected {bool some;const char *label;size_t length;bool active;int64_t count;} JsonTypedExpected;
static XrXirValue named_call(XrXirProgram *program,uint32_t entry,const char *text,size_t length) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};CHECK(xr_xir_string_new(domain,text,length,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,&argument,1)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);XrXirCallStatus status;
    do {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;}while(status==XR_XIR_CALL_READY);
    CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return result;
}
static void named_expected(const XrXirValue *value,JsonTypedExpected expected) {
    bool some=false;const XrXirValue *tuple=NULL;
    CHECK(xr_xir_value_valid(value) && xr_xir_nullable_view(value,&some,&tuple) && some==expected.some);
    if(!some){CHECK(!tuple);return;}
    XrXirValue fields[3]={{0}};for(unsigned i=0;i<3;++i)CHECK(xr_xir_tuple_get(tuple,i,&fields[i])==XR_XIR_VALUE_OK);
    const char *text=NULL;size_t length=0;CHECK(fields[0].type==XR_XIR_STRING && xr_xir_string_view(&fields[0],&text,&length));
    CHECK(expected.label && length==expected.length && !memcmp(text,expected.label,length));
    CHECK(fields[1].type==XR_XIR_BOOL && !fields[1].reserved && fields[1].payload==(int64_t)expected.active);
    CHECK(fields[2].type==XR_XIR_I64 && !fields[2].reserved && fields[2].payload==expected.count);
    for(unsigned i=0;i<3;++i)xr_xir_value_drop(&fields[i]);
}
static void named_case(XrXirProgram *program,uint32_t entry,const char *text,JsonTypedExpected expected) {
    XrXirValue result=named_call(program,entry,text,strlen(text));named_expected(&result,expected);xr_xir_value_drop(&result);
}
static void named_oracles(XrXirProgram *program,const uint32_t entries[8],const JsonTypedKeys *keys) {
    const struct {const char *label,*active,*count;JsonTypedExpected expected;} cases[]={
        {"\"ok\"","true","42",{true,"ok",2,true,42}},
        {"\"\\u6c49\\ud83d\\ude00\"","false","-7",{true,"汉😀",7,false,0}},
        {"\"a\\u0000b\"","true","0",{true,"a\0b",3,true,0}},
        {"\"\\n\\\"\\\\\"","false","9223372036854775807",{true,"\n\"\\",3,false,INT64_MAX}},
        {"null","true","1",{0}},{"\"ok\"","null","1",{0}},{"\"ok\"","1","1",{0}},
        {"\"ok\"","true","1.0",{0}},{"\"ok\"","true","1e0",{0}},{"\"ok\"","true","9223372036854775808",{0}},
        {"\"\\ud800\"","true","1",{0}},{"\"ok\"","True","1",{0}},{"\"ok\"","true","01",{0}},
        {"\"ok\"","true","1",{true,"ok",2,true,1}}};
    size_t total=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        char text[512];int n=snprintf(text,sizeof(text),"{\"%s\":%s,\"%s\":%s,\"%s\":%s}",
            keys->active,cases[i].active,keys->count,cases[i].count,keys->label,cases[i].label);CHECK(n>0 && n<(int)sizeof(text));
        named_case(program,entries[1],text,cases[i].expected);++total;
    }
    char text[512];int n=snprintf(text,sizeof(text),"{\"%s\":\"ok\"}",keys->label);CHECK(n>0 && n<(int)sizeof(text));
    named_case(program,entries[1],text,(JsonTypedExpected){0});++total;
    n=snprintf(text,sizeof(text),"{\"%s\":\"ok\",\"%s\":\"other\",\"%s\":true,\"%s\":1}",keys->label,keys->label,keys->active,keys->count);
    CHECK(n>0 && n<(int)sizeof(text));named_case(program,entries[1],text,(JsonTypedExpected){0});++total;
    const char *rejected[]={"{}","[]","null","","{\"extra\":1}"};
    for(unsigned i=0;i<5;++i){named_case(program,entries[1],rejected[i],(JsonTypedExpected){0});++total;}
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entries[0],NULL,0)==XR_XIR_CALL_READY);XrXirCallStatus status;
    do {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;}while(status==XR_XIR_CALL_READY);
    XrXirValue result={0};CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==42);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    printf("Typed JSON initializer:%zu independent string/bool/i64 cases and entry42, keys %s/%s/%s\n",total,keys->label,keys->active,keys->count);
}
#endif // XIR_JSON_TYPED_DECODE_RUNTIME_H
