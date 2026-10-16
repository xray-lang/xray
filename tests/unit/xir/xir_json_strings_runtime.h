/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_strings_runtime.h - Independent ordinary JSON string-token cases
 */
#ifndef XIR_JSON_STRINGS_RUNTIME_H
#define XIR_JSON_STRINGS_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_nullable.h"
#include "xir_json_quote_oracles.h"
static XrXirValue json_strings_call(XrXirProgram *program,uint32_t entry,const char *bytes,size_t length) {
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
static void json_strings_expected(const XrXirValue *value,const char *bytes,size_t length) {
    bool some=false;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(value,&some,&payload) && some==(bytes!=NULL));
    if(some) {
        const char *text=NULL;size_t size=0;
        CHECK(payload && payload->type==XR_XIR_STRING && xr_xir_string_view(payload,&text,&size) &&
            size==length && !memcmp(text,bytes,length));
    } else CHECK(!payload);
}
static void json_strings_case(XrXirProgram *program,uint32_t entry,const char *input,
    size_t size,const char *expected,size_t length) {
    XrXirValue value=json_strings_call(program,entry,input,size);
    json_strings_expected(&value,expected,length);xr_xir_value_drop(&value);
}
static void json_strings_oracles(XrXirProgram *program,const uint32_t entries[4]) {
    json_quote_oracles(program,entries[2]);
    for(unsigned c=0;c<128;++c) {
        char byte=(char)c,input[]={'"',(char)c,'"'},escaped[16];
        bool valid=c>=32 && c!=34 && c!=92;
        json_strings_case(program,entries[1],input,sizeof(input),valid?&byte:NULL,valid?1:0);
        int n=snprintf(escaped,sizeof(escaped),"\"\\u%04x\"",c);CHECK(n==8);
        json_strings_case(program,entries[1],escaped,(size_t)n,&byte,1);
    }
    const struct {const char *input,*expected;} accepted[]={
        {"\"\"",""},{"\"null\"","null"},{"\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"","\"\\/\b\f\n\r\t"},
        {"\"é汉😀\"","é汉😀"},{"\"\\u0080\"","\xc2\x80"},{"\"\\u07ff\"","\xdf\xbf"},
        {"\"\\u0800\"","\xe0\xa0\x80"},{"\"\\uD7FF\"","\xed\x9f\xbf"},
        {"\"\\uE000\"","\xee\x80\x80"},{"\"\\uFFFF\"","\xef\xbf\xbf"},
        {"\"\\ud800\\udc00\"","\xf0\x90\x80\x80"},{"\"\\uDBFF\\uDFFF\"","\xf4\x8f\xbf\xbf"},
        {"\"\\u6c49\\ud83d\\ude00\"","汉😀"},{"\"\xe2\x80\xa8\xe2\x80\xa9\"","\xe2\x80\xa8\xe2\x80\xa9"}
    };
    for(unsigned i=0;i<sizeof(accepted)/sizeof(*accepted);++i)
        json_strings_case(program,entries[1],accepted[i].input,strlen(accepted[i].input),accepted[i].expected,strlen(accepted[i].expected));
    const char *rejected[]={"","null","true","0","\"","\"abc","abc\""," \"a\"","\"a\" ",
        "\"a\"x\"","\"\\\"","\"\\x00\"","\"\\0\"","\"\\v\"","\"\\U0000\"","\"\\u\"",
        "\"\\u123\"","\"\\u12g4\"","\"\\ud800\"","\"\\udfff\"","\"\\udc00\\ud800\"",
        "\"\\ud800\\u0041\"","\"\\ud800\\ud800\"","\"\\ud800\\udc0\"","\"\\ud800x\\udc00\""};
    for(unsigned i=0;i<sizeof(rejected)/sizeof(*rejected);++i)
        json_strings_case(program,entries[1],rejected[i],strlen(rejected[i]),NULL,0);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entries[0],NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==42);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    puts("Ordinary JSON strings:256 ASCII decode cases,14 UTF8/escape positives,25 malformed refusals,139 quote cases, comptime42 and input Domain death");
}
#endif // XIR_JSON_STRINGS_RUNTIME_H
