/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_timed_runtime_faults.h - Full runtime fault ordinals for timed Source
 */
#ifndef XIR_CHANNEL_TIMED_RUNTIME_FAULTS_H
#define XIR_CHANNEL_TIMED_RUNTIME_FAULTS_H
static size_t timed_source_fault_once(XrXirProgram *program,uint32_t entry,size_t ordinal) {
    size_t live=runtime_live,bytes=runtime_bytes,begin=runtime_attempts;TimedSourceOutput witness={0};XrXirInstanceConfig config={0};
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.time=(XrXirTimeProvider){XR_XIR_CALL_ABI_VERSION,0,timed_source_clock,timed_source_offset,&witness};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,timed_source_output,&witness};
    runtime_fail_at=ordinal==SIZE_MAX ? SIZE_MAX : begin+ordinal;
    XrXirInstance *instance=NULL;XrXirCallStatus release=XR_XIR_CALL_READY,status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_start(instance,entry,NULL,0);
        if (status==XR_XIR_CALL_READY) {
            unsigned polls=0;XrXirInstanceResult result={0};
            do {
                CHECK(++polls<4096);result=xr_xir_instance_poll_bounded(instance,64);
                if (result.outcome.status==XR_XIR_CALL_SUSPENDED) {
                    XrXirWaitRequest wait={0};CHECK(xr_xir_instance_wait_request(instance,result.epoch,result.outcome.wake,&wait)==XR_XIR_CALL_READY && wait.kind==XR_XIR_WAIT_CHANNEL);
                    xr_time_sleep_ns(1000000);
                }
            } while(result.outcome.status==XR_XIR_CALL_READY || result.outcome.status==XR_XIR_CALL_SUSPENDED);
            status=result.outcome.status;CHECK(xr_xir_call_result_valid(&result.outcome));
            if (status==XR_XIR_CALL_RETURNED) {
                XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==status && value.type==XR_XIR_I64 && value.payload==99);
                xr_xir_value_drop(&value);
            } else release=status;
        }
    }
    size_t sites=runtime_attempts-begin;runtime_fail_at=SIZE_MAX;
    if (status!=(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM))
        fprintf(stderr,"Timed Source runtime ordinal=%zu sites=%zu status=%u groups=%u\n",ordinal,sites,status,witness.groups);
    CHECK(status==(ordinal==SIZE_MAX ? XR_XIR_CALL_RETURNED : XR_XIR_CALL_OOM));
    if (ordinal==SIZE_MAX) CHECK(witness.groups==9);
    size_t attempts=runtime_attempts;CHECK(xr_xir_instance_free(instance)==release && runtime_attempts==attempts);
    CHECK(runtime_live==live && runtime_bytes==bytes);return sites;
}
static void timed_source_runtime_faults(XrXirProgram *program,uint32_t entry) {
    size_t sites=timed_source_fault_once(program,entry,SIZE_MAX);CHECK(sites && sites<4096);
    for (size_t ordinal=0;ordinal<sites;++ordinal) {
        CHECK(timed_source_fault_once(program,entry,ordinal)>ordinal);
        CHECK(timed_source_fault_once(program,entry,SIZE_MAX)==sites);
    }
    xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    printf("Timed actual Source runtime complete allocator ordinals=%zu; nine typed output prefix and same-process recovery physical0\n",sites);
}
#endif // XIR_CHANNEL_TIMED_RUNTIME_FAULTS_H
