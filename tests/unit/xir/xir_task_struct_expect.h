/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_task_struct_expect.h - Independent struct values after Task code retirement
 */
#ifndef XIR_TASK_STRUCT_EXPECT_H
#define XIR_TASK_STRUCT_EXPECT_H
#include "xir/xxir_struct.h"
#include "xir/xxir_array.h"
#include "xir/xxir_atomic.h"
static bool constructed_value(const XrXirValue *value,unsigned family) {
    CHECK(!family && xr_xir_value_valid(value));
    const XirObject *object=(const XirObject *)(uintptr_t)value->payload;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(object->arena);
    CHECK(xr_xir_type_is_struct(types,(XrXirType)value->type));
    XrXirValueAdmission admission={object->arena,object->domain,NULL,NULL,1000000,16777216};
    XrXirValue array={0},atomic={0},element={0};XrXirFaultDetail fault={0};bool completed=false;
    XrXirValueStatus status=xr_xir_struct_get(value,0,&admission,&array);
    if (status!=XR_XIR_VALUE_OK) goto failed;
    status=xr_xir_struct_get(value,1,&admission,&atomic);
    if (status!=XR_XIR_VALUE_OK) goto failed;
    int64_t length=0;status=xr_xir_array_len(&array,&admission,&length);
    if (status!=XR_XIR_VALUE_OK) goto failed;
    CHECK(length==2);
    for (int64_t n=0;n<length;++n) {
        status=xr_xir_array_get(&array,n,&admission,&element,&fault);
        if (status!=XR_XIR_VALUE_OK) goto failed;
        CHECK(element.type==XR_XIR_I64 && element.payload==10*(n+1));xr_xir_value_drop(&element);
    }
    XrXirAtomicRequest request={&atomic,NULL,NULL,0,XR_XIR_ATOMIC_OPERATION_LOAD,XR_XIR_I64};
    XrXirAtomicProgress progress={0};
    XrXirAtomicOutcome loaded=xr_xir_atomic_start(&request,&admission,NULL,&progress,&element);
    xr_xir_atomic_progress_clear(&progress);
    if (loaded.status!=XR_XIR_RUN_OK || loaded.permission!=XR_XIR_CALL_READY) {
        CHECK(loaded.status==XR_XIR_RUN_STEP_LIMIT || loaded.status==XR_XIR_RUN_FRAME_LIMIT ||
            loaded.status==XR_XIR_RUN_OUT_OF_MEMORY || loaded.permission==XR_XIR_CALL_LIMIT);goto done;
    }
    CHECK(!loaded.continuing && element.type==XR_XIR_I64 && element.payload==42);completed=true;goto done;
failed:
    CHECK(status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_OOM);
done:
    xr_xir_value_drop(&element);xr_xir_value_drop(&atomic);xr_xir_value_drop(&array);return completed;
}
#endif // XIR_TASK_STRUCT_EXPECT_H
