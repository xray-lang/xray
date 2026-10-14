/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_current.c - Typed DAP control without a legacy VM fallback
 */
#include "xdap_current.h"
#include "../../xir/xxir_value_internal.h"
#include <string.h>
struct XdapCurrent {
    XrXirDomain *metadata;
    XrXirVmDebug *debug;
    XrXirInstance *instance;
    bool ended, driving;
};
XR_FUNC XrXirValueStatus xdap_current_new(XrXirDomain *metadata,
    XrXirVmDebug **debug, XrXirProgram *program, const XrXirInstanceConfig *config, XdapCurrent **output) {
    if (!output || *output || !debug || !*debug || !program ||
        !xr_xir_domain_current(metadata)) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (!xr_xir_domain_retain(metadata)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueStatus status;
    XdapCurrent *session = xr_xir_domain_metadata_allocate(metadata, sizeof(*session), &status);
    if (!session) { xr_xir_domain_drop(metadata); return status; }
    XrXirInstance *instance = NULL;
    XrXirCallStatus created = xr_xir_vm_debug_instance_new(*debug, program, config, &instance);
    if (created != XR_XIR_CALL_READY) {
        xr_xir_domain_metadata_deallocate(metadata, session, sizeof(*session));
        xr_xir_domain_drop(metadata);
        return created == XR_XIR_CALL_OOM ? XR_XIR_VALUE_OOM :
            created == XR_XIR_CALL_LIMIT ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
    }
    *session = (XdapCurrent){metadata, *debug, instance, false, false};
    *debug = NULL;
    *output = session;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirCallStatus xdap_current_start(XdapCurrent *session, uint32_t entry,
    const XrXirValue *arguments, uint32_t count) {
    if (session && session->driving) return XR_XIR_CALL_BUSY;
    return session && !session->ended ? xr_xir_instance_start(session->instance, entry, arguments, count) :
        XR_XIR_CALL_BAD_STATE;
}
XR_FUNC void xdap_current_result_drop(XdapCurrentResult *result) {
    if (!result) return;
    xr_xir_call_result_drop(&result->outcome);
    *result = (XdapCurrentResult){0};
}
XR_FUNC XrXirValueStatus xdap_current_poll(XdapCurrent *session, uint64_t quantum,
    XdapCurrentResult *output) {
    if (!session || session->driving || !output || output->state || output->debug_epoch || output->instance_epoch ||
        !xr_xir_call_result_empty(&output->outcome) || !quantum) return XR_XIR_VALUE_BAD_ARGUMENT;
    if (xr_xir_vm_debug_paused(session->debug)) {
        *output = (XdapCurrentResult){.state=XDAP_CURRENT_STOPPED,
            .debug_epoch=xr_xir_vm_debug_epoch(session->debug)};
        return XR_XIR_VALUE_OK;
    }
    XrXirInstanceResult result = {0};
    for (uint64_t i = 0; i < quantum; ++i) {
        XrXirValueStatus observed=xr_xir_vm_debug_observation_status(session->debug,false);
        if (observed!=XR_XIR_VALUE_OK) return observed;
        session->driving = true;
        result = xr_xir_instance_poll_bounded(session->instance, 1);
        session->driving = false;
        observed=xr_xir_vm_debug_observation_status(session->debug,result.outcome.status==XR_XIR_CALL_RETURNED);
        /* The target's admitted failure and cleanup remain authoritative. */
        if (observed!=XR_XIR_VALUE_OK && (result.outcome.status==XR_XIR_CALL_READY ||
            result.outcome.status==XR_XIR_CALL_RETURNED)) return observed;
        if (xr_xir_vm_debug_paused(session->debug)) {
            *output = (XdapCurrentResult){.state=XDAP_CURRENT_STOPPED,
                .debug_epoch=xr_xir_vm_debug_epoch(session->debug), .instance_epoch=result.epoch};
            return XR_XIR_VALUE_OK;
        }
        if (result.outcome.status != XR_XIR_CALL_READY) break;
    }
    XdapCurrentResult owned = {.instance_epoch=result.epoch};
    XrXirValueStatus status = xr_xir_call_result_copy(&result.outcome, &owned.outcome);
    if (status != XR_XIR_VALUE_OK) return status;
    if (result.outcome.status == XR_XIR_CALL_SUSPENDED) owned.state = XDAP_CURRENT_WAITING;
    else if (result.outcome.status != XR_XIR_CALL_READY) owned.state = XDAP_CURRENT_ENDED;
    *output = owned;
    return XR_XIR_VALUE_OK;
}
XR_FUNC XrXirValueStatus xdap_current_continue(XdapCurrent *session, uint64_t epoch,
    XrXirVmDebugMode mode) {
    return session && !session->driving && !session->ended ? xr_xir_vm_debug_continue(session->debug, epoch, mode) :
        XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC XrXirCallStatus xdap_current_wait_request(XdapCurrent *session,uint64_t epoch,
    uint64_t wake,XrXirWaitRequest *output) {
    if (!session) return XR_XIR_CALL_BAD_ARGUMENT;
    if (session->driving) return XR_XIR_CALL_BUSY;
    return !session->ended ? xr_xir_instance_wait_request(session->instance,epoch,wake,output) : XR_XIR_CALL_BAD_STATE;
}
XR_FUNC XrXirCallStatus xdap_current_resume(XdapCurrent *session,uint64_t epoch,uint64_t wake) {
    if (!session) return XR_XIR_CALL_BAD_ARGUMENT;
    if (session->driving) return XR_XIR_CALL_BUSY;
    return !session->ended ? xr_xir_instance_resume(session->instance,epoch,wake) : XR_XIR_CALL_BAD_STATE;
}
XR_FUNC void xdap_current_pause(XdapCurrent *session) {
    if (session && !session->ended) xr_xir_vm_debug_request_pause(session->debug);
}
XR_FUNC XrXirValueStatus xdap_current_breakpoint(XdapCurrent *session,
    uint32_t function, uint32_t instruction, bool enabled) {
    return session && !session->driving && !session->ended ? xr_xir_vm_debug_breakpoint(session->debug, function, instruction, enabled) :
        XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC XrXirValueStatus xdap_current_breakpoints_replace(XdapCurrent *session,
    const XrXirVmDebugLocation *locations,uint32_t count) {
    return session && !session->driving && !session->ended ?
        xr_xir_vm_debug_breakpoints_replace(session->debug,locations,count) : XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC XrXirValueStatus xdap_current_snapshot(XdapCurrent *session,
    XrXirVmDebugSnapshot **output) {
    return session && !session->driving ? xr_xir_vm_debug_snapshot(session->debug, output) : XR_XIR_VALUE_BAD_ARGUMENT;
}
XR_FUNC XrXirCallStatus xdap_current_cancel(XdapCurrent *session) {
    if (!session) return XR_XIR_CALL_BAD_ARGUMENT;
    if (session->driving) return XR_XIR_CALL_BUSY;
    XrXirCallStatus status = xr_xir_instance_cancel_current(session->instance);
    if (status == XR_XIR_CALL_CANCEL_REQUESTED) {
        xr_xir_vm_debug_disable(session->debug);
        session->ended = true;
    }
    return status;
}
XR_FUNC XrXirCallStatus xdap_current_free(XdapCurrent *session) {
    if (!session) return XR_XIR_CALL_READY;
    if (session->driving) return XR_XIR_CALL_BUSY;
    xr_xir_vm_debug_disable(session->debug);
    XrXirCallStatus status = xr_xir_instance_free(session->instance);
    if (status == XR_XIR_CALL_BUSY) return status;
    xr_xir_vm_debug_drop(session->debug);
    XrXirDomain *metadata = session->metadata;
    xr_xir_domain_metadata_deallocate(metadata, session, sizeof(*session));
    xr_xir_domain_drop(metadata);
    return status;
}
XR_FUNC XrXirCallStatus xdap_current_stop(XdapCurrent *session) {
    if (!session) return XR_XIR_CALL_BAD_ARGUMENT;
    if (session->driving) return XR_XIR_CALL_BUSY;
    xr_xir_vm_debug_disable(session->debug);session->driving=true;
    XrXirCallStatus status=xr_xir_instance_stop(session->instance);session->driving=false;
    if (status!=XR_XIR_CALL_BUSY) session->ended=true;
    return status;
}
