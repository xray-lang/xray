/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_canonical_source.c - Shared CLI admission to an owned Source product
 *
 * KEY CONCEPT:
 *   Project authority and parsing borrow one ledger; only the ordinary Source
 *   owner constructs the resolver, graph, Checked packets and Lowered code.
 */
#include "xcli_canonical_source.h"
#include "xcli_graph_authority.h"
#include "../../base/xfileio.h"
#include "../../base/xio_policy.inc.h"
#include "../../os/os_fs.h"
#include "../../xir/xxir_library_catalog.h"

XR_FUNC XrCompileResourceLimits xr_cli_compile_default_resource_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(1) << 30, UINT64_C(256) << 20, UINT64_C(8) << 30};
}
XR_FUNC XrTomlParseLimits xr_cli_compile_default_manifest_limits(void) {
    return (XrTomlParseLimits){UINT64_C(1) << 20, 64};
}

static XrCliCompileSourceStatus source_io_status(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XR_CLI_COMPILE_SOURCE_OK;
    case XR_OS_IO_NOT_FOUND: return XR_CLI_COMPILE_SOURCE_NOT_FOUND;
    case XR_OS_IO_BAD_ARGUMENT: return XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT;
    case XR_OS_IO_BUDGET: return XR_CLI_COMPILE_SOURCE_BUDGET;
    case XR_OS_IO_OUT_OF_MEMORY: return XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    case XR_OS_IO_UNSUPPORTED: return XR_CLI_COMPILE_SOURCE_UNSUPPORTED;
    default: return XR_CLI_COMPILE_SOURCE_IO;
    }
}
static XrCliCompileSourceStatus source_manifest_status(XrManifestStatus status) {
    switch (status) {
    case XR_MANIFEST_OK: return XR_CLI_COMPILE_SOURCE_OK;
    case XR_MANIFEST_NOT_FOUND: return XR_CLI_COMPILE_SOURCE_NOT_FOUND;
    case XR_MANIFEST_INVALID: return XR_CLI_COMPILE_SOURCE_INVALID;
    case XR_MANIFEST_BAD_ARGUMENT: return XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT;
    case XR_MANIFEST_LIMIT: return XR_CLI_COMPILE_SOURCE_LIMIT;
    case XR_MANIFEST_BUDGET: return XR_CLI_COMPILE_SOURCE_BUDGET;
    case XR_MANIFEST_OUT_OF_MEMORY: return XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    case XR_MANIFEST_IO: return XR_CLI_COMPILE_SOURCE_IO;
    case XR_MANIFEST_UNSUPPORTED: return XR_CLI_COMPILE_SOURCE_UNSUPPORTED;
    default: return XR_CLI_COMPILE_SOURCE_INVALID;
    }
}
static XrCliCompileSourceStatus source_xir_status(XrXirStatus status) {
    switch (status) {
    case XR_XIR_OK: return XR_CLI_COMPILE_SOURCE_OK;
    case XR_XIR_BUDGET: return XR_CLI_COMPILE_SOURCE_BUDGET;
    case XR_XIR_OUT_OF_MEMORY: return XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    case XR_XIR_IO: return XR_CLI_COMPILE_SOURCE_IO;
    case XR_XIR_UNRESOLVED: return XR_CLI_COMPILE_SOURCE_NOT_FOUND;
    default: return XR_CLI_COMPILE_SOURCE_REJECTED;
    }
}
static XrCliCompileSourceStatus source_session_status(XrCompilerSessionStatus status) {
    switch (status) {
    case XR_COMPILER_SESSION_OK: return XR_CLI_COMPILE_SOURCE_OK;
    case XR_COMPILER_SESSION_BUDGET: return XR_CLI_COMPILE_SOURCE_BUDGET;
    case XR_COMPILER_SESSION_OUT_OF_MEMORY: return XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    default: return XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT;
    }
}
static bool source_catalog_matches(const XrCliCompileSourceRequest *request) {
    if (!request->libraries) return true;
    const XrXirCompileContext *catalog = xr_xir_compile_library_catalog_context(request->libraries);
    const XrXirCompileContext *context = request->context;
    return catalog && catalog->resources == context->resources &&
        catalog->limits.functions == context->limits.functions &&
        catalog->limits.parameters == context->limits.parameters &&
        catalog->limits.blocks == context->limits.blocks &&
        catalog->limits.instructions == context->limits.instructions &&
        catalog->limits.frame_bytes == context->limits.frame_bytes;
}
static bool source_absolute(XrIoContext *io, const char *path) {
    unsigned char prefix[3] = {0};
    for (size_t i = 0; i < sizeof(prefix); ++i) {
        if (!io_work(io, 1)) return false;
        prefix[i] = (unsigned char)path[i];
        if (!prefix[i]) break;
    }
#ifdef XR_OS_WINDOWS
    bool separator0 = prefix[0] == '/' || prefix[0] == '\\';
    bool separator1 = prefix[1] == '/' || prefix[1] == '\\';
    bool letter = (prefix[0] >= 'A' && prefix[0] <= 'Z') ||
                  (prefix[0] >= 'a' && prefix[0] <= 'z');
    return (separator0 && separator1) ||
        (letter && prefix[1] == ':' && (prefix[2] == '/' || prefix[2] == '\\'));
#else
    return prefix[0] == '/';
#endif
}
XR_FUNC void xr_cli_compile_source_diagnostic_free(XrCliCompileSourceDiagnostic *diagnostic) {
    if (!diagnostic) return;
    xr_xir_compile_source_product_diagnostic_free(&diagnostic->source);
    memset(diagnostic, 0, sizeof(*diagnostic));
}
XR_FUNC XrCliCompileSourceStatus xr_cli_compile_source_build(
    const XrCliCompileSourceRequest *request, XrXirSourceProduct **output,
    XrCliCompileSourceDiagnostic *diagnostic) {
    /* Never overwrite a caller's still-owned diagnostic storage. */
    if (diagnostic && (diagnostic->source.snapshot || diagnostic->source.source_path))
        return XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT;
    XrCliCompileSourceDiagnostic detail = {0};
    XrCliCompileSourceStatus status = XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT;
    if (!request || !request->context || !request->context->resources || !output || *output ||
        !request->absolute_entry_path || !request->absolute_stdlib_path || !source_catalog_matches(request))
        goto rejected;
    if (request->target.architecture != XR_XIR_ARCH_X86_64 ||
        request->target.abi_version != XR_XIR_VALUE_ABI_VERSION) {
        status = XR_CLI_COMPILE_SOURCE_UNSUPPORTED; goto rejected;
    }
    XrOsIoPolicy policy = xr_compile_io_policy(request->context->resources);
    XrIoContext io = {&policy, XR_OS_IO_OK};
    detail.stage = XR_CLI_COMPILE_SOURCE_PATH;
    if (!source_absolute(&io, request->absolute_entry_path) ||
        !source_absolute(&io, request->absolute_stdlib_path)) {
        status = io.status == XR_OS_IO_OK ? XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT : source_io_status(io.status);
        goto rejected;
    }
    char *entry = NULL, *stdlib = NULL;
    XrCliGraphAuthority *authority = NULL;
    XrCompilerSession *session = NULL;
    XrXirSourceProduct *product = NULL;
    io_status(&io, xr_realpath_owned(&policy, request->absolute_entry_path, &entry));
    if (io.status == XR_OS_IO_OK)
        io_status(&io, xr_realpath_owned(&policy, request->absolute_stdlib_path, &stdlib));
    if (io.status == XR_OS_IO_OK) {
        XrFsStat info = {0};
        io_status(&io, xr_os_io_stat(&policy, stdlib, &info));
        if (io.status == XR_OS_IO_OK && info.kind != XR_FS_DIR) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
    }
    status = source_io_status(io.status);
    if (status == XR_CLI_COMPILE_SOURCE_OK) {
        detail.stage = XR_CLI_COMPILE_SOURCE_AUTHORITY;
        detail.authority_status = xr_cli_compile_graph_authority_open(request->context, entry,
            request->libraries, &request->manifest_limits, &authority, &detail.authority);
        status = source_manifest_status(detail.authority_status);
    }
    if (status == XR_CLI_COMPILE_SOURCE_OK) {
        detail.stage = XR_CLI_COMPILE_SOURCE_SESSION;
        detail.session_status = xr_compile_session_new(request->context->resources, &session);
        status = source_session_status(detail.session_status);
    }
    if (status == XR_CLI_COMPILE_SOURCE_OK) {
        detail.stage = XR_CLI_COMPILE_SOURCE_PRODUCT;
        XrXirSourceProductRequest source = {
            {session, entry, xr_cli_compile_graph_authority_entry(authority), request->context,
             stdlib, xr_cli_compile_graph_authority_lockfile(authority), XR_XIR_PROGRAM, request->libraries},
            request->target};
        status = source_xir_status(xr_xir_compile_source_product_build(&source, &product, &detail.source));
    }
    xr_compile_session_free(session);
    xr_cli_compile_graph_authority_close(authority);
    xr_compile_resources_free(stdlib);
    xr_compile_resources_free(entry);
    if (status == XR_CLI_COMPILE_SOURCE_OK) *output = product;
rejected:
    detail.status = status;
    if (diagnostic) *diagnostic = detail;
    else xr_cli_compile_source_diagnostic_free(&detail);
    return status;
}
XR_FUNC const char *xr_cli_compile_source_status_name(XrCliCompileSourceStatus status) {
    switch (status) {
    case XR_CLI_COMPILE_SOURCE_OK: return "ok";
    case XR_CLI_COMPILE_SOURCE_BAD_ARGUMENT: return "bad-argument";
    case XR_CLI_COMPILE_SOURCE_NOT_FOUND: return "not-found";
    case XR_CLI_COMPILE_SOURCE_INVALID: return "invalid";
    case XR_CLI_COMPILE_SOURCE_LIMIT: return "limit";
    case XR_CLI_COMPILE_SOURCE_BUDGET: return "budget";
    case XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY: return "out-of-memory";
    case XR_CLI_COMPILE_SOURCE_IO: return "io";
    case XR_CLI_COMPILE_SOURCE_UNSUPPORTED: return "unsupported";
    case XR_CLI_COMPILE_SOURCE_REJECTED: return "source-rejected";
    default: return "invalid-status";
    }
}
