/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_state_dispatch.inc.c - Operations on execution-owned module storage
 */
XR_RBC_CASE(MODULE_BEGIN)
XR_RBC_CASE(MODULE_READY) {
    XrXirValueStatus status = op == XR_RBC_MODULE_BEGIN ?
        xrt_module_begin(XR_RBC_SLOT(a).state, instruction >> 16) :
        xrt_module_ready(XR_RBC_SLOT(a).state, instruction >> 16);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
XR_RBC_CASE(STATE_CONSTRUCT)
XR_RBC_CASE(STATE_INITIALIZE)
XR_RBC_CASE(STATE_REFERENCE) {
    XrRbcStateAccess access = function->state_accesses[instruction >> 16];
    XrtModuleState *state = XR_RBC_SLOT(op == XR_RBC_STATE_INITIALIZE ? a : access.state).state;
    XrXirValueStatus status;
    if (op == XR_RBC_STATE_REFERENCE) {
        status = xrt_module_reference(state, access.slot, &XR_RBC_SLOT(a).state_ref);
    } else {
        uint32_t kind = (xr_rbc_state_shape(module, access.slot) & 255u) - 1u;
        bool boolean = false;
        const void *value = &XR_RBC_SLOT(access.value);
        if (kind == XRT_MODULE_UNIT) value = NULL;
        else if (kind == XRT_MODULE_BOOL) {
            if (XR_RBC_REG(access.value) > 1) return XR_RBC_INVALID;
            boolean = XR_RBC_REG(access.value) != 0;
            value = &boolean;
        }
        status = op == XR_RBC_STATE_CONSTRUCT ?
            xrt_module_construct(state, access.slot, value, &XR_RBC_SLOT(a).state_ref) :
            xrt_module_initialize(state, access.slot, value);
    }
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
XR_RBC_CASE(STATE_PUBLISH) {
    XrXirValueStatus status = xrt_module_publish(XR_RBC_SLOT(a).state_ref);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
XR_RBC_CASE(STATE_READ) {
    const void *value = NULL;
    XrXirValueStatus status = xrt_module_borrow(XR_RBC_SLOT(b).state_ref, &value);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, XR_RBC_SOURCE());
    if (c == XRT_MODULE_STRING) {
        XrtString *string;
        memcpy(&string, value, sizeof(string));
        XR_RBC_STRING(a) = xrt_string_retain(string, XR_RBC_SOURCE());
    } else if (c == XRT_MODULE_BOOL) {
        bool boolean;
        memcpy(&boolean, value, sizeof(boolean));
        XR_RBC_REG(a) = boolean;
    } else if (c != XRT_MODULE_UNIT) memcpy(&XR_RBC_REG(a), value, sizeof(uint64_t));
    XR_RBC_NEXT();
}
XR_RBC_CASE(STATE_WRITE) {
    bool boolean = false;
    const void *value = &XR_RBC_SLOT(b);
    if (c == XRT_MODULE_UNIT) value = NULL;
    else if (c == XRT_MODULE_BOOL) {
        if (XR_RBC_REG(b) > 1) return XR_RBC_INVALID;
        boolean = XR_RBC_REG(b) != 0;
        value = &boolean;
    }
    XrXirValueStatus status = xrt_module_replace(XR_RBC_SLOT(a).state_ref, value);
    if (status != XR_XIR_VALUE_OK) xrt_value_failure(status, XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
