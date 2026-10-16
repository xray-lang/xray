/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_tuple_value.inc.c - Failure-atomic immutable ordered field publication
 */
typedef struct ProductCopyInput {
    const XrXirValue *fields;
    uint32_t count, replace;
    const XrXirValue *replacement;
} ProductCopyInput;
static XrXirValueStatus product_copy_fields(XrXirType type, const ProductCopyInput *input,
    XrXirValueAdmission *admission, XrXirValue *output) {
    uint64_t bytes = sizeof(XirTuple) + (uint64_t)input->count * sizeof(XrXirValue);
    if (bytes > SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirTuple *tuple = (XirTuple *)constructed_allocate(admission->domain,
        (XrXirTypeArena *)admission->arena,type,(size_t)bytes,&status);
    if (!tuple) return status;
    tuple->count = input->count;tuple->fields = (XrXirValue *)(tuple+1);
    memset(tuple->fields,0,(size_t)input->count*sizeof(*tuple->fields));
    for (uint32_t i = 0; i < input->count; ++i) {
        const XrXirValue *field = input->replacement && i == input->replace ? input->replacement : &input->fields[i];
        status = xr_xir_value_copy(field,&tuple->fields[i]);
        if (status != XR_XIR_VALUE_OK) {
            while (i) xr_xir_value_drop(&tuple->fields[--i]);
            constructed_discard(&tuple->object,(size_t)bytes);return status;
        }
    }
    XrXirValue result = {(uint32_t)type,0,0};
    xr_xir_value_object_publish(&tuple->object);
    memcpy(&result.payload,&tuple,sizeof(tuple));*output = result;return XR_XIR_VALUE_OK;
}
static XrXirValueStatus product_new_graph_operation(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !admission->domain || !admission->arena || !unit_value(output) || (count && !fields))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xir_product_signature(xr_xir_compile_type_arena_types(admission->arena),type);
    if (!node || node->parameter_span || node->parameter_count != count || (count && !node->parameters))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (count > admission->work) return XR_XIR_VALUE_LIMIT;
    admission->work -= count;
    for (uint32_t i = 0; i < count; ++i) {
        if (node->parameters[i].type == XR_XIR_UNIT) {
            if (!unit_value(&fields[i])) return XR_XIR_VALUE_BAD_ARGUMENT;
        } else {
            XrXirValueStatus status = xr_xir_value_admit(&fields[i],node->parameters[i].type,admission);
            if (status != XR_XIR_VALUE_OK) return status;
        }
    }
    ProductCopyInput input = {fields,count,UINT32_MAX,NULL};
    return product_copy_fields(type,&input,admission,output);
}
XR_FUNC XrXirValueStatus xr_xir_tuple_new(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    /* The empty tuple is an owner-free Unit value, including at scalar entries. */
    if (type==XR_XIR_UNIT) {
        if (!unit_value(output) || count || fields || (admission && !admission_domain_current(admission)))
            return XR_XIR_VALUE_BAD_ARGUMENT;
        if (admission) {
            if (!admission->work) return XR_XIR_VALUE_LIMIT;
            --admission->work;
        }
        return XR_XIR_VALUE_OK;
    }
    if (!admission_domain_current(admission) || !xr_xir_tuple_signature(
        xr_xir_compile_type_arena_types(admission->arena),type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = product_new_graph_operation(type, fields, count, admission, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static XrXirValueStatus xr_xir_tuple_get_graph_operation(const XrXirValue *value,uint32_t field,
    XrXirValue *output) {
    if (!unit_value(output) || !xr_xir_value_valid(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node=xr_xir_tuple_signature(
        xr_xir_compile_type_arena_types(xr_xir_value_arena(value)),(XrXirType)value->type);
    if (!node || field>=node->parameter_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirTuple *tuple=(const XirTuple *)object_pointer(value);
    return xr_xir_value_copy(&tuple->fields[field],output);
}
XR_FUNC XrXirValueStatus xr_xir_tuple_get(const XrXirValue *value,uint32_t field,
    XrXirValue *output) {
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_tuple_get_graph_operation(value, field, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}
