/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nullable_value.inc.c - Failure-atomic active payload construction
 */
XR_FUNC XrXirValueStatus xr_xir_nullable_new(XrXirType type,
    const XrXirValue *payload, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !admission->domain || !admission->arena || !unit_value(output))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena), type);
    if (!node || node->kind != XR_XIR_TYPE_NULLABLE || node->parameter_span)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission->work) return XR_XIR_VALUE_LIMIT;
    --admission->work;
    XrXirValueStatus status = payload ? xr_xir_value_admit(payload, node->element, admission) : XR_XIR_VALUE_OK;
    if (status != XR_XIR_VALUE_OK) return status;
    size_t bytes = sizeof(XirNominalValue) + (payload ? sizeof(XrXirValue) : 0);
    XirNominalValue *record = (XirNominalValue *)constructed_allocate(admission->domain,
        (XrXirTypeArena *)admission->arena, type, bytes, &status);
    if (!record) return status;
    record->variant = payload ? 1 : 0; record->count = record->variant;
    record->fields = payload ? (XrXirValue *)(record + 1) : NULL;
    if (payload) {
        record->fields[0] = (XrXirValue){0};
        status = xr_xir_value_copy(payload, &record->fields[0]);
        if (status != XR_XIR_VALUE_OK) { constructed_discard(&record->object, bytes); return status; }
    }
    XrXirValue result = {(uint32_t)type, 0, 0};
    memcpy(&result.payload, &record, sizeof(record)); *output = result;
    return XR_XIR_VALUE_OK;
}
XR_FUNC bool xr_xir_nullable_view(const XrXirValue *value, bool *some,
    const XrXirValue **payload) {
    if (!some || !payload || !xr_xir_value_valid(value) ||
        !xr_xir_type_is_nullable(xr_xir_compile_type_arena_types(xr_xir_value_arena(value)), (XrXirType)value->type))
        return false;
    const XirNominalValue *record = (const XirNominalValue *)object_pointer(value);
    *some = record->variant != 0; *payload = record->count ? record->fields : NULL;
    return true;
}
