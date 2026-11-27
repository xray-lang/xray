/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_test_plan.h - Private compiler-owned test role inventory
 */
#ifndef XLIR_TEST_PLAN_H
#define XLIR_TEST_PLAN_H

#include "xlir_source.h"
#include "xir/xxir_declarations.h"

typedef struct XrLirTestPlan XrLirTestPlan;

typedef struct XrLirTestPlanRole {
    XrXirTestRole role;
    const char *name;
    size_t name_length;
    XrLirFunction function;
} XrLirTestPlanRole;

/* One verified module owns an initialization-only entry and all enabled root
 * roles. Rows follow source declaration order. Skipped rows have no function
 * handle. Views and handles borrow the plan; no role is executed here. */
XR_FUNC XrLirSourceStatus xr_lir_compile_test_plan(const XrLirSourceRequest *,
    XrLirTestPlan **, XrLirSourceFailure *);
XR_FUNC XrLirFunction xr_lir_test_plan_initializer(const XrLirTestPlan *);
XR_FUNC uint32_t xr_lir_test_plan_role_count(const XrLirTestPlan *);
XR_FUNC XrLirStatus xr_lir_test_plan_role_at(const XrLirTestPlan *, uint32_t ordinal,
    XrLirTestPlanRole *);
/* Exact role and owner check for future callers receiving a function handle. */
XR_FUNC XrLirStatus xr_lir_test_plan_check_role(const XrLirTestPlan *, uint32_t ordinal,
    XrLirFunction);
XR_FUNC void xr_lir_test_plan_free(XrLirTestPlan *);

#endif // XLIR_TEST_PLAN_H
