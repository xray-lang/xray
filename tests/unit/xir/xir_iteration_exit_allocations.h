/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_iteration_exit_allocations.h - Faults after real loop suspension
 *
 * KEY CONCEPT:
 *   A live snapshot and registered defer precede each injected exit fault.
 */
#ifndef XIR_ITERATION_EXIT_ALLOCATIONS_H
#define XIR_ITERATION_EXIT_ALLOCATIONS_H
static void iteration_exit_allocations(XrXirProgram *program) {
    size_t baseline=runtime_live, baseline_bytes=runtime_bytes;
    for (unsigned mode=0;mode<3;++mode) {
        size_t sites=0;
        for (size_t pass=0;pass<=(sites ? sites : 1);++pass) {
            runtime_fail_at=SIZE_MAX;
            XrXirInstance *instance=NULL;
            XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            IterationTrace trace={0};
            config.output=(XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, iteration_trace, &trace};
            C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
            C(xr_xir_instance_start(instance,5,NULL,0)==XR_XIR_CALL_READY);
            XrXirInstanceResult paused=xr_xir_instance_poll(instance);
            C(paused.outcome.status==XR_XIR_CALL_SUSPENDED && trace.count==0);
            if (mode==1) {
                C(xr_xir_instance_resume(instance,paused.epoch,paused.outcome.wake)==XR_XIR_CALL_READY);
                paused=xr_xir_instance_poll(instance);
                C(paused.outcome.status==XR_XIR_CALL_SUSPENDED && trace.count==1);
            }
            unsigned prefix=trace.count;
            runtime_attempts=0;runtime_fail_at=pass ? pass-1 : SIZE_MAX;
            XrXirCallStatus status;
            if (mode<2) status=xr_xir_instance_stop(instance);
            else {
                unsigned resumes=0;
                do {
                    C(++resumes<=3);
                    status=xr_xir_instance_resume(instance,paused.epoch,paused.outcome.wake);
                    if (status!=XR_XIR_CALL_READY) break;
                    paused=xr_xir_instance_poll(instance);status=paused.outcome.status;
                } while (status==XR_XIR_CALL_SUSPENDED);
            }
            if (!pass || !sites) {
                if (!pass) sites=runtime_attempts;
                else C(runtime_attempts==0);
                C(status==(mode<2 ? XR_XIR_CALL_READY : XR_XIR_CALL_RETURNED));
                C(trace.count==(mode<2 ? prefix+1 : 3));
            } else {
                C(status==XR_XIR_CALL_OOM);
                C(trace.count>=prefix && trace.count<=(mode<2 ? prefix+1 : 3));
            }
            XrXirValue value={0};
            if (status==XR_XIR_CALL_RETURNED) {
                C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
                C(value.type==XR_XIR_I64 && value.payload==41);xr_xir_value_drop(&value);
            }
            C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE);
            C(value.type==XR_XIR_UNIT && !value.payload && !value.reserved);
            unsigned before_free=trace.count;
            C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
            C(trace.count==before_free);
            if (!sites) C(runtime_attempts==0);
            C(runtime_live==baseline && runtime_bytes==baseline_bytes);
        }
        runtime_fail_at=SIZE_MAX;
        printf("iteration exit mode%u: %zu post-suspension OOM sites, no result republication, physical baseline restored\n",mode,sites);
    }
}
#endif
