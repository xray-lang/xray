/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_consumer_cases.h - Typed pull, cleanup and root replacement witnesses
 */
#ifndef XIR_GENERATOR_CONSUMER_CASES_H
#define XIR_GENERATOR_CONSUMER_CASES_H
#include "xir/xxir_generator_internal.h"
#include <string.h>
typedef struct GeneratorConsumerLog {unsigned count;int64_t expected;} GeneratorConsumerLog;
static XrXirOutputStatus consumer_output(void *opaque,const XrXirOutputGroup *group) {
    GeneratorConsumerLog *log=opaque;
    CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==log->expected);
    ++log->count;return XR_XIR_OUTPUT_OK;
}
static XrXirInstance *consumer_instance(XrXirProgram *program,GeneratorConsumerLog *log) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,consumer_output,log};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);return instance;
}
static XrXirValue consumer_root(XrXirInstance *instance,uint32_t entry,const XrXirValue *args,uint32_t count) {
    CHECK(xr_xir_instance_start(instance,entry,args,count)==XR_XIR_CALL_READY);
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
    if(result.outcome.status!=XR_XIR_CALL_RETURNED) fprintf(stderr,"Consumer entry%u status%u\n",entry,result.outcome.status);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void generator_consumer_cases(XrXirProgram *program,const uint32_t ids[8]) {
    XrXirValue kept={0};
    for(unsigned n=0;n<5;++n) {
        GeneratorConsumerLog log={0,n==0 || n==4 ? 301 : n==1 ? 302 : n==2 ? 303 : 304};
        XrXirInstance *instance=consumer_instance(program,&log);XrXirValue value={0};
        if(n==4) {
            CHECK(xr_xir_instance_start(instance,ids[n],NULL,0)==XR_XIR_CALL_READY);
            XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
            CHECK(result.outcome.status==XR_XIR_CALL_RUNTIME_PANIC && !result.outcome.wake);
            CHECK(result.outcome.panic.detail.code==432 && !result.outcome.panic.detail.reserved &&
                !result.outcome.panic.detail.index && !result.outcome.panic.detail.length);
            char message[96];CHECK(xr_xir_panic_message_format(result.outcome.panic.detail,message,sizeof(message))==21 &&
                !strcmp(message,"iterator is exhausted"));
        } else {
            value=consumer_root(instance,ids[n],NULL,0);
            if(n==2) {CHECK(xr_xir_value_copy(&value,&kept)==XR_XIR_VALUE_OK);}
            else CHECK(value.type==XR_XIR_I64 && value.payload==(n==0 ? 33 : n==1 ? 42 : 37));
        }
        xr_xir_value_drop(&value);CHECK(log.count==1);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);CHECK(log.count==1);
    }
    GeneratorConsumerLog log={0,301};XrXirInstance *instance=consumer_instance(program,&log);
    XrXirValue iterator=consumer_root(instance,ids[5],NULL,0);
    XirGenerator *generator=(XirGenerator *)(uintptr_t)iterator.payload;
    CHECK(generator && !generator->accounting.polls && !log.count);
    for(unsigned n=0;n<2;++n) {
        uint64_t polls=generator->accounting.polls;XrXirValue more=consumer_root(instance,ids[6],&iterator,1);
        CHECK(more.type==XR_XIR_BOOL && more.payload==1 && !log.count);
        CHECK(!n || generator->accounting.polls==polls);xr_xir_value_drop(&more);
    }
    XrXirValue first=consumer_root(instance,ids[7],&iterator,1);CHECK(first.type==XR_XIR_I64 && first.payload==11);
    xr_xir_value_drop(&first);XrXirValue second=consumer_root(instance,ids[5],NULL,0);
    XirGenerator *other=(XirGenerator *)(uintptr_t)second.payload;
    CHECK(other!=generator && other->call!=generator->call && !other->accounting.polls);
    first=consumer_root(instance,ids[7],&second,1);CHECK(first.type==XR_XIR_I64 && first.payload==11);xr_xir_value_drop(&first);
    GeneratorConsumerLog denied_log={0,301};XrXirInstance *foreign=consumer_instance(program,&denied_log);
    CHECK(xr_xir_instance_start(foreign,ids[6],&iterator,1)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(foreign)==XR_XIR_CALL_READY && !denied_log.count);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.count==2);
    CHECK(xr_xir_value_valid(&iterator) && xr_xir_value_valid(&second));
    CHECK(!generator->call && !other->call && !generator->consumer && !other->consumer);
    CHECK(generator->outcome.status==XR_XIR_CALL_CANCELLED && other->outcome.status==XR_XIR_CALL_CANCELLED);
    xr_xir_value_drop(&iterator);xr_xir_value_drop(&second);xr_xir_compile_program_drop(program);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&kept,&bytes,&length));
    CHECK(length==5 && !memcmp(bytes,"owned",5));xr_xir_value_drop(&kept);
}
#endif
