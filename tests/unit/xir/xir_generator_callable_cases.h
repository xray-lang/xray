/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_callable_cases.h - Callable and witness targets create parked producers
 */
#ifndef XIR_GENERATOR_CALLABLE_CASES_H
#define XIR_GENERATOR_CALLABLE_CASES_H
typedef struct CallableArgumentLog {unsigned cleanup;} CallableArgumentLog;
static XrXirOutputStatus callable_argument_output(void *owner,const XrXirOutputGroup *group) {
    CallableArgumentLog *log=owner;
    CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==401);++log->cleanup;
    return XR_XIR_OUTPUT_OK;
}
static XrXirValue callable_argument_result(XrXirInstance *instance) {
    CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void callable_argument_timer(XrXirInstance *instance,uint64_t delay) {
    XrXirInstanceResult pending=xr_xir_instance_poll_bounded(instance,1000000);XrXirWaitRequest wait={0};
    if(pending.outcome.status!=XR_XIR_CALL_SUSPENDED) fprintf(stderr,"Callable timer expected%llu status%u\n",(unsigned long long)delay,pending.outcome.status);
    CHECK(pending.outcome.status==XR_XIR_CALL_SUSPENDED && pending.outcome.wake && pending.epoch);
    CHECK(xr_xir_instance_wait_request(instance,pending.epoch,pending.outcome.wake,&wait)==XR_XIR_CALL_READY);
    CHECK(wait.kind==XR_XIR_WAIT_TIMER_MS && wait.after_ms==delay);
    CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_READY);
}
static void generator_callable_cases(XrXirProgram *program,const uint32_t ids[10]) {
    static const int64_t expected[]={7,7,7,7};
    for(unsigned mode=0;mode<4;++mode) {
        CallableArgumentLog log={0};XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,callable_argument_output,&log};
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,ids[mode],NULL,0)==XR_XIR_CALL_READY);
        callable_argument_timer(instance,5);
        XrXirValue value=callable_argument_result(instance);
        CHECK(value.type==XR_XIR_I64 && value.payload==expected[mode]);xr_xir_value_drop(&value);CHECK(log.cleanup==1u);
        CHECK(xr_xir_instance_start(instance,ids[4],NULL,0)==XR_XIR_CALL_READY);
        value=callable_argument_result(instance);CHECK(value.type==XR_XIR_I64 && value.payload==1);xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.cleanup==1u);
    }
    puts("Generator callable arguments:4 witness and direct cases PASS");
}
#endif
