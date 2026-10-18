/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_numbers_runtime.h - Independent JSON grammar and signed range cases
 */
#ifndef XIR_JSON_NUMBERS_RUNTIME_H
#define XIR_JSON_NUMBERS_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_type_arena.h"
#include <inttypes.h>
typedef struct JsonNumberExpected {bool token,integer;int64_t value;} JsonNumberExpected;
typedef struct JsonNumberTrial {const char *text;size_t size;JsonNumberExpected expected;bool constant;} JsonNumberTrial;
static bool json_numbers_faults;
static size_t json_numbers_fault_cases,json_numbers_fault_sites;
static XrXirValue json_numbers_call(XrXirProgram *program,uint32_t entry,const char *bytes,size_t length) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};CHECK(xr_xir_string_new(domain,bytes,length,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,&argument,1)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return result;
}
static void json_numbers_expected(const XrXirValue *value,JsonNumberExpected expected) {
    const XrXirTypeArena *arena=xr_xir_value_arena(value);CHECK(arena && xr_xir_value_valid(value));
    const XrXirTypeNode *tuple=xr_xir_tuple_signature(xr_xir_compile_type_arena_types(arena),(XrXirType)value->type);
    CHECK(tuple && tuple->parameter_count==2);
    XrXirValue token={0},field={0};
    CHECK(xr_xir_tuple_get(value,0,&token)==XR_XIR_VALUE_OK && token.type==XR_XIR_BOOL &&
        !token.reserved && token.payload==(int64_t)expected.token);
    CHECK(xr_xir_tuple_get(value,1,&field)==XR_XIR_VALUE_OK);
    bool some=false;const XrXirValue *integer=NULL;
    CHECK(xr_xir_nullable_view(&field,&some,&integer) && some==expected.integer);
    if(some)CHECK(integer && integer->type==XR_XIR_I64 && !integer->reserved && integer->payload==expected.value);
    else CHECK(!integer);
    xr_xir_value_drop(&token);xr_xir_value_drop(&field);
}
static void json_numbers_trial(XrXirProgram *program,uint32_t entry,const JsonNumberTrial *trial) {
    XrXirDomain *domain=NULL;XrXirInstance *instance=NULL;XrXirValue argument={0},result={0};
    XrXirCallStatus status=XR_XIR_CALL_READY;bool driven=false;
    if(!trial->constant) {
        XrXirValueStatus made=xr_xir_domain_new(65536,&domain);
        if(made!=XR_XIR_VALUE_OK) {CHECK(made==XR_XIR_VALUE_OOM && !domain);status=XR_XIR_CALL_OOM;goto done;}
        made=xr_xir_string_new(domain,trial->text,trial->size,&argument);
        if(made!=XR_XIR_VALUE_OK) {CHECK(made==XR_XIR_VALUE_OOM && !argument.type && !argument.reserved && !argument.payload);status=XR_XIR_CALL_OOM;goto done;}
    }
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    status=xr_xir_instance_new(program,&config,&instance);if(status==XR_XIR_CALL_OOM){CHECK(!instance);goto done;}
    CHECK(status==XR_XIR_CALL_READY);
    status=xr_xir_instance_start(instance,entry,trial->constant?NULL:&argument,trial->constant?0:1);
    xr_xir_value_drop(&argument);if(domain){xr_xir_domain_drop(domain);domain=NULL;}
    if(status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;driven=status==XR_XIR_CALL_OOM;
    }
    if(status==XR_XIR_CALL_RETURNED)CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    else {
        CHECK(status==XR_XIR_CALL_OOM);XrXirCallStatus expected=xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED || !driven?
            XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_OOM;
        CHECK(xr_xir_instance_take_result(instance,&result)==expected && !result.type && !result.reserved && !result.payload);
    }
done:
    if(instance)CHECK(xr_xir_instance_free(instance)==(driven?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
    xr_xir_value_drop(&argument);if(domain)xr_xir_domain_drop(domain);
    CHECK(status==XR_XIR_CALL_RETURNED || (runtime_fail_at!=SIZE_MAX && status==XR_XIR_CALL_OOM));
    if(status==XR_XIR_CALL_RETURNED) {
        if(trial->constant)CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==42);
        else json_numbers_expected(&result,trial->expected);
    }
    xr_xir_value_drop(&result);
}
static void json_numbers_full_faults(XrXirProgram *program,uint32_t entry,const JsonNumberTrial *trial) {
    size_t held=runtime_live,bytes=runtime_bytes;CHECK(runtime_fail_at==SIZE_MAX);runtime_attempts=0;
    json_numbers_trial(program,entry,trial);size_t sites=runtime_attempts;CHECK(sites && runtime_live==held && runtime_bytes==bytes);
    for(size_t site=0;site<sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;json_numbers_trial(program,entry,trial);
        CHECK(runtime_attempts>site && runtime_live==held && runtime_bytes==bytes);
        runtime_fail_at=SIZE_MAX;json_numbers_trial(program,entry,trial);CHECK(runtime_live==held && runtime_bytes==bytes);
    }
    ++json_numbers_fault_cases;json_numbers_fault_sites+=sites;
}
static void json_numbers_case(XrXirProgram *program,uint32_t entry,const char *text,size_t size,JsonNumberExpected expected) {
    if(json_numbers_faults) {
        JsonNumberTrial trial={text,size,expected,false};json_numbers_full_faults(program,entry,&trial);return;
    }
    XrXirValue value=json_numbers_call(program,entry,text,size);json_numbers_expected(&value,expected);xr_xir_value_drop(&value);
}
static void json_numbers_oracles(XrXirProgram *program,const uint32_t entries[4]) {
    const struct {const char *text;JsonNumberExpected expected;} fixed[]={
        {"0",{true,true,0}},{"-0",{true,true,0}},{"1",{true,true,1}},{"-1",{true,true,-1}},
        {"9223372036854775807",{true,true,INT64_MAX}},{"-9223372036854775808",{true,true,INT64_MIN}},
        {"9223372036854775808",{true,false,0}},{"-9223372036854775809",{true,false,0}},
        {"18446744073709551616",{true,false,0}},{"-18446744073709551616",{true,false,0}},
        {"0.0",{true,false,0}},{"-0.0",{true,false,0}},{"1.25",{true,false,0}},
        {"1e0",{true,false,0}},{"1E+9",{true,false,0}},{"-1e-9",{true,false,0}},
        {"0.00e+00",{true,false,0}},{"-12.345E-99999",{true,false,0}},
        {"1e9999999999999999999999999999999999999",{true,false,0}}
    };
    size_t cases=0;
    for(unsigned i=0;i<sizeof(fixed)/sizeof(*fixed);++i) {
        json_numbers_case(program,entries[1],fixed[i].text,strlen(fixed[i].text),fixed[i].expected);++cases;
    }
    const char *invalid[]={"","-","+0","+1","00","01","-00","-01",".0","-.0","1.","1.e0",
        "1e","1E","1e+","1E-","1e+-1","1e--1","1e1.0","1.2.3","1e1e1","0x10","0o7","0b1",
        "NaN","Infinity","-Infinity","true","null","\"1\""," 1","1 ","\t1","1\n","[1]","{\"n\":1}","١","１"};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        json_numbers_case(program,entries[1],invalid[i],strlen(invalid[i]),(JsonNumberExpected){false,false,0});++cases;
    }
    for(unsigned bit=0;bit<63;++bit) {
        int64_t power=(int64_t)(UINT64_C(1)<<bit);
        int64_t values[]={power-1,power,-power,1-power};
        for(unsigned i=0;i<4;++i) {
            char text[32];int length=snprintf(text,sizeof(text),"%" PRId64,values[i]);CHECK(length>0 && length<(int)sizeof(text));
            json_numbers_case(program,entries[1],text,(size_t)length,(JsonNumberExpected){true,true,values[i]});++cases;
        }
    }
    for(unsigned c=0;c<128;++c) {
        char byte=(char)c,inside[]={'1',(char)c,'2'};bool digit=c>=48 && c<=57;
        json_numbers_case(program,entries[1],&byte,1,(JsonNumberExpected){digit,digit,digit?(int64_t)c-48:0});++cases;
        bool token=digit || c==46 || c==69 || c==101;
        json_numbers_case(program,entries[1],inside,sizeof(inside),(JsonNumberExpected){token,digit,digit?102+10*((int64_t)c-48):0});++cases;
    }
    const char nul[]={'1',0,'2'};json_numbers_case(program,entries[1],nul,sizeof(nul),(JsonNumberExpected){false,false,0});++cases;
    if(json_numbers_faults) {
        JsonNumberTrial trial={NULL,0,{false,false,0},true};json_numbers_full_faults(program,entries[0],&trial);
        printf("Ordinary JSON number full runtime FI: cases=%zu actual allocation sites=%zu, every ordinal and same Program retry\n",
            json_numbers_fault_cases,json_numbers_fault_sites);
    }
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entries[0],NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==42);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    printf("Ordinary JSON numbers: %zu independent grammar/range cases, actual signed extrema, full ASCII positions, embedded NUL and comptime42\n",cases);
}
#endif // XIR_JSON_NUMBERS_RUNTIME_H
