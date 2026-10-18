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
#include "../../toolchain/xcompiler_session.h"
#include <string.h>

XR_FUNC XrCompileResourceLimits xmcp_source_default_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(1073741824), UINT64_C(268435456), UINT64_C(8589934592)};
}

XR_FUNC XrXirStatus xmcp_source_check(const XmcpSourceRequest *request, XmcpSourceOwner *output) {
    if (!request || !output || output->result.checked || output->result.snapshot ||
        !xr_xir_compile_source_failure_empty(&output->failure) || !request->text || !request->module_id ||
        !request->context || !request->context->resources) return XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, request->module_id, NULL};
    if (!xr_module_identity_authority_valid(&authority)) return XR_XIR_BAD_STRUCTURE;
    XrCompilerSession *session = NULL;
    XrCompilerSessionStatus created = xr_compile_session_new(request->context->resources, &session);
    XrXirStatus status = created == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET :
        created == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
    if (status == XR_XIR_OK) {
        XrXirSourceRequest input = {session, NULL, &authority, request->context, NULL, NULL, XR_XIR_PROGRAM, NULL};
        XrXirSourceText text = {NULL, request->text, request->length};
        status = xr_xir_compile_source_check_text(&input, &text, &output->result,
            &output->diagnostic, &output->failure);
    } else output->diagnostic.status = status;
    xr_compile_session_free(session);
    return status;
}

XR_FUNC void xmcp_source_free(XmcpSourceOwner *owner) {
    if (!owner) return;
    xr_xir_compile_source_result_free(&owner->result);
    xr_xir_compile_source_failure_free(&owner->failure);
    memset(owner, 0, sizeof(*owner));
}
