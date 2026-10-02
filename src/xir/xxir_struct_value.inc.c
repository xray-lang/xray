/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_struct_value.inc.c - Failure-atomic nominal boxed snapshots
 */
/* Fields have already passed recursive admission. Copying a checked path
 * must not re-admit every descendant at every ancestor. */
static XrXirValueStatus struct_copy_fields(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    uint64_t bytes = sizeof(XirNominalValue) + (uint64_t) count * sizeof(XrXirValue);
    if (bytes > SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirNominalValue *record = (XirNominalValue *) constructed_allocate(admission->domain,
        (XrXirTypeArena *) admission->arena, type, (size_t) bytes, &status);
    if (!record) return status;
    record->count = count; record->variant = 0; record->fields = (XrXirValue *) (record + 1);
    memset(record->fields, 0, (size_t) count * sizeof(*record->fields));
    for (uint32_t i = 0; i < count; ++i) {
        status = xr_xir_value_copy(&fields[i], &record->fields[i]);
        if (status != XR_XIR_VALUE_OK) {
            while (i) xr_xir_value_drop(&record->fields[--i]);
            constructed_discard(&record->object, (size_t) bytes); return status;
        }
    }
    *output = (XrXirValue) {(uint32_t) type, 0, 0};
    memcpy(&output->payload, &record, sizeof(record)); return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_struct_new(XrXirType type, const XrXirValue *fields,
    uint32_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !admission->domain || !admission->arena || !unit_value(output) ||
        (count != 0) != (fields != NULL)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena), type);
    if (!node || !xr_xir_type_is_struct(xr_xir_compile_type_arena_types(admission->arena), type) ||
        node->parameter_span || node->nominal.field_count != count)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (count > admission->work) return XR_XIR_VALUE_LIMIT;
    admission->work -= count;
    for (uint32_t i = 0; i < count; ++i) {
        XrXirValueStatus status = xr_xir_value_admit(&fields[i], node->nominal.fields[i], admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    return struct_copy_fields(type, fields, count, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_struct_get(const XrXirValue *value, uint32_t field,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!value || !unit_value(output) || !admission) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena), (XrXirType) value->type);
    if (!node || !xr_xir_type_is_struct(xr_xir_compile_type_arena_types(admission->arena), (XrXirType) value->type) ||
        field >= node->nominal.field_count) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(value, (XrXirType) value->type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    return xr_xir_value_copy(&((XirNominalValue *) object_pointer(value))->fields[field], output);
}
XR_FUNC XrXirValueStatus xr_xir_struct_set(const XrXirValuePlace *place, uint32_t field,
    const XrXirValue *value, XrXirValueAdmission *admission) {
    if (!place || !place->payload || !admission) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue destination = {(uint32_t) place->type, 0, 0};
    memcpy(&destination.payload, place->payload, sizeof(destination.payload));
    const XrXirTypes *types = xr_xir_compile_type_arena_types(admission->arena);
    const XrXirTypeNode *node = xr_xir_type_node(types, (XrXirType) destination.type);
    if (!node || !xr_xir_type_is_struct(types, (XrXirType) destination.type) || field >= node->nominal.field_count ||
        !(types->nominals->identities[node->nominal.declaration].fields[field].flags & XR_XIR_FIELD_MUTABLE))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(&destination, (XrXirType) destination.type, admission);
    if (status == XR_XIR_VALUE_OK) status = xr_xir_value_admit(value, node->nominal.fields[field], admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue owned = {0}; status = xr_xir_value_copy(value, &owned);
    if (status != XR_XIR_VALUE_OK) return status;
    XirNominalValue *record = (XirNominalValue *) object_pointer(&destination);
    XrXirValue replacement = {0};
    if (atomic_load_explicit(&record->object.references, memory_order_acquire) != 1) {
        status = xr_xir_struct_new(record->object.type, record->fields, record->count, admission, &replacement);
        if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&owned); return status; }
        record = (XirNominalValue *) object_pointer(&replacement);
    }
    XrXirValue previous = record->fields[field]; record->fields[field] = owned;
    if (replacement.type) {
        memcpy(place->payload, &replacement.payload, sizeof(replacement.payload));
        xr_xir_value_drop(&destination);
    }
    xr_xir_value_drop(&previous); return XR_XIR_VALUE_OK;
}
