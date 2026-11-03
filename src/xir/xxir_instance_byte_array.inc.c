/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_byte_array.inc.c - Current-instance byte candidates
 *
 * KEY CONCEPT:
 *   Both engines borrow the active call admission only until the operation returns.
 */
XR_FUNC XrXirCallStatus xr_xir_instance_array_byte_append(XrXirCallView *view,
    const XrXirValue *array,const XrXirValue *snapshot,XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!view_instance(view)) return XR_XIR_CALL_BAD_STATE;
    if (!output || !value_unit(*output)) return XR_XIR_CALL_BAD_ARGUMENT;
    return value_call_status(xr_xir_array_byte_append(array,snapshot,xr_xir_call_admission(view),output));
}
XR_FUNC XrXirCallStatus xr_xir_instance_array_byte_repeat(XrXirCallView *view,
    const XrXirValue *array,int64_t distance,int64_t count,XrXirValue *output,XrXirFaultDetail *fault) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!view_instance(view)) return XR_XIR_CALL_BAD_STATE;
    if (!output || !value_unit(*output) || !fault) return XR_XIR_CALL_BAD_ARGUMENT;
    return value_call_status(xr_xir_array_byte_repeat(array,distance,count,xr_xir_call_admission(view),output,fault));
}
