/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_path.inc.c - Active-call authority for synchronous value paths
 */
static XrXirCallStatus instance_path_cell(XrXirCallView *view,
    const XrXirInstance *instance, const XrXirValueReceiver *receiver) {
    if (receiver->kind != XR_XIR_ROOT_CELL || receiver->local_payload || receiver->slot ||
        !xr_xir_type_is_cell(instance->program->types, (XrXirType)receiver->value.type) ||
        xr_xir_cell_element(instance->program->types, (XrXirType)receiver->value.type) != receiver->type)
        return XR_XIR_CALL_BAD_STATE;
    return instance_cell_access(view, instance, &receiver->value);
}
XR_FUNC XrXirCallStatus xr_xir_instance_cell_project(XrXirCallView *view,
    XrXirType type, const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    XrXirValue *output) {
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver || !output || !value_unit(*output)) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status = XR_XIR_CALL_READY;
    if (receiver->kind == XR_XIR_ROOT_OBJECT) {
        if (receiver->local_payload || receiver->slot || receiver->value.type != (uint32_t)receiver->type ||
            !xr_xir_type_is_class(instance->program->types, receiver->type) || view_is_child(instance, view))
            return XR_XIR_CALL_BAD_STATE;
    } else status = instance_path_cell(view, instance, receiver);
    return status == XR_XIR_CALL_READY ? value_call_status(
        xr_xir_call_cell_project(view, type, &receiver->value, path, output)) : status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_read(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    XrXirValue *output, XrXirFaultDetail *fault) {
    if (!fault) return XR_XIR_CALL_BAD_ARGUMENT;
    *fault = (XrXirFaultDetail){0};
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver || !output || !value_unit(*output)) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    if (receiver->kind == XR_XIR_ROOT_CELL) {
        XrXirCallStatus access = instance_path_cell(view, instance, receiver);
        return access == XR_XIR_CALL_READY ? value_call_status(
            xr_xir_call_cell_path_read(view, &receiver->value, path, output, fault)) : access;
    }
    XrXirValuePlace place = {0};
    XrXirCallStatus status = value_place(view, instance, receiver, false, admission, &place);
    if (status != XR_XIR_CALL_READY) return status;
    return value_call_status(xr_xir_value_path_read(&place, path, admission, output, fault));
}
static XrXirCallStatus instance_path_mutate(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    const XrXirValue *value, XrXirFaultDetail *fault, bool append) {
    if (!fault) return XR_XIR_CALL_BAD_ARGUMENT;
    *fault = (XrXirFaultDetail){0};
    XrXirInstance *instance = view_instance(view);
    if (!instance || !receiver) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    if (receiver->kind == XR_XIR_ROOT_CELL) {
        XrXirCallStatus access = instance_path_cell(view, instance, receiver);
        return access == XR_XIR_CALL_READY ? value_call_status(
            xr_xir_call_cell_path_write(view, &receiver->value, path, value, fault, append)) : access;
    }
    XrXirValuePlace place = {0};
    XrXirCallStatus status = value_place(view, instance, receiver, true, admission, &place);
    if (status != XR_XIR_CALL_READY) return status;
    return value_call_status(append ? xr_xir_value_path_push(&place, path, value, admission, fault) :
        xr_xir_value_path_write(&place, path, value, admission, fault));
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_length(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    XrXirValue *output, XrXirFaultDetail *fault) {
    if (fault) *fault = (XrXirFaultDetail){0};
    if (!output || !value_unit(*output)) return XR_XIR_CALL_BAD_STATE;
    XrXirValue array = {0};
    XrXirCallStatus status = xr_xir_instance_path_read(view, receiver, path, &array, fault);
    if (status != XR_XIR_CALL_READY) return status;
    int64_t length = 0;
    status = value_call_status(xr_xir_array_len(&array, xr_xir_call_admission(view), &length));
    xr_xir_value_drop(&array);
    if (status == XR_XIR_CALL_READY) *output = (XrXirValue){XR_XIR_I64, 0, length};
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_write(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    const XrXirValue *value, XrXirFaultDetail *fault) {
    return instance_path_mutate(view, receiver, path, value, fault, false);
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_capacity(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    XrXirValue *output, XrXirFaultDetail *fault) {
    if (fault) *fault = (XrXirFaultDetail){0};
    if (!output || !value_unit(*output)) return XR_XIR_CALL_BAD_STATE;
    XrXirValue array = {0};
    XrXirCallStatus status = xr_xir_instance_path_read(view, receiver, path, &array, fault);
    if (status != XR_XIR_CALL_READY) return status;
    int64_t capacity = 0;
    status = value_call_status(xr_xir_array_capacity(&array, xr_xir_call_admission(view), &capacity));
    xr_xir_value_drop(&array);
    if (status == XR_XIR_CALL_READY) *output = (XrXirValue){XR_XIR_I64, 0, capacity};
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_push(XrXirCallView *view,
    const XrXirValueReceiver *receiver, const XrXirValuePath *path,
    const XrXirValue *value, XrXirFaultDetail *fault) {
    return instance_path_mutate(view, receiver, path, value, fault, true);
}
