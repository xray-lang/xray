/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_byte_array_value.inc.c - Owned byte candidates from canonical bulk kernels
 *
 * KEY CONCEPT:
 *   A complete private candidate exists before any caller place is published.
 */
#include "../shared/xr_byte_array_append_core.h"
#include "../shared/xr_byte_array_repeat_core.h"
typedef struct ByteArrayCandidate {
    const XrXirValue *snapshot;
    int64_t distance,count;
    XrXirFaultDetail *fault;
} ByteArrayCandidate;
static XrXirValueStatus byte_array_candidate(const XrXirValue *value,const ByteArrayCandidate *request,
    XrXirValueAdmission *admission,XrXirValue *output) {
    if (!unit_value(output) || !value || !request || !admission || !admission->domain ||
        !admission_domain_current(admission) ||
        !xr_xir_type_is_array(xr_xir_compile_type_arena_types(admission->arena),(XrXirType)value->type) ||
        xr_xir_array_element(xr_xir_compile_type_arena_types(admission->arena),(XrXirType)value->type)!=XR_XIR_U8)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=xr_xir_value_admit(value,(XrXirType)value->type,admission);
    if (status!=XR_XIR_VALUE_OK) return status;
    const XirArray *source=(const XirArray *)object_pointer(value),*snapshot=NULL;
    if (source->element!=XR_XIR_U8 || source->stride!=1) return XR_XIR_VALUE_BAD_ARGUMENT;
    int64_t count=request->count;
    if (request->snapshot) {
        status=xr_xir_value_admit(request->snapshot,(XrXirType)value->type,admission);
        if (status!=XR_XIR_VALUE_OK) return status;
        snapshot=(const XirArray *)object_pointer(request->snapshot);
        if (snapshot->element!=XR_XIR_U8 || snapshot->stride!=1) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (snapshot->length>INT32_MAX) return XR_XIR_VALUE_LIMIT;
        count=(int64_t)snapshot->length;
    } else {
        if (!request->fault) return XR_XIR_VALUE_BAD_ARGUMENT;
        *request->fault=(XrXirFaultDetail){0};
        if (request->distance<=0 || (uint64_t)request->distance>source->length || count<0) {
            int64_t index=count<0?count:request->distance<=0?(int64_t)source->length:
                (int64_t)source->length-request->distance;
            *request->fault=(XrXirFaultDetail){XR_XIR_PANIC_BOUNDS,0,index,(int64_t)source->length};
            return XR_XIR_VALUE_BOUNDS;
        }
    }
    if (source->length>INT32_MAX || count>INT32_MAX || source->length>(uint64_t)(INT32_MAX-count))
        return XR_XIR_VALUE_LIMIT;
    size_t length=source->length+(size_t)count,capacity=source->capacity;
    if (length>capacity && !array_capacity(length,1,&capacity)) return XR_XIR_VALUE_LIMIT;
    uint64_t work=(uint64_t)source->length+(uint64_t)count+1u;
    if (!admission_is_compile(admission)) {
        if (work>admission->work || !xr_xir_domain_work(admission->domain,work)) return XR_XIR_VALUE_LIMIT;
        admission->work-=work;
    }
    XirArray *candidate=NULL;
    status=array_allocate((XrXirType)value->type,capacity,admission,&candidate);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirValue owned=array_value(candidate);
    if (source->length) memcpy(candidate->data,source->data,source->length);
    candidate->length=source->length;
    if (snapshot) {
        XrByteArrayAppendView view={candidate->data,(int64_t)candidate->length,
            (int64_t)candidate->capacity,XR_ELEM_U8,true,candidate};
        XrByteArrayAppendResult result=xr_byte_array_append_core(&view,snapshot->data,count,
            XR_ELEM_U8,snapshot,NULL,NULL);
        if (result.status!=XR_BYTE_ARRAY_APPEND_OK) status=XR_XIR_VALUE_BAD_ARGUMENT;
        else candidate->length=(size_t)view.length;
    } else {
        XrByteArrayRepeatView view={candidate->data,(int64_t)candidate->length,
            (int64_t)candidate->capacity,XR_ELEM_U8,true};
        XrByteArrayRepeatResult result=xr_byte_array_repeat_tail_core(&view,request->distance,count,NULL,NULL);
        if (result.status!=XR_BYTE_ARRAY_REPEAT_OK) status=XR_XIR_VALUE_BAD_ARGUMENT;
        else candidate->length=(size_t)view.length;
    }
    if (status!=XR_XIR_VALUE_OK) xr_xir_value_drop(&owned);
    else *output=owned;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_array_byte_append(const XrXirValue *array,
    const XrXirValue *snapshot,XrXirValueAdmission *admission,XrXirValue *output) {
    if (!snapshot || !admission) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(array,admission,&local);
    if (status!=XR_XIR_VALUE_OK)return status;
    ByteArrayCandidate request={snapshot,0,0,NULL};xr_xir_value_graph_begin();
    status=byte_array_candidate(array,&request,&local,output);
    xr_xir_value_graph_end();return storage_admission_end(admission,&local,status);
}
XR_FUNC XrXirValueStatus xr_xir_array_byte_repeat(const XrXirValue *array,
    int64_t distance,int64_t count,XrXirValueAdmission *admission,XrXirValue *output,XrXirFaultDetail *fault) {
    if (fault) *fault=(XrXirFaultDetail){0};
    if (!admission) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local={0};
    XrXirValueStatus status=storage_admission_begin(array,admission,&local);
    if (status!=XR_XIR_VALUE_OK)return status;
    ByteArrayCandidate request={NULL,distance,count,fault};xr_xir_value_graph_begin();
    status=byte_array_candidate(array,&request,&local,output);
    xr_xir_value_graph_end();return storage_admission_end(admission,&local,status);
}
