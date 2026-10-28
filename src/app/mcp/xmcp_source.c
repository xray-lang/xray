/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_source.c - Owned memory source admission for MCP
 *
 * KEY CONCEPT:
 *   Session destruction cannot invalidate the checked query owner. A failed
 *   check never receives executable or complete-query permission.
 */
#include "xmcp_source.h"
#include "../../base/xresource_limits.h"
#include "../../toolchain/xcompiler_session.h"
#include <string.h>

XR_FUNC XrCompileResourceLimits xmcp_source_default_limits(void) {
    return (XrCompileResourceLimits)XR_RESOURCE_SANDBOX_COMPILER_LIMITS;
}

XR_FUNC XrXirStatus xmcp_source_resource_status(XrCompileResourceStatus status) {
    switch (status) {
    case XR_COMPILE_RESOURCE_OK: return XR_XIR_OK;
    case XR_COMPILE_RESOURCE_MEMORY_LIMIT: return XR_XIR_MEMORY_LIMIT;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_XIR_OUT_OF_MEMORY;
    case XR_COMPILE_RESOURCE_OVERFLOW: return XR_XIR_OVERFLOW;
    case XR_COMPILE_RESOURCE_CANCELLED: return XR_XIR_CANCELLED;
    case XR_COMPILE_RESOURCE_DEADLINE: return XR_XIR_DEADLINE;
    case XR_COMPILE_RESOURCE_CLOCK_FAILURE: return XR_XIR_CLOCK_FAILURE;
    default: return XR_XIR_BAD_STRUCTURE;
    }
}

XR_FUNC XrXirStatus xmcp_source_check(const XmcpSourceRequest *request, XmcpSourceOwner *output) {
    if (!request || !output || output->result.checked || output->result.snapshot ||
        !xr_xir_compile_source_failure_empty(&output->failure) || !request->text || !request->module_id ||
        !request->context || !request->context->resources) return XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, request->module_id, NULL};
    if (!xr_module_identity_authority_valid(&authority)) return XR_XIR_BAD_STRUCTURE;
    XrCompilerSession *session = NULL;
    XrCompileResourceStatus created = request->control ?
        xr_compile_request_control_poll(request->control) : XR_COMPILE_RESOURCE_OK;
    if (created == XR_COMPILE_RESOURCE_OK) created = xr_compile_session_new(request->context->resources, &session);
    if (created == XR_COMPILE_RESOURCE_OK && request->control)
        created = xr_compile_session_bind_request(session, request->control);
    XrXirStatus status = xmcp_source_resource_status(created);
    if (status == XR_XIR_OK) {
        XrXirSourceRequest input = {session, NULL, &authority, request->context, NULL, NULL, XR_XIR_PROGRAM, NULL};
        XrXirSourceText text = {NULL, request->text, request->length};
        XrXirSourceDiagnostic immediate={0};
        status = xr_xir_compile_source_check_text(&input, &text, &output->result,&immediate,&output->failure);
        output->diagnostic=(XrXirSourceDiagnosticFact){immediate.status,immediate.module};
    } else output->diagnostic.status = status;
    xr_compile_session_free(session);
    if (status == XR_XIR_OK && request->control) {
        status = xmcp_source_resource_status(xr_compile_request_control_poll(request->control));
        if (status != XR_XIR_OK) { xmcp_source_free(output); output->diagnostic.status = status; }
    }
    return status;
}

XR_FUNC void xmcp_source_free(XmcpSourceOwner *owner) {
    if (!owner) return;
    xr_xir_compile_source_result_free(&owner->result);
    xr_xir_compile_source_failure_free(&owner->failure);
    memset(owner, 0, sizeof(*owner));
}
