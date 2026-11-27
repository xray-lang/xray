/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
#ifndef TEST_ROLE_SESSION_SELECTION_H
#define TEST_ROLE_SESSION_SELECTION_H
#include "lir/xlir_test_plan.h"
#include <stdlib.h>
#include <string.h>

static bool select_session_actions(const XrLirTestPlan *plan, const char *name,
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
#endif // TEST_ROLE_SESSION_SELECTION_H
