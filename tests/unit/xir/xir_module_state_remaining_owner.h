/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_state_remaining_owner.h - Observe canonical runtime allocations and actual class releases
 *
 * KEY CONCEPT:
 *   Finite complete Programs preserve real ownership and physical cleanup.
 */

#ifndef XIR_MODULE_STATE_REMAINING_OWNER_H
#define XIR_MODULE_STATE_REMAINING_OWNER_H
#include "base/xmalloc.h"
#include "xir/xxir_class.h"
#include "xir/xxir_program.h"
#include "xir_source_program_compile_owner.h"
typedef struct RemainingTrace {
    XrXirInstance *instance;uint32_t trace[20],trace_count,published[4],publication_count;
    uint32_t released[4],release_count,constructed,freed[4],free_count,old_freed[4];
    uintptr_t classes[4],old_fields[4];uint32_t replacements,helper_calls,scenario,output;bool inspecting,class_live[4];
} RemainingTrace;
static RemainingTrace *remaining_active,*remaining_registry[2];
static void remaining_actual_free(void *pointer) {
    for(uint32_t i=0;pointer && i<2;++i) {
        RemainingTrace *trace=remaining_registry[i];if(!trace)continue;
        for(uint32_t n=0;n<trace->constructed;++n) {
            if(trace->class_live[n] && (uintptr_t)pointer==trace->classes[n]) {
                CHECK(trace->free_count<4);for(uint32_t j=0;j<trace->free_count;++j)CHECK(trace->freed[j]!=n+1);
                trace->freed[trace->free_count++]=n+1;trace->class_live[n]=false;
            }
            if((uintptr_t)pointer==trace->old_fields[n]) {++trace->old_freed[n];trace->old_fields[n]=0;}
        }
    }
    xr_free(pointer);
}
#undef xr_free
#define xr_free(pointer) remaining_actual_free(pointer)
#define xr_xir_class_new remaining_real_new
#define xr_xir_class_get remaining_real_get
#define xr_xir_class_set remaining_real_set
#include "xir_runtime_allocations.h"
#undef xr_xir_class_new
#undef xr_xir_class_get
#undef xr_xir_class_set
XR_FUNC XrXirValueStatus xr_xir_class_new(XrXirType type,const XrXirValue *fields,
    uint32_t count,XrXirValueAdmission *admission,XrXirValue *output) {
    XrXirValueStatus status=remaining_real_new(type,fields,count,admission,output);
    if(status==XR_XIR_VALUE_OK && remaining_active) {
        RemainingTrace *t=remaining_active;CHECK(count==1 && t->constructed<4);
        for(uint32_t i=0;i<2;++i)if(remaining_registry[i])
            for(uint32_t j=0;j<remaining_registry[i]->constructed;++j)
                if(remaining_registry[i]->class_live[j])CHECK(remaining_registry[i]->classes[j]!=(uintptr_t)output->payload);
        uint32_t n=t->constructed++;t->classes[n]=(uintptr_t)output->payload;t->class_live[n]=true;
        t->old_fields[n]=(uintptr_t)fields[0].payload;
    }
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_class_get(const XrXirValue *receiver,uint32_t field,
    XrXirValueAdmission *admission,XrXirValue *output) {
    XrXirValueStatus status=remaining_real_get(receiver,field,admission,output);
    if(status==XR_XIR_VALUE_OK && remaining_active && !remaining_active->inspecting)
        ++remaining_active->helper_calls;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_class_set(const XrXirValue *receiver,uint32_t field,
    const XrXirValue *replacement,XrXirValueAdmission *admission) {
    XrXirValueStatus status=remaining_real_set(receiver,field,replacement,admission);
    if(remaining_active) {
        RemainingTrace *t=remaining_active;uint32_t found=0;
        for(uint32_t n=0;n<t->constructed;++n)if(t->classes[n]==(uintptr_t)receiver->payload) {
            ++found;CHECK(t->old_freed[n]==(status==XR_XIR_VALUE_OK?1u:0u));
        }
        CHECK(found==1);if(status==XR_XIR_VALUE_OK)++t->replacements;
    }
    return status;
}
#endif // XIR_MODULE_STATE_REMAINING_OWNER_H
