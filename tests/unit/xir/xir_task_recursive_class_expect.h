/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_recursive_class_expect.h - Independent finite identity-chain values
 */
#ifndef XIR_TASK_RECURSIVE_CLASS_EXPECT_H
#define XIR_TASK_RECURSIVE_CLASS_EXPECT_H
#include "xir/xxir_nullable.h"
static bool constructed_recursive_class_value(const XrXirValue *value,XrXirValueAdmission *admission) {
    XrXirValue next={0},tail={0},array={0},atomic={0},element={0};
    XrXirValueStatus status=xr_xir_class_get(value,2,admission,&next);
    XrXirFaultDetail fault={0};bool completed=false,some=false;const XrXirValue *child=NULL;
    if (status!=XR_XIR_VALUE_OK) goto failed;
    CHECK(xr_xir_nullable_view(&next,&some,&child) && some && child);
    status=xr_xir_class_get(child,0,admission,&array);
    if (status!=XR_XIR_VALUE_OK) goto failed;
    int64_t length=0;status=xr_xir_array_len(&array,admission,&length);
    if (status!=XR_XIR_VALUE_OK) goto failed;
    CHECK(length==2);
    for (int64_t n=0;n<length;++n) {
        status=xr_xir_array_get(&array,n,admission,&element,&fault);
        if (status!=XR_XIR_VALUE_OK) goto failed;
        CHECK(element.type==XR_XIR_I64 && element.payload==30+10*n);xr_xir_value_drop(&element);
    }
    status=xr_xir_class_get(child,1,admission,&atomic);
    if (status!=XR_XIR_VALUE_OK) goto failed;
    XrXirAtomicRequest request={&atomic,NULL,NULL,0,XR_XIR_ATOMIC_OPERATION_LOAD,XR_XIR_I64};
    XrXirAtomicProgress progress={0};
    XrXirAtomicOutcome loaded=xr_xir_atomic_start(&request,admission,NULL,&progress,&element);
    xr_xir_atomic_progress_clear(&progress);
    if (loaded.status!=XR_XIR_RUN_OK || loaded.permission!=XR_XIR_CALL_READY) {
        CHECK(loaded.status==XR_XIR_RUN_STEP_LIMIT || loaded.status==XR_XIR_RUN_FRAME_LIMIT ||
            loaded.status==XR_XIR_RUN_OUT_OF_MEMORY || loaded.permission==XR_XIR_CALL_LIMIT);goto done;
    }
    CHECK(!loaded.continuing && element.type==XR_XIR_I64 && element.payload==3);
    status=xr_xir_class_get(child,2,admission,&tail);
    if (status!=XR_XIR_VALUE_OK) goto failed;
    CHECK(xr_xir_nullable_view(&tail,&some,&child) && !some && !child);
    completed=true;goto done;
failed:CHECK(status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_OOM);
done:
    xr_xir_value_drop(&element);xr_xir_value_drop(&atomic);xr_xir_value_drop(&array);
    xr_xir_value_drop(&tail);xr_xir_value_drop(&next);return completed;
}
#endif // XIR_TASK_RECURSIVE_CLASS_EXPECT_H
