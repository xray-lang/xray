/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_value.inc.c - Transactional class construction and field assignment
 *
 * KEY CONCEPT:
 *   Identity handles pin their field storage, type arena and physical accounting domain.
 */
static XrXirValueStatus class_receiver_admit(const XrXirValue *receiver,
    XrXirValueAdmission *admission) {
    if (!receiver || !admission ||
        !xr_xir_type_is_class(xr_xir_type_arena_types(admission->arena),(XrXirType)receiver->type) ||
        !xr_xir_value_valid(receiver)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirObject *object=object_pointer(receiver);
    if (!class_body_layout(object) || admission->arena != object->arena ||
        admission->domain != object->domain)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    return xr_xir_value_admit(receiver,(XrXirType)receiver->type,admission);
}
XR_FUNC XrXirValueStatus xr_xir_class_new(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !admission->domain || !admission->arena || !unit_value(output) ||
        ((count != 0) != (fields != NULL))) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types=xr_xir_type_arena_types(admission->arena);
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    const XrXirStorageLayout *layout=xr_xir_type_arena_storage(admission->arena,type);
    if (!node || !xr_xir_type_is_class(types,type) || node->parameter_span ||
        node->nominal.field_count != count || !layout || layout->field_count != count ||
        !layout->body.alignment || layout->body.alignment > _Alignof(XirClassObject))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (count > admission->work) return XR_XIR_VALUE_LIMIT;
    admission->work-=count;
    for (uint32_t i=0;i<count;++i) {
        if (!class_field_leaf(node->nominal.fields[i])) return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirValueStatus status=xr_xir_value_admit(&fields[i],node->nominal.fields[i],admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    uint64_t bytes=sizeof(XirClassObject)+(uint64_t)layout->body.size;
    if (bytes > SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirObject *object=constructed_allocate(admission->domain,(XrXirTypeArena *)admission->arena,
        type,(size_t)bytes,&status);
    if (!object) return status;
    object->kind=XIR_OBJECT_CLASS;
    memset((XirClassObject *)object+1,0,layout->body.size);
    for (uint32_t i=0;i<count;++i) {
        XrXirValue owned={0};status=xr_xir_value_copy(&fields[i],&owned);
        if (status != XR_XIR_VALUE_OK) {
            XirObject *pending=NULL;
            while(i){XrXirValue previous=class_field_value(object,--i);queue_release(&previous,&pending);}
            constructed_discard(object,(size_t)bytes);release_pending(pending);return status;
        }
        class_field_publish(object,i,&owned);
    }
    *output=(XrXirValue){(uint32_t)type,0,0};memcpy(&output->payload,&object,sizeof(object));
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_class_get(const XrXirValue *receiver, uint32_t field,
    XrXirValue *output) {
    if (!unit_value(output) || !xr_xir_value_valid(receiver) ||
        !owned_carrier_type((XrXirType)receiver->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirObject *object=object_pointer(receiver);
    const XrXirStorageLayout *layout=class_body_layout(object);
    if (!layout || field >= layout->field_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue value=class_field_value(object,field);
    return xr_xir_value_copy(&value,output);
}
XR_FUNC XrXirValueStatus xr_xir_class_set(const XrXirValue *receiver, uint32_t field,
    const XrXirValue *replacement, XrXirValueAdmission *admission) {
    XrXirValueStatus status=class_receiver_admit(receiver,admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XirObject *object=object_pointer(receiver);
    const XrXirTypes *types=xr_xir_type_arena_types(object->arena);
    const XrXirTypeNode *node=xr_xir_type_node(types,object->type);
    if (field >= node->nominal.field_count ||
        !(types->nominals->identities[node->nominal.declaration].fields[field].flags & XR_XIR_FIELD_MUTABLE))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    status=xr_xir_value_admit(replacement,node->nominal.fields[field],admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue owned={0};status=xr_xir_value_copy(replacement,&owned);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue previous=class_field_value(object,field);
    class_field_publish(object,field,&owned);
    xr_xir_value_drop(&previous);return XR_XIR_VALUE_OK;
}
