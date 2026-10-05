/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_resize_limits.h - Real range, retain and domain boundaries
 *
 * KEY CONCEPT:
 *   Both argument side effects precede the negative-length diagnostic.
 */
#ifndef XIR_ARRAY_RESIZE_LIMITS_H
#define XIR_ARRAY_RESIZE_LIMITS_H
static void resize_runtime_limits(ResizeCompile *run) {
    for(unsigned n=0;n<2;++n) {
        XrXirInstance *instance=resize_open(run);
        XrXirValue alias=resize_run(instance,resize_find(run->module,"saved"));
        CHECK(xr_xir_instance_start(instance,resize_find(run->module,n?"maximum":"negative"),NULL,0)==XR_XIR_CALL_READY);
        XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        CHECK(status==(n?XR_XIR_CALL_LIMIT:XR_XIR_CALL_NUMERIC_RANGE));
        XrXirValue absent={0};CHECK(xr_xir_instance_take_result(instance,&absent)==XR_XIR_CALL_BAD_STATE&&!absent.type&&!absent.payload);
        resize_state(instance,run,false,false);
        if(!n){XrXirValue trace=resize_run(instance,resize_find(run->module,"currentTrace"));CHECK(trace.payload==12);xr_xir_value_drop(&trace);}
        static const int64_t old[]={1,INT64_MIN,3};resize_array(&alias,old,3,true,1);xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    }
    for(unsigned shrink=0;shrink<2;++shrink)for(unsigned mode=0;mode<4;++mode) {
        XrXirInstance *instance=resize_open(run);XrXirValue alias=resize_run(instance,resize_find(run->module,"saved"));
        XirObject *backing=object_pointer(&alias);CHECK(backing);
        _Atomic uint32_t *references=mode==1?&backing->references:mode==2?&instance->domain->references:&run->program->arena->references;
        uint32_t original=atomic_load(references);uint64_t limit=instance->domain->limit;
        if(!mode)instance->domain->limit=instance->domain->stats.live_bytes;else atomic_store(references,UINT32_MAX);
        XrXirCallStatus status=xr_xir_instance_start(instance,resize_find(run->module,shrink?"shrink":"grow"),NULL,0);
        if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        instance->domain->limit=limit;if(mode)atomic_store(references,original);CHECK(status==XR_XIR_CALL_LIMIT);
        resize_state(instance,run,shrink!=0,false);static const int64_t old[]={1,INT64_MIN,3};
        resize_array(&alias,old,3,true,1);xr_xir_value_drop(&alias);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    }
    puts("resize negativeE0422 arguments12 maximumLIMIT eight domain/retain failures root+alias unchanged physical0");
}
#endif
