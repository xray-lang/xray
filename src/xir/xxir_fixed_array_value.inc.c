/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_fixed_array_value.inc.c - Failure-atomic publication of fixed byte storage
 *
 * KEY CONCEPT:
 *   All type, element and allocation checks precede publication of the one owner.
 */
static XrXirValueStatus fixed_array_prepare(XrXirType type,uint32_t count,
    XrXirValueAdmission *admission,XrXirValue *output,XirFixedArray **prepared) {
    if(!admission || !admission->arena || !admission_domain_current(admission) ||
        !unit_value(output) || !count || xr_xir_fixed_array_length(
        xr_xir_compile_type_arena_types(admission->arena),type)!=count)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if(!admission_is_compile(admission)) {
        if(count>admission->work || !xr_xir_domain_work(admission->domain,count))return XR_XIR_VALUE_LIMIT;
        admission->work-=count;
    }
    uint64_t bytes=sizeof(XirFixedArray)+(uint64_t)count;
    if(bytes>SIZE_MAX)return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirFixedArray *fixed=(XirFixedArray *)constructed_allocate(admission->domain,
        (XrXirTypeArena *)admission->arena,type,(size_t)bytes,&status);
    if(!fixed)return status;
    fixed->length=count;fixed->bytes=(uint8_t *)(fixed+1);*prepared=fixed;return XR_XIR_VALUE_OK;
}
static void fixed_array_publish(XirFixedArray *fixed,XrXirValue *output) {
    XR_CHECK(fixed && fixed->length && unit_value(output),"fixed publication owns exact nonempty storage");
    xr_xir_value_object_publish(&fixed->object);
    *output=(XrXirValue){fixed->object.type,0,(int64_t)(uintptr_t)fixed};
}
XR_FUNC XrXirValueStatus xr_xir_fixed_array_from_bytes(XrXirType type,const uint8_t *bytes,
    uint32_t count,XrXirValueAdmission *admission,XrXirValue *output) {
    if(!bytes)return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XirFixedArray *fixed=NULL;
    XrXirValueStatus status=fixed_array_prepare(type,count,admission,output,&fixed);
    if(status==XR_XIR_VALUE_OK) {
        memcpy(fixed->bytes,bytes,count);fixed_array_publish(fixed,output);
    }
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_fixed_array_new(XrXirType type,const XrXirValue *values,
    uint32_t count,XrXirValueAdmission *admission,XrXirValue *output) {
    if(!values || !admission || !admission->arena || !admission_domain_current(admission) ||
        !unit_value(output) || !count || xr_xir_fixed_array_length(
        xr_xir_compile_type_arena_types(admission->arena),type)!=count)return XR_XIR_VALUE_BAD_ARGUMENT;
    if(!admission_is_compile(admission) && count>admission->work)return XR_XIR_VALUE_LIMIT;
    for(uint32_t i=0;i<count;++i)
        if(values[i].type!=XR_XIR_U8 || values[i].reserved ||
            !xr_xir_integer_payload_valid(XR_XIR_U8,values[i].payload))return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();XirFixedArray *fixed=NULL;
    XrXirValueStatus status=fixed_array_prepare(type,count,admission,output,&fixed);
    if(status==XR_XIR_VALUE_OK) {
        for(uint32_t i=0;i<count;++i)fixed->bytes[i]=(uint8_t)values[i].payload;
        fixed_array_publish(fixed,output);
    }
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_fixed_array_get(const XrXirValue *value,int64_t index,
    XrXirValue *output,XrXirFaultDetail *fault) {
    if(!unit_value(output) || !xr_xir_value_valid(value) || !xr_xir_type_is_fixed_array(
        xr_xir_compile_type_arena_types(xr_xir_value_arena(value)),(XrXirType)value->type))return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirObject *object=object_pointer(value);
    if(object->kind!=XR_XIR_TYPE_FIXED_ARRAY)return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirFixedArray *fixed=(const XirFixedArray *)object;
    if(index<0 || (uint64_t)index>=fixed->length) {
        if(fault)*fault=(XrXirFaultDetail){XR_XIR_PANIC_BOUNDS,0,index,fixed->length};
        return XR_XIR_VALUE_BOUNDS;
    }
    *output=(XrXirValue){XR_XIR_U8,0,fixed->bytes[index]};return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_fixed_array_count(const XrXirValue *value,int64_t *output) {
    if(!output || !xr_xir_value_valid(value) || !xr_xir_type_is_fixed_array(
        xr_xir_compile_type_arena_types(xr_xir_value_arena(value)),(XrXirType)value->type))return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirObject *object=object_pointer(value);
    if(object->kind!=XR_XIR_TYPE_FIXED_ARRAY)return XR_XIR_VALUE_BAD_ARGUMENT;
    *output=((const XirFixedArray *)object)->length;return XR_XIR_VALUE_OK;
}
