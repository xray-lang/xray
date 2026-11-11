/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_state_dispatch.inc.c - Dispatch into the prepared module access leaves
 */
XR_RBC_CASE(MODULE_BEGIN)
XR_RBC_CASE(MODULE_READY)
XR_RBC_CASE(STATE_CONSTRUCT)
XR_RBC_CASE(STATE_INITIALIZE)
XR_RBC_CASE(STATE_REFERENCE)
XR_RBC_CASE(STATE_PUBLISH)
XR_RBC_CASE(STATE_READ)
XR_RBC_CASE(STATE_WRITE) {
    XrRbcStatus status = xr_rbc_state_step(function, registers, stack->module_accesses,
        instruction, XR_RBC_SOURCE());
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
