/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_math.inc.c - Authenticated scalar math dispatch and system entropy
 *
 * KEY CONCEPT:
 *   Pure evaluation is shared with CTFE; random results use the existing system entropy provider.
 */
#include "xxir_math_eval_internal.h"
#include "../shared/xr_math_core.h"
#include "../os/os_random.h"
static XrXirRunStatus instance_math_call(uint32_t id,const XrXirValue *arguments,
    uint32_t count,XrXirValue *output) {
    if(!xir_math_native_arguments(id,arguments,count,output))return XR_XIR_RUN_BAD_ARTIFACT;
    if(!xir_math_native_random(id))return xir_math_native_evaluate(id,arguments,count,output);
    if(id==XR_NATIVE_CALLABLE_MATH_RANDOM_INT) {
        int64_t lo,hi;memcpy(&lo,&arguments[0].payload,sizeof(lo));memcpy(&hi,&arguments[1].payload,sizeof(hi));
        int64_t result=xr_math_core_random_i64(xr_random_bytes_callback,NULL,lo,hi);
        *output=(XrXirValue){XR_XIR_I64,0,result};
    } else {
        double result=xr_math_core_random_f64(xr_random_bytes_callback,NULL);uint64_t bits;
        memcpy(&bits,&result,sizeof(bits));*output=(XrXirValue){XR_XIR_F64,0,0};
        memcpy(&output->payload,&bits,sizeof(bits));
    }
    return XR_XIR_RUN_OK;
}
