/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nullable_value.inc.c - Failure-atomic active payload construction
 */
#include "../shared/xr_decimal_float.h"
static XrXirValueStatus xr_xir_nullable_new_graph_operation(XrXirType type,
    const XrXirValue *payload, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !admission->domain || !admission->arena || !unit_value(output))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypeNode *node = xr_xir_type_node(xr_xir_compile_type_arena_types(admission->arena), type);
    if (!node || node->kind != XR_XIR_TYPE_NULLABLE || node->parameter_span)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!admission_is_compile(admission) && (!admission->work)) return XR_XIR_VALUE_LIMIT;
    if (!admission_is_compile(admission)) --admission->work;
    XrXirValueStatus status = !payload ? XR_XIR_VALUE_OK : node->element==XR_XIR_UNIT ?
        (unit_value(payload)?XR_XIR_VALUE_OK:XR_XIR_VALUE_BAD_ARGUMENT) : xr_xir_value_admit(payload, node->element, admission);
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
    xr_xir_value_object_publish(&record->object);
    memcpy(&result.payload, &record, sizeof(record)); *output = result;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xr_xir_nullable_new(XrXirType type,
    const XrXirValue *payload, XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus graph_outcome = xr_xir_nullable_new_graph_operation(type, payload, admission, output);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static bool xr_xir_nullable_view_graph_operation(const XrXirValue *value, bool *some,
    const XrXirValue **payload) {
    if (!some || !payload || !xr_xir_value_valid(value) ||
        !xr_xir_type_is_nullable(xr_xir_compile_type_arena_types(xr_xir_value_arena(value)), (XrXirType)value->type))
        return false;
    const XirNominalValue *record = (const XirNominalValue *)object_pointer(value);
    *some = record->variant != 0; *payload = record->count ? record->fields : NULL;
    return true;
}
XR_FUNC bool xr_xir_nullable_view(const XrXirValue *value, bool *some,
    const XrXirValue **payload) {
    xr_xir_value_graph_begin();
    bool graph_outcome = xr_xir_nullable_view_graph_operation(value, some, payload);
    xr_xir_value_graph_end();
    return graph_outcome;
}
static bool nullable_decimal_work(void *context, uint64_t units) {
    XrXirValueAdmission *admission = context;
    if (units > admission->work || !xr_xir_domain_work(admission->domain, units)) return false;
    admission->work -= units; return true;
}
static XrXirValueStatus nullable_decimal_parse(const XrXirValue *text, XrXirType type,
    XrXirValueAdmission *admission, XrXirValue *output) {
    const char *bytes = NULL; size_t length = 0;
    if (!admission || !admission->arena || !text || !unit_value(output) ||
        xr_xir_nullable_element(xr_xir_compile_type_arena_types(admission->arena), type) != XR_XIR_F64)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    if ((!admission_is_compile(admission) && !nullable_decimal_work(admission, sizeof(*text)))) return XR_XIR_VALUE_LIMIT;
    if (!xr_xir_string_view(text, &bytes, &length) || length > UINT32_MAX) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrDecimalWork work = {admission, nullable_decimal_work, false}; uint64_t bits = 0;
    XrDecimalStatus status = xr_decimal_float_text_work(admission_is_compile(admission)?NULL:&work, bytes, length, 64, &bits);
    if (status == XR_DECIMAL_WORK_LIMIT) return XR_XIR_VALUE_LIMIT;
    XrXirValue payload = {XR_XIR_F64, 0, 0}; memcpy(&payload.payload, &bits, sizeof(bits));
    return xr_xir_nullable_new_graph_operation(type, status == XR_DECIMAL_OK ? &payload : NULL, admission, output);
}
XR_FUNC XrXirValueStatus xr_xir_f64_try_parse(const XrXirValue *text, XrXirType type,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission_domain_current(admission)) return XR_XIR_VALUE_BAD_ARGUMENT;
    xr_xir_value_graph_begin();
    XrXirValueStatus status = nullable_decimal_parse(text, type, admission, output);
    xr_xir_value_graph_end(); return status;
}
