/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_method_factory_cases.h - Callable and witness targets create parked producers
 */
#ifndef XIR_GENERATOR_METHOD_FACTORY_CASES_H
#define XIR_GENERATOR_METHOD_FACTORY_CASES_H
typedef struct MethodFactoryLog {unsigned cleanup;} MethodFactoryLog;
static XrXirOutputStatus method_factory_output(void *owner,const XrXirOutputGroup *group) {
    MethodFactoryLog *log=owner;
    CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==401);++log->cleanup;
    return XR_XIR_OUTPUT_OK;
}
static XrXirValue method_factory_result(XrXirInstance *instance) {
    CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void method_factory_timer(XrXirInstance *instance,uint64_t delay) {
    XrXirInstanceResult pending=xr_xir_instance_poll_bounded(instance,1000000);XrXirWaitRequest wait={0};
    if(pending.outcome.status!=XR_XIR_CALL_SUSPENDED) fprintf(stderr,"Factory timer expected%llu status%u\n",(unsigned long long)delay,pending.outcome.status);
    CHECK(pending.outcome.status==XR_XIR_CALL_SUSPENDED && pending.outcome.wake && pending.epoch);
    CHECK(xr_xir_instance_wait_request(instance,pending.epoch,pending.outcome.wake,&wait)==XR_XIR_CALL_READY);
    CHECK(wait.kind==XR_XIR_WAIT_TIMER_MS && wait.after_ms==delay);
    CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_READY);
}
static void generator_method_factory_cases(XrXirProgram *program,const uint32_t ids[10]) {
    static const int64_t expected[]={12,14,16,9,10,10,11,12};
    for(unsigned mode=0;mode<8;++mode) {
        MethodFactoryLog log={0};XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,method_factory_output,&log};
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,ids[mode],NULL,0)==XR_XIR_CALL_READY);
        if(mode!=7) method_factory_timer(instance,5);if(mode<3) method_factory_timer(instance,7);
        XrXirValue value=method_factory_result(instance);
        CHECK(value.type==XR_XIR_I64 && value.payload==expected[mode]);xr_xir_value_drop(&value);CHECK(log.cleanup==(mode==7 ? 0u : 1u));
        CHECK(xr_xir_instance_start(instance,ids[9],NULL,0)==XR_XIR_CALL_READY);
        value=method_factory_result(instance);CHECK(value.type==XR_XIR_I64 && value.payload==(mode<6 ? 1 : 0));xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.cleanup==(mode==7 ? 0u : 1u));
    }
    for(unsigned replace=0;replace<2;++replace) {
        MethodFactoryLog log={0};XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,method_factory_output,&log};
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,ids[8],NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1000000);
        CHECK(result.outcome.status==XR_XIR_CALL_BAD_ARGUMENT && !result.outcome.wake && !log.cleanup);
        CHECK(xr_xir_call_result_valid(&result.outcome) && !result.outcome.value.type);
        if(replace) {
            CHECK(xr_xir_instance_start(instance,ids[9],NULL,0)==XR_XIR_CALL_READY);
            XrXirValue value=method_factory_result(instance);CHECK(value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);
        }
        CHECK(xr_xir_instance_free(instance)==(replace ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT) && !log.cleanup);
    }
    puts("Generator method factories:8 legal controls +2 exact worker refusals PASS");
}
#endif
