/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_error_value.inc.c - Retained enum views without wrapper allocation
 */
XR_FUNC XrXirValueStatus xr_xir_error_erase(const XrXirValue *value,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !value || !unit_value(output) ||
        (value->type != XR_XIR_ERROR &&
         !xr_xir_type_is_enum(xr_xir_compile_type_arena_types(admission->arena), (XrXirType) value->type)))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(value, (XrXirType) value->type, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    XrXirValue borrowed = {XR_XIR_ERROR, 0, value->payload};
    return xr_xir_value_copy(&borrowed, output);
}
XR_FUNC XrXirValueStatus xr_xir_error_is(const XrXirValue *value, XrXirType type,
    XrXirValueAdmission *admission, bool *matches) {
    if (!matches) return XR_XIR_VALUE_BAD_ARGUMENT;
    *matches = false;
    if (!admission || !value || value->type != XR_XIR_ERROR ||
        !xr_xir_type_is_enum(xr_xir_compile_type_arena_types(admission->arena), type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(value, XR_XIR_ERROR, admission);
    if (status == XR_XIR_VALUE_OK) *matches = object_pointer(value)->type == type;
    return status;
}
XR_FUNC XrXirValueStatus xr_xir_error_narrow(const XrXirValue *value, XrXirType type,
    XrXirValueAdmission *admission, XrXirValue *output) {
    if (!admission || !unit_value(output) || !value || value->type != XR_XIR_ERROR ||
        !xr_xir_type_is_enum(xr_xir_compile_type_arena_types(admission->arena), type))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status = xr_xir_value_admit(value, XR_XIR_ERROR, admission);
    if (status != XR_XIR_VALUE_OK) return status;
    if (object_pointer(value)->type != type) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValue borrowed = {(uint32_t) type, 0, value->payload};
    return xr_xir_value_copy(&borrowed, output);
}

XR_FUNC bool xr_xir_error_borrow(const XrXirValue *value, XrXirValue *output) {
    if (!unit_value(output) || !value || value->type != XR_XIR_ERROR || !xr_xir_value_valid(value)) return false;
    *output = (XrXirValue) {(uint32_t) object_pointer(value)->type, 0, value->payload};
    return true;
}
