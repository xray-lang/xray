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
static bool json_strings_faults;
static size_t json_strings_fault_cases,json_strings_fault_sites;
typedef struct JsonStringsTrial {
    const char *input,*expected;
    size_t input_length,expected_length;
    bool constant;
} JsonStringsTrial;
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
static void json_strings_trial(XrXirProgram *program,uint32_t entry,const JsonStringsTrial *trial) {
    XrXirDomain *domain=NULL;XrXirInstance *instance=NULL;XrXirValue argument={0},result={0};
    XrXirCallStatus status=XR_XIR_CALL_READY;bool driven=false;
    if(!trial->constant) {
        XrXirValueStatus made=xr_xir_domain_new(65536,&domain);
        if(made!=XR_XIR_VALUE_OK) {CHECK(made==XR_XIR_VALUE_OOM && !domain);status=XR_XIR_CALL_OOM;goto done;}
        made=xr_xir_string_new(domain,trial->input,trial->input_length,&argument);
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
        else json_strings_expected(&result,trial->expected,trial->expected_length);
    }
    xr_xir_value_drop(&result);
}
static void json_strings_full_faults(XrXirProgram *program,uint32_t entry,const JsonStringsTrial *trial) {
    size_t held=runtime_live,bytes=runtime_bytes;CHECK(runtime_fail_at==SIZE_MAX);runtime_attempts=0;
    json_strings_trial(program,entry,trial);size_t sites=runtime_attempts;CHECK(sites && runtime_live==held && runtime_bytes==bytes);
    for(size_t site=0;site<sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;json_strings_trial(program,entry,trial);
        CHECK(runtime_attempts>site && runtime_live==held && runtime_bytes==bytes);
        runtime_fail_at=SIZE_MAX;json_strings_trial(program,entry,trial);CHECK(runtime_live==held && runtime_bytes==bytes);
    }
    ++json_strings_fault_cases;json_strings_fault_sites+=sites;
}
static void json_strings_case(XrXirProgram *program,uint32_t entry,const char *input,
    size_t size,const char *expected,size_t length) {
    if(json_strings_faults) {
        JsonStringsTrial trial={input,expected,size,length,false};json_strings_full_faults(program,entry,&trial);return;
    }
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
    if(json_strings_faults) {
        JsonStringsTrial trial={NULL,NULL,0,0,true};json_strings_full_faults(program,entries[0],&trial);
        printf("Ordinary JSON token full runtime FI: cases=%zu actual allocation sites=%zu, every ordinal and same Program retry\n",
            json_strings_fault_cases,json_strings_fault_sites);
    }
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
