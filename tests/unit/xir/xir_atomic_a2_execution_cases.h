/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_atomic_a2_execution_cases.h - Independent Atomic execution and ownership qualification
 *
 * KEY CONCEPT:
 *   Each backend checks fixed expectations after the ordinary source pipeline.
 */
#ifndef XIR_ATOMIC_A2_EXECUTION_CASES_H
#define XIR_ATOMIC_A2_EXECUTION_CASES_H
#include "xir/xxir_output.h"
typedef struct AtomicOutput {char bytes[512];size_t length;unsigned groups;XrXirOutputSink sink;} AtomicOutput;
static XrXirOutputStatus atomic_output_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t size) {
    AtomicOutput *out=context;CHECK(stream==XR_XIR_STDOUT && size<=sizeof(out->bytes)-out->length);
    memcpy(out->bytes+out->length,bytes,size);out->length+=size;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus atomic_output_group(void *context,const XrXirOutputGroup *group) {
    AtomicOutput *out=context;CHECK(group && group->count==21);
    const uint32_t expected[]={XR_XIR_STRING,XR_XIR_I64,XR_XIR_I64,XR_XIR_I64,XR_XIR_BOOL,
        XR_XIR_I64,XR_XIR_BOOL,XR_XIR_I64,XR_XIR_STRING,XR_XIR_F64,XR_XIR_F64,XR_XIR_F64,
        XR_XIR_F64,XR_XIR_BOOL,XR_XIR_STRING,XR_XIR_BOOL,XR_XIR_BOOL,XR_XIR_BOOL,
        XR_XIR_BOOL,XR_XIR_BOOL,XR_XIR_STRING};
    for (uint32_t i=0;i<21;++i) CHECK(group->values[i].type==expected[i]);
    ++out->groups;return xr_xir_output_render(&out->sink,group);
}
static void atomic_run_once(XrXirInstance *instance,uint32_t entry,AtomicOutput *out,bool second) {
    out->length=0;out->groups=0;
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult poll={0};unsigned attempts=0;
    do {CHECK(++attempts<4096);poll=xr_xir_instance_poll_bounded(instance,1);} while(poll.outcome.status==XR_XIR_CALL_READY);
    if (poll.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Atomic execution status %u polls %u\n",poll.outcome.status,attempts);
    CHECK(poll.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==(second?60:40));xr_xir_value_drop(&result);
    const char first[]="owned\xe4\xb8\xad 40 44 50 true 60 false 60 60 1.5 2.0 4.5 3.5 true 4.5 false true true true false false\n";
    const char again[]="owned\xe4\xb8\xad 60 64 50 true 60 false 60 60 1.5 2.0 4.5 3.5 true 4.5 false true true true false false\n";
    const char *expected=second?again:first;size_t size=strlen(expected);
    if (out->length!=size || memcmp(out->bytes,expected,size)) fprintf(stderr,"Atomic output %.*s",(int)out->length,out->bytes);
    CHECK(out->groups==1 && out->length==size && !memcmp(out->bytes,expected,size));
}
static void atomic_execute_pair(XrXirProgram *program,uint32_t entry) {
    AtomicOutput output[2]={{0},{0}};XrXirInstance *instances[2]={NULL,NULL};
    for(unsigned i=0;i<2;++i) {
        output[i].sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,atomic_output_bytes,&output[i],512};
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,atomic_output_group,&output[i]};
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    atomic_run_once(instances[0],entry,&output[0],false);atomic_run_once(instances[1],entry,&output[1],false);
    atomic_run_once(instances[0],entry,&output[0],true);
    CHECK(xr_xir_instance_free(instances[1])==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instances[0])==XR_XIR_CALL_READY);
}
#endif
