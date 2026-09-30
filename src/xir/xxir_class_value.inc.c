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
        XrXirValueStatus status=xr_xir_value_admit(&fields[i],node->nominal.fields[i],admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    ClassAllocation allocation={0};
    XrXirValueStatus status=class_allocation_layout(admission,type,&allocation);
    if (status != XR_XIR_VALUE_OK) return status;
    XirObject *object=constructed_allocate(admission->domain,(XrXirTypeArena *)admission->arena,
        type,allocation.bytes,&status);
    if (!object) return status;
    object->kind=XIR_OBJECT_CLASS;
    XirClassObject *instance=(XirClassObject *)object;
    instance->allocation_bytes=allocation.bytes;instance->release_offset=allocation.release_offset;
    instance->release_capacity=allocation.release_capacity;
    unsigned char *body=(unsigned char *)(instance+1);
    memset(body,0,layout->body.size);
    for (uint32_t i=0;i<count;++i) {
        StoragePack pack={0};
        status=storage_pack_begin(admission,node->nominal.fields[i],&pack);
        if (status == XR_XIR_VALUE_OK)
            status=storage_pack_value(&pack,&fields[i],body+layout->field_offsets[i]);
        storage_pack_end(&pack);
        if (status != XR_XIR_VALUE_OK) {
            XirObject *pending=NULL;class_release_fields(object,i,&pending);
            constructed_discard(object,allocation.bytes);release_pending(pending);return status;
        }
    }
    *output=(XrXirValue){(uint32_t)type,0,0};memcpy(&output->payload,&object,sizeof(object));
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_class_get(const XrXirValue *receiver, uint32_t field,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !unit_value(output) || !xr_xir_value_valid(receiver) ||
        !owned_carrier_type((XrXirType)receiver->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XirObject *object=object_pointer(receiver);
    const XrXirStorageLayout *layout=class_body_layout(object);
    if (!layout || field >= layout->field_count || admission->arena != object->arena)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission->work) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    const XrXirTypeNode *node=xr_xir_type_node(xr_xir_type_arena_types(object->arena),object->type);
    const unsigned char *body=(const unsigned char *)((const XirClassObject *)object+1);
    StorageSpan span={node->nominal.fields[field],body+layout->field_offsets[field]};
    return storage_unpack(span,admission,output);
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
    XrXirLayout physical={0};
    if (!xr_xir_type_arena_layout(object->arena,node->nominal.fields[field],&physical))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    StoragePrepared prepared={0};
    status=storage_prepared_begin(&prepared,replacement,physical.size,admission);
    if (status != XR_XIR_VALUE_OK) return status;
    const XrXirStorageLayout *layout=class_body_layout(object);
    unsigned char *bytes=(unsigned char *)((XirClassObject *)object+1)+layout->field_offsets[field];
    /* No fallible operation remains. Prepared bytes own the replacement before
     * any old owner is released, including a source alias of this field. */
    storage_pack_release(&prepared.pack,bytes,UINT64_MAX);
    if (physical.size) memcpy(bytes,prepared.bytes,physical.size);
    prepared.owns=false;storage_prepared_end(&prepared);return XR_XIR_VALUE_OK;
}
