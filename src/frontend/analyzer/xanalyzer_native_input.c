/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_native_input.c - Borrowed native authority without parser state
 */
#include "xanalyzer_native_input.h"

static XrManifestStatus native_resource_status(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_MANIFEST_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_MANIFEST_OVERFLOW :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_MANIFEST_OUT_OF_MEMORY : XR_MANIFEST_BAD_ARGUMENT;
}

XR_FUNC XrCompileResourceStatus xa_analyzer_bind_native_plan(XaAnalyzer *analyzer,
    XrNativePackagePlan *plan) {
    if (!analyzer || !analyzer->compiler_session)
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompileResourceStatus status = xr_compile_session_resource_status(analyzer->compiler_session);
    if (status != XR_COMPILE_RESOURCE_OK)
        return status;
    if (analyzer->native_plan_bound)
        return analyzer->native_plan == plan ? XR_COMPILE_RESOURCE_OK : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (analyzer->semantic_revision > 1)
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrOsIoPolicy policy = xr_compile_io_policy(xr_compile_session_resources(analyzer->compiler_session));
    if (plan && (!plan->valid || !xr_native_package_plan_uses_policy(plan, &policy)))
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    analyzer->native_plan = plan;
    analyzer->native_plan_bound = true;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrManifestStatus xa_analyzer_find_native_symbol(XaAnalyzer *analyzer,
    const char *name, const XrNativeSymbol **output) {
    if (!analyzer || !output)
        return XR_MANIFEST_BAD_ARGUMENT;
    XrCompileResourceStatus available = xr_compile_session_resource_status(analyzer->compiler_session);
    if (available != XR_COMPILE_RESOURCE_OK)
        return native_resource_status(available);
    XrNativePackagePlan *plan = xa_analyzer_native_plan(analyzer);
    if (!plan || !name) {
        *output = NULL;
        return XR_MANIFEST_OK;
    }
    if (!plan->valid) {
        (void) xr_compile_state_fail(xr_compile_session_compile_state(analyzer->compiler_session),
            XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return XR_MANIFEST_INVALID;
    }
    const XrNativeSymbol *symbol = NULL;
    XrManifestStatus status = xr_native_package_find_symbol_owned(plan, name, &symbol);
    if (status != XR_MANIFEST_OK) {
        XrCompileResourceStatus failure = status == XR_MANIFEST_OVERFLOW ? XR_COMPILE_RESOURCE_BUDGET :
            status == XR_MANIFEST_OUT_OF_MEMORY ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
        (void) xr_compile_state_fail(xr_compile_session_compile_state(analyzer->compiler_session), failure);
        return status;
    }
    *output = symbol;
    return XR_MANIFEST_OK;
}

static XrManifestStatus native_operation_plan(XaAnalyzer *analyzer, XrNativePackagePlan **output) {
    if (!analyzer)
        return XR_MANIFEST_BAD_ARGUMENT;
    XrCompileResourceStatus status = xr_compile_session_resource_status(analyzer->compiler_session);
    if (status != XR_COMPILE_RESOURCE_OK)
        return native_resource_status(status);
    *output = xa_analyzer_native_plan(analyzer);
    if (*output && !(*output)->valid) {
        (void) xr_compile_state_fail(xr_compile_session_compile_state(analyzer->compiler_session),
            XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return XR_MANIFEST_BAD_ARGUMENT;
    }
    return XR_MANIFEST_OK;
}

static XrManifestStatus native_operation_result(XaAnalyzer *analyzer, XrManifestStatus status,
    bool semantic_rejection) {
    if (status == XR_MANIFEST_OK || (status == XR_MANIFEST_INVALID && semantic_rejection))
        return status;
    XrCompileResourceStatus failure = status == XR_MANIFEST_OVERFLOW || status == XR_MANIFEST_LIMIT
        ? XR_COMPILE_RESOURCE_BUDGET : status == XR_MANIFEST_OUT_OF_MEMORY
        ? XR_COMPILE_RESOURCE_OUT_OF_MEMORY : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    (void) xr_compile_state_fail(xr_compile_session_compile_state(analyzer->compiler_session), failure);
    return status;
}

XR_FUNC XrManifestStatus xa_analyzer_validate_native_arity(XaAnalyzer *analyzer,
    const char *name, uint32_t arity, XrManifestDiagnostic *diagnostic) {
    XrNativePackagePlan *plan = NULL;
    XrManifestStatus status = native_operation_plan(analyzer, &plan);
    if (status != XR_MANIFEST_OK || !plan)
        return status;
    XrManifestDiagnostic candidate = {0};
    status = native_operation_result(analyzer,
        xr_native_package_validate_symbol_arity_owned(plan, name, arity, &candidate), true);
    if (status == XR_MANIFEST_INVALID && diagnostic && candidate.message[0])
        *diagnostic = candidate;
    return status;
}

XR_FUNC XrManifestStatus xa_analyzer_note_native_layout_subject(XaAnalyzer *analyzer,
    const char *name) {
    XrNativePackagePlan *plan = NULL;
    XrManifestStatus status = native_operation_plan(analyzer, &plan);
    if (status != XR_MANIFEST_OK || !plan)
        return status;
    return native_operation_result(analyzer,
        xr_native_package_note_layout_subject_owned(plan, name), false);
}

XR_FUNC XrManifestStatus xa_analyzer_resolve_native_layout(XaAnalyzer *analyzer,
    const char *name, const struct XrAggregateLayout *layout, bool *matched) {
    XrNativePackagePlan *plan = NULL;
    XrManifestStatus status = native_operation_plan(analyzer, &plan);
    if (status != XR_MANIFEST_OK)
        return status;
    if (!matched)
        return XR_MANIFEST_BAD_ARGUMENT;
    if (!plan) {
        *matched = false;
        return XR_MANIFEST_OK;
    }
    return native_operation_result(analyzer,
        xr_native_package_resolve_layout_owned(plan, name, layout, matched), false);
}
