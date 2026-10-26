/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * string_bytes_cases.h - Fixed naming-rule oracles across execution providers
 *
 * KEY CONCEPT:
 *   Text contains real UTF-8 and embedded NUL bytes, independent of storage.
 */
#ifndef STRING_BYTES_CASES_H
#define STRING_BYTES_CASES_H
#include "base/xresource_limits.h"
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"

typedef struct StringBytesCase {
    bool c_string;
    const char *text;
    size_t length;
    int64_t maximum;
    bool allow_empty, expected;
} StringBytesCase;

typedef struct StringBytesFunctions {
    uint32_t entry,printable,c_string,discard,scalar,ref_panic,var_bytes;
} StringBytesFunctions;

#define STRING_BYTES_CASE(kind, text, maximum, allow, expected) \
    {kind,text,sizeof(text)-1,maximum,allow,expected}
static const StringBytesCase string_bytes_cases[] = {
    STRING_BYTES_CASE(false,"",8,false,false),
    STRING_BYTES_CASE(false," A~",3,false,true),
    STRING_BYTES_CASE(false,"\x1f",8,false,false),
    STRING_BYTES_CASE(false,"\xc3\xa9",8,false,false),
    STRING_BYTES_CASE(false,"four",3,false,false),
    STRING_BYTES_CASE(true,"",0,true,true),
    STRING_BYTES_CASE(true,"",0,false,false),
    STRING_BYTES_CASE(true,"a\0b",8,true,false),
    STRING_BYTES_CASE(true,"\xc3\xa9",2,false,true),
    STRING_BYTES_CASE(true,"abc",2,true,false),
};
#undef STRING_BYTES_CASE

typedef struct StringBytesOutput {
    char bytes[80];
    size_t size;
    XrXirOutputSink sink;
} StringBytesOutput;

static XrXirOutputStatus string_bytes_output(void *context,XrXirOutputStream stream,
    const char *bytes,size_t length) {
    StringBytesOutput *output=context;
    CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->size);
    memcpy(output->bytes+output->size,bytes,length);
    output->size+=length;
    return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus string_bytes_output_group(void *context,const XrXirOutputGroup *group) {
    return xr_xir_output_render(&((StringBytesOutput *)context)->sink,group);
}
static XrXirValue string_bytes_execute(XrXirInstance *instance,uint32_t function,
    const XrXirValue *arguments,uint32_t count) {
    CHECK(xr_xir_instance_start(instance,function,arguments,count)==XR_XIR_CALL_READY);
    XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,XR_RESOURCE_DEFAULT_LIMIT);
    if (polled.outcome.status!=XR_XIR_CALL_RETURNED)
        fprintf(stderr,"string bytes function %u outcome=%u\n",function,(unsigned)polled.outcome.status);
    CHECK(polled.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_value_valid(&result));
    return result;
}
static void string_bytes_run(XrXirProgram *program,const StringBytesFunctions *functions) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    StringBytesOutput output={0};
    output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,string_bytes_output,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,string_bytes_output_group,&output};
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue result=string_bytes_execute(instance,functions->entry,NULL,0);
    xr_xir_value_drop(&result);
    XrXirDomain *domain=NULL;
    CHECK(xr_xir_domain_new(XR_RESOURCE_DEFAULT_LIMIT,&domain)==XR_XIR_VALUE_OK);
    for (size_t i=0;i<sizeof(string_bytes_cases)/sizeof(*string_bytes_cases);++i) {
        const StringBytesCase *test=string_bytes_cases+i;
        XrXirValue arguments[3]={{0},{XR_XIR_I64,0,(uint64_t)test->maximum},
            {XR_XIR_BOOL,0,test->allow_empty}};
        CHECK(xr_xir_string_new(domain,test->text,test->length,arguments)==XR_XIR_VALUE_OK);
        result=string_bytes_execute(instance,test->c_string?functions->c_string:functions->printable,
            arguments,test->c_string?3u:2u);
        CHECK(result.type==XR_XIR_BOOL && !result.reserved && result.payload==test->expected);
        xr_xir_value_drop(&result);
        result=string_bytes_execute(instance,functions->discard,arguments,1);
        CHECK(result.type==XR_XIR_UNIT);
        xr_xir_value_drop(&result);
        result=string_bytes_execute(instance,functions->scalar,arguments,1);
        CHECK(result.type==XR_XIR_I64 && result.payload==17);
        xr_xir_value_drop(&result);
        result=string_bytes_execute(instance,functions->ref_panic,arguments,1);
        int64_t expected_head=test->length?2*(int64_t)(uint8_t)test->text[0]:0;
        CHECK(result.type==XR_XIR_I64 && result.payload==expected_head);
        xr_xir_value_drop(&result);
        int64_t expected_sum=(int64_t)test->length;
        for (size_t b=0;b<test->length;++b) expected_sum+=(uint8_t)test->text[b];
        result=string_bytes_execute(instance,functions->var_bytes,arguments,1);
        CHECK(result.type==XR_XIR_I64 && (int64_t)result.payload==expected_sum);
        xr_xir_value_drop(&result);
        xr_xir_value_drop(arguments);
    }
    const char expected[]="true false false\ntrue false false\n";
    CHECK(output.size==sizeof(expected)-1 && !memcmp(output.bytes,expected,sizeof(expected)-1));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_domain_drop(domain);
}
#endif // STRING_BYTES_CASES_H
