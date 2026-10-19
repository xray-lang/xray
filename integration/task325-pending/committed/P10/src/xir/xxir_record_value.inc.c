/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_record_value.inc.c - Exact records over shared owned product storage
 */
XR_FUNC XrXirValueStatus xr_xir_record_new(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission_domain_current(admission) || !xr_xir_record_signature(
        xr_xir_compile_type_arena_types(admission->arena),type) || (!!count != !!fields))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = product_new_graph_operation(type,fields,count,admission,output);
    xr_xir_value_graph_end();return status;
}
XR_FUNC XrXirValueStatus xr_xir_record_get(const XrXirValue *value, uint32_t field,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission_domain_current(admission) || !unit_value(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local = {0};
    XrXirValueStatus status = storage_admission_begin(value,admission,&local);
    if (status != XR_XIR_VALUE_OK) return status;
    const XrXirTypeNode *node = xr_xir_record_signature(xr_xir_compile_type_arena_types(local.arena),(XrXirType)value->type);
    if (!node || field >= node->parameter_count)
        return storage_admission_end(admission,&local,XR_XIR_VALUE_BAD_ARGUMENT);
    xr_xir_value_graph_begin();
    status = xr_xir_value_admit(value,(XrXirType)value->type,&local);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_value_copy(&((XirTuple *)object_pointer(value))->fields[field],output);
    xr_xir_value_graph_end();return storage_admission_end(admission,&local,status);
}
static XrXirValueStatus record_set_graph(const XrXirValue *current, uint32_t field,
    const XrXirValue *value, XrXirValueAdmission *admission, XrXirValue *output) {
    const XrXirTypeNode *node = xr_xir_record_signature(xr_xir_compile_type_arena_types(admission->arena),(XrXirType)current->type);
    if (!node || field >= node->parameter_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(current,(XrXirType)current->type,admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirType type = node->parameters[field].type;
    if (type == XR_XIR_UNIT) {
        if (!unit_value(value)) return XR_XIR_VALUE_BAD_ARGUMENT;
    } else {
        status = xr_xir_value_admit(value,type,admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    if (node->parameter_count > admission->work) return XR_XIR_VALUE_LIMIT;
    admission->work -= node->parameter_count;
    const XirTuple *record = (const XirTuple *)object_pointer(current);
    ProductCopyInput input = {record->fields,record->count,field,value};
    return product_copy_fields((XrXirType)current->type,&input,admission,output);
}
XR_FUNC XrXirValueStatus xr_xir_record_set(const XrXirValuePlace *place, uint32_t field,
    const XrXirValue *value, XrXirValueAdmission *admission) {
    if (!place || !place->payload || !admission_domain_current(admission) ||
        !xr_xir_record_signature(xr_xir_compile_type_arena_types(admission->arena),place->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    int64_t payload;memcpy(&payload,place->payload,sizeof(payload));
    XrXirValue current = {0},replacement = {0};
    if (!xr_xir_value_from_payload(admission->arena,place->type,payload,&current)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission local = {0};
    XrXirValueStatus status = storage_admission_begin(&current,admission,&local);
    if (status != XR_XIR_VALUE_OK) return status;
    xr_xir_value_graph_begin();
    status = record_set_graph(&current,field,value,&local,&replacement);
    if (status == XR_XIR_VALUE_OK) {
        memcpy(place->payload,&replacement.payload,sizeof(replacement.payload));xr_xir_value_drop(&current);
    }
    xr_xir_value_graph_end();return storage_admission_end(admission,&local,status);
}
