/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#ifndef TEST_ROLE_SESSION_SELECTION_H
#define TEST_ROLE_SESSION_SELECTION_H
#include <stdlib.h>
#include <string.h>

#if !defined(TEST_ROLE_SESSION_HOST_ONLY)
#include "lir/xlir_test_plan.h"

static inline bool select_session_actions(const XrLirTestPlan *plan, const char *name,
    XrLirFunction **output, uint32_t *count) {
    uint32_t roles = xr_lir_test_plan_role_count(plan);
    uint32_t selected = UINT32_MAX;
    for (uint32_t i = 0; i < roles; ++i) {
        XrLirTestPlanRole role;
        if (xr_lir_test_plan_role_at(plan, i, &role) != XR_LIR_OK) return false;
        if (role.role != XR_XIR_TEST_ROLE_TEST || strcmp(role.name, name)) continue;
        if (selected != UINT32_MAX) return false;
        selected = i;
    }
    if (selected == UINT32_MAX || sizeof(XrLirFunction) > SIZE_MAX / roles) return false;
    XrLirFunction *functions = calloc(roles, sizeof(*functions));
    if (!functions) return false;
    uint32_t used = 0;
    const XrXirTestRole order[] = {
        XR_XIR_TEST_ROLE_BEFORE_EACH, XR_XIR_TEST_ROLE_TEST, XR_XIR_TEST_ROLE_AFTER_EACH};
    for (size_t phase = 0; phase < sizeof(order) / sizeof(order[0]); ++phase) {
        for (uint32_t i = 0; i < roles; ++i) {
            XrLirTestPlanRole role;
            if (xr_lir_test_plan_role_at(plan, i, &role) != XR_LIR_OK) goto reject;
            if (role.role != order[phase] || (role.role == XR_XIR_TEST_ROLE_TEST && i != selected))
                continue;
            if (xr_lir_test_plan_check_role(plan, i, role.function) != XR_LIR_OK) goto reject;
            functions[used++] = role.function;
        }
    }
    *output = functions;
    *count = used;
    return true;
reject:
    free(functions);
    return false;
}
#endif

#if defined(TEST_ROLE_SESSION_CLOSE_CONTROL)
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_values.h"
#include "os/os_file_metadata.h"

typedef enum SessionRequestMode {
    SESSION_REQUEST_NONE, SESSION_REQUEST_ALLOW, SESSION_REQUEST_CANCEL
} SessionRequestMode;

static inline SessionRequestMode session_request_mode(const char *argument) {
    if (!strcmp(argument, "--allow-action")) return SESSION_REQUEST_ALLOW;
    if (!strcmp(argument, "--cancel-action")) return SESSION_REQUEST_CANCEL;
    return SESSION_REQUEST_NONE;
}

typedef struct SessionRequestProbe {
    const XrtHostExecutionControl *control;
    unsigned polls;
    bool allow;
} SessionRequestProbe;

static inline bool session_request_current(void *context) {
    SessionRequestProbe *probe = context;
    if (!xrt_value_root_active() || xrt_execution_host_control() != probe->control) abort();
    ++probe->polls;
    return probe->allow;
}

typedef struct SessionCloseControl {
    XrtBillingOwner *billing;
    XrOsFileMetadataOwner *metadata;
    unsigned attempts;
} SessionCloseControl;

static SessionCloseControl *session_close_control;

static inline void session_close_retire(void *context) {
    SessionCloseControl *control = context;
    if (control != session_close_control || control->attempts ||
        xrt_native_values_stat_prepare(xrt_value_root_active(), &control->billing, &control->metadata) !=
            XRT_MANAGED_OK ||
        !control->metadata || control->metadata->handle || !xrt_billing_owner_retain(control->billing))
        abort();
    /* Only the injected closer accepts this token. It is never sent to the OS. */
    control->metadata->handle = 17;
}

static inline XrOsIoStatus session_close_attempt(XrOsFileMetadataOwner *metadata) {
    SessionCloseControl *control = session_close_control;
    if (!control || metadata != control->metadata || metadata->handle != 17) abort();
    if (++control->attempts < 3) return XR_OS_IO_IO;
    metadata->handle = 0;
    return XR_OS_IO_OK;
}

static inline bool session_close_finish(SessionCloseControl *control) {
    session_close_control = NULL;
    bool retired = control->attempts == 3 && control->billing &&
        xrt_billing_owner_stats(control->billing).blocks == 1;
    if (control->billing) xrt_billing_owner_drop(control->billing);
    return retired;
}
#endif
#endif // TEST_ROLE_SESSION_SELECTION_H
