/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_generator_iterable_cases.h - One actual factory call and lexical producer close
 */
#ifndef XIR_GENERATOR_ITERABLE_CASES_H
#define XIR_GENERATOR_ITERABLE_CASES_H
typedef struct IterableLog {unsigned cleanup;} IterableLog;
static XrXirOutputStatus iterable_output(void *owner,const XrXirOutputGroup *group) {
    IterableLog *log=owner;
    CHECK(group && group->line && group->stream==XR_XIR_STDOUT && group->count==1);
    CHECK(group->values[0].type==XR_XIR_I64 && group->values[0].payload==401);++log->cleanup;
    return XR_XIR_OUTPUT_OK;
}
static XrXirValue iterable_result(XrXirInstance *instance) {
    CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void iterable_timer(XrXirInstance *instance,uint64_t delay) {
    XrXirInstanceResult pending=xr_xir_instance_poll_bounded(instance,1000000);XrXirWaitRequest wait={0};
    CHECK(pending.outcome.status==XR_XIR_CALL_SUSPENDED && pending.outcome.wake && pending.epoch);
    CHECK(xr_xir_instance_wait_request(instance,pending.epoch,pending.outcome.wake,&wait)==XR_XIR_CALL_READY);
    CHECK(wait.kind==XR_XIR_WAIT_TIMER_MS && wait.after_ms==delay);
    CHECK(xr_xir_instance_resume(instance,pending.epoch,pending.outcome.wake)==XR_XIR_CALL_READY);
}
static void generator_iterable_cases(XrXirProgram *program,const uint32_t ids[10]) {
    for(unsigned mode=0;mode<9;++mode) {
        IterableLog log={0};XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,iterable_output,&log};
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,ids[mode<2 ? mode : mode+1],NULL,0)==XR_XIR_CALL_READY);
        if(mode==4) iterable_timer(instance,3);
        if(mode!=5) iterable_timer(instance,5);
        if(mode==0 || mode==2 || mode==4) iterable_timer(instance,7);
        XrXirValue value=iterable_result(instance);CHECK(value.type==XR_XIR_I64 && value.payload==(mode==0 ? 13 : mode==1 ? 6 : mode==2 || mode==5 ? 17 : mode==3 ? 19 : mode==4 ? 21 : mode==6 ? 9 : mode==7 ? 7 : 71));
        xr_xir_value_drop(&value);CHECK(log.cleanup==(mode==5 ? 0u : 1u));
        CHECK(xr_xir_instance_start(instance,ids[2],NULL,0)==XR_XIR_CALL_READY);
        value=iterable_result(instance);CHECK(value.type==XR_XIR_I64 && value.payload==(mode<2 ? 111 : mode==2 || mode==6 ? 1 : mode==5 ? 0 : 11));xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && log.cleanup==(mode==5 ? 0u : 1u));
    }
    puts("Custom iterator:9 factory, generator method, generic witness, Error and wait cases PASS");
}
#endif
