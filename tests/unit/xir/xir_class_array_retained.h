/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_array_retained.h - Owned Array fields outlive all execution owners
 *
 * KEY CONCEPT:
 *   Data copies retain actual metadata without restoring execution authority.
 */
#ifndef XIR_CLASS_ARRAY_RETAINED_H
#define XIR_CLASS_ARRAY_RETAINED_H
#include "xir/xxir_array.h"
#include "xir/xxir_output.h"
typedef struct ClassArrayTrace {unsigned count;} ClassArrayTrace;
static bool class_array_trace(void *context,const XrXirOutputGroup *group) {
    ClassArrayTrace *trace=context;
    C(group->count==1 && trace->count==0 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==41);
    ++trace->count;return true;
}
static void class_array_suspend_cases(XrXirProgram *program) {
    for(unsigned cancel=0;cancel<2;++cancel){
        XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();
        ClassArrayTrace trace={0};config.output=(XrXirOutputProvider){class_array_trace,&trace};
        C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        C(xr_xir_instance_start(instance,5,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=xr_xir_instance_poll(instance);C(result.outcome.status==XR_XIR_CALL_SUSPENDED && !trace.count);
        if(!cancel){
            C(xr_xir_instance_resume(instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
            C(xr_xir_instance_poll(instance).outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue value={0};C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==41);xr_xir_value_drop(&value);
        }
        C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);C(trace.count==1);
    }
}
static void class_array_retained(XrXirValue *receiver) {
    XrXirValue array={0},copy={0},text={0};
    C(xr_xir_class_get(receiver,0,&(XrXirValueAdmission){.arena=xr_xir_value_arena(receiver),.work=10000},&array)==XR_XIR_VALUE_OK);
    C(xr_xir_value_copy(&array,&copy)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(receiver);xr_xir_value_drop(&array);
    XrXirValueAdmission admission={xr_xir_value_arena(&copy),NULL,NULL,NULL,64,0};
    XrXirFaultDetail fault={0};
    C(xr_xir_array_get(&copy,0,&admission,&text,&fault)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&copy);
    const char *bytes=NULL;size_t length=0;
    C(xr_xir_string_view(&text,&bytes,&length)&&length==6&&!memcmp(bytes,"mapped",6));
    xr_xir_value_drop(&text);
}
#endif
