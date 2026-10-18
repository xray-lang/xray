/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_string_owned.inc.c - Shared admitted owned String entry points
 */
XR_FUNC XrXirCallStatus xr_xir_instance_string_copy_bytes(XrXirCallView *view,
    XrXirType type,const XrXirValue *receiver,XrXirValue *output) {
    if(!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if(!view_instance(view)) return XR_XIR_CALL_BAD_STATE;
    return value_call_status(xr_xir_string_copy_bytes(receiver,type,xr_xir_call_admission(view),output));
}
XR_FUNC XrXirCallStatus xr_xir_instance_string_replace(XrXirCallView *view,
    const XrXirValue *receiver,const XrXirValue *search,
    const XrXirValue *replacement,bool all,XrXirValue *output) {
    if(!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if(!view_instance(view)) return XR_XIR_CALL_BAD_STATE;
    return value_call_status(xr_xir_string_replace(receiver,search,replacement,all,xr_xir_call_admission(view),output));
}
