/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_current.h - Current Program/Instance debug control
 */
#ifndef XDAP_CURRENT_H
#define XDAP_CURRENT_H
#include "../../xir/xxir_vm_debug.h"
typedef struct XdapCurrent XdapCurrent;
typedef enum XdapCurrentState {
    XDAP_CURRENT_RUNNING, XDAP_CURRENT_STOPPED, XDAP_CURRENT_WAITING, XDAP_CURRENT_ENDED
} XdapCurrentState;
typedef struct XdapCurrentResult {
    XdapCurrentState state;
    uint64_t debug_epoch, instance_epoch;
    XrXirCallResult outcome;
} XdapCurrentResult;
/* Success takes the observer and constructs its authenticated target Instance.
 * The caller keeps its Program reference. Failure preserves debug and output. */
XR_FUNC XrXirValueStatus xdap_current_new(XrXirDomain *metadata,
    XrXirVmDebug **debug, XrXirProgram *program, const XrXirInstanceConfig *config, XdapCurrent **output);
XR_FUNC XrXirCallStatus xdap_current_start(XdapCurrent *session, uint32_t entry,
    const XrXirValue *arguments, uint32_t count);
/* Every actual VM transition uses the existing one-quantum driver. A stopped
 * observer causes no further polls or target work until explicit continue. */
XR_FUNC XrXirValueStatus xdap_current_poll(XdapCurrent *session, uint64_t quantum,
    XdapCurrentResult *output);
/* Only an actual pending Instance suspension grants a host wait request.
 * Epoch and wake are rechecked on resume; a debugger stop grants neither. */
XR_FUNC XrXirCallStatus xdap_current_wait_request(XdapCurrent *,uint64_t epoch,
    uint64_t wake,XrXirWaitRequest *);
XR_FUNC XrXirCallStatus xdap_current_resume(XdapCurrent *,uint64_t epoch,uint64_t wake);
XR_FUNC XrXirValueStatus xdap_current_continue(XdapCurrent *session, uint64_t epoch,
    XrXirVmDebugMode mode);
XR_FUNC XrXirValueStatus xdap_current_continue_filtered(XdapCurrent *,uint64_t epoch);
XR_FUNC XrXirValueStatus xdap_current_write_begin(XdapCurrent *,uint64_t,uint64_t,uint32_t,const XrXirValue *);
XR_FUNC XrXirValueStatus xdap_current_write_finish(XdapCurrent *,bool *,XrXirVmDebugSnapshot **);
XR_FUNC void xdap_current_pause(XdapCurrent *session);
XR_FUNC XrXirValueStatus xdap_current_breakpoint(XdapCurrent *session,
    uint32_t function, uint32_t instruction, bool enabled);
XR_FUNC XrXirValueStatus xdap_current_breakpoints_replace(XdapCurrent *,
    const XrXirVmDebugLocation *,uint32_t count);
XR_FUNC XrXirValueStatus xdap_current_rules_replace(XdapCurrent *,const XrXirVmDebugRule *,uint32_t count);
XR_FUNC XrXirValueStatus xdap_current_break_on_throw(XdapCurrent *,bool enabled);
XR_FUNC XrXirValueStatus xdap_current_snapshot(XdapCurrent *session,
    XrXirVmDebugSnapshot **output);
/* Actual live activation identities between polls. A not-yet-polled admitted
 * root is known directly; after polling, incomplete observer ingress rejects
 * instead of presenting a partial inventory. Outputs survive failure. */
XR_FUNC XrXirValueStatus xdap_current_threads(XdapCurrent *,uint64_t *,uint32_t,uint32_t *);
XR_FUNC XrXirCallStatus xdap_current_cancel(XdapCurrent *session);
/* Stop and drain the whole target, including child activations. The ordinary
 * Instance stop owns cleanup and its exact failure status is preserved. */
XR_FUNC XrXirCallStatus xdap_current_stop(XdapCurrent *session);
/* Consumes the session unless the actual Instance reports BUSY. */
XR_FUNC XrXirCallStatus xdap_current_free(XdapCurrent *session);
XR_FUNC void xdap_current_result_drop(XdapCurrentResult *result);
#endif // XDAP_CURRENT_H
