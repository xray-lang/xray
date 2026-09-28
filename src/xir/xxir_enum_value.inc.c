/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_enum_value.inc.c - Failure-atomic active payload ownership
 */
XR_FUNC XrXirValueStatus xr_xir_enum_new(XrXirType type, uint32_t variant,
    const XrXirValue *fields, uint32_t count, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !admission->domain || !admission->arena || !unit_value(output) ||
        (count != 0) != (fields != NULL)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types = xr_xir_type_arena_types(admission->arena);
    if (!xr_xir_type_is_enum(types, type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    const XrXirNominalIdentity *identity = &types->nominals->identities[node->nominal.declaration];
    if (variant >= identity->variant_count || count != identity->variants[variant].field_count)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission->work || count > admission->work - 1) return XR_XIR_VALUE_LIMIT;
    admission->work -= (uint64_t) count + 1;
    XrXirTypeArena *arena = (XrXirTypeArena *) admission->arena;
    if (!count) {
        const XirNominalValue *record = xr_xir_type_arena_empty_variant(arena, type, variant);
        if (!record) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (!xr_xir_type_arena_retain(arena)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
        *output = (XrXirValue) {(uint32_t) type, 0, 0};
        memcpy(&output->payload, &record, sizeof(record)); return XR_XIR_VALUE_OK;
    }
    uint32_t begin = identity->variants[variant].field_begin;
    for (uint32_t i = 0; i < count; ++i) {
        XrXirValueStatus status = xr_xir_value_admit(&fields[i], node->nominal.fields[begin + i], admission);
        if (status != XR_XIR_VALUE_OK) return status;
    }
    uint64_t bytes = sizeof(XirNominalValue) + (uint64_t) count * sizeof(XrXirValue);
    if (bytes > SIZE_MAX) return XR_XIR_VALUE_LIMIT;
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XirNominalValue *record = (XirNominalValue *) constructed_allocate(admission->domain, arena, type, (size_t) bytes, &status);
    if (!record) return status;
    record->variant = variant; record->count = count; record->fields = (XrXirValue *) (record + 1);
    for (uint32_t i = 0; i < count; ++i) {
        record->fields[i] = (XrXirValue) {0};
        status = xr_xir_value_copy(&fields[i], &record->fields[i]);
        if (status != XR_XIR_VALUE_OK) {
            while (i) xr_xir_value_drop(&record->fields[--i]);
            constructed_discard(&record->object, (size_t) bytes); return status;
        }
    }
    *output = (XrXirValue) {(uint32_t) type, 0, 0};
    memcpy(&output->payload, &record, sizeof(record)); return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_enum_variant(const XrXirValue *value, uint32_t *output) {
    if (!output || !xr_xir_value_valid(value) || !owned_carrier_type((XrXirType) value->type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirObject *object = object_pointer(value);
    if (!xr_xir_type_is_enum(xr_xir_type_arena_types(object->arena), object->type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    *output = ((const XirNominalValue *) object)->variant;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_enum_get(const XrXirValue *value, uint32_t variant,
    uint32_t field, XrXirValueAdmission *admission, XrXirValue *output) {
    uint32_t actual = 0;
    if (!unit_value(output) || !admission || xr_xir_enum_variant(value, &actual) != XR_XIR_VALUE_OK ||
        actual != variant) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XirNominalValue *record = (const XirNominalValue *) object_pointer(value);
    if (field >= record->count) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(value, (XrXirType) value->type, admission);
    return status == XR_XIR_VALUE_OK ? xr_xir_value_copy(&record->fields[field], output) : status;
}
