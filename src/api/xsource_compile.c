/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_compile.c - Shared source admission to an owned Source product
 *
 * KEY CONCEPT:
 *   Project authority and parsing borrow one ledger; only the ordinary Source
 *   owner constructs the resolver, graph, Checked packets and Lowered code.
 */
#include "xsource_compile.h"
#include "xsource_authority.h"
#include "../base/xfileio.h"
#include "../base/xio_policy.inc.h"
#include "../os/os_fs.h"
#include "../xir/xxir_library_catalog.h"
#include "../xir/xxir_stdlib_internal.h"

XR_FUNC XrCompileResourceLimits xr_source_compile_default_resource_limits(void) {
    return (XrCompileResourceLimits){UINT64_C(1) << 30, UINT64_C(256) << 20, UINT64_C(8) << 30};
}
XR_FUNC XrTomlParseLimits xr_source_compile_default_manifest_limits(void) {
    return (XrTomlParseLimits){UINT64_C(1) << 20, 64};
}

static XrSourceCompileStatus source_io_status(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XR_SOURCE_COMPILE_OK;
    case XR_OS_IO_NOT_FOUND: return XR_SOURCE_COMPILE_NOT_FOUND;
    case XR_OS_IO_BAD_ARGUMENT: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    case XR_OS_IO_BUDGET: return XR_SOURCE_COMPILE_BUDGET;
    case XR_OS_IO_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_OS_IO_UNSUPPORTED: return XR_SOURCE_COMPILE_UNSUPPORTED;
    default: return XR_SOURCE_COMPILE_IO;
    }
}
static XrSourceCompileStatus source_manifest_status(XrManifestStatus status) {
    switch (status) {
    case XR_MANIFEST_OK: return XR_SOURCE_COMPILE_OK;
    case XR_MANIFEST_NOT_FOUND: return XR_SOURCE_COMPILE_NOT_FOUND;
    case XR_MANIFEST_INVALID: return XR_SOURCE_COMPILE_INVALID;
    case XR_MANIFEST_BAD_ARGUMENT: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    case XR_MANIFEST_LIMIT: return XR_SOURCE_COMPILE_LIMIT;
    case XR_MANIFEST_BUDGET: return XR_SOURCE_COMPILE_BUDGET;
    case XR_MANIFEST_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_MANIFEST_IO: return XR_SOURCE_COMPILE_IO;
    case XR_MANIFEST_UNSUPPORTED: return XR_SOURCE_COMPILE_UNSUPPORTED;
    default: return XR_SOURCE_COMPILE_INVALID;
    }
}
static XrSourceCompileStatus source_xir_status(XrXirStatus status) {
    switch (status) {
    case XR_XIR_OK: return XR_SOURCE_COMPILE_OK;
    case XR_XIR_BUDGET: return XR_SOURCE_COMPILE_BUDGET;
    case XR_XIR_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_XIR_IO: return XR_SOURCE_COMPILE_IO;
    case XR_XIR_UNRESOLVED: return XR_SOURCE_COMPILE_NOT_FOUND;
    default: return XR_SOURCE_COMPILE_REJECTED;
    }
}
static XrSourceCompileStatus source_session_status(XrCompilerSessionStatus status) {
    switch (status) {
    case XR_COMPILER_SESSION_OK: return XR_SOURCE_COMPILE_OK;
    case XR_COMPILER_SESSION_BUDGET: return XR_SOURCE_COMPILE_BUDGET;
    case XR_COMPILER_SESSION_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    default: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    }
}
static bool source_catalog_matches(const XrSourceCompileRequest *request) {
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
XR_FUNC void xr_source_compile_diagnostic_free(XrSourceCompileDiagnostic *diagnostic) {
    if (!diagnostic) return;
    xr_xir_compile_source_product_diagnostic_free(&diagnostic->source);
    memset(diagnostic, 0, sizeof(*diagnostic));
}
XR_FUNC XrSourceCompileStatus xr_source_compile_build(
    const XrSourceCompileRequest *request, XrXirSourceProduct **output,
    XrSourceCompileDiagnostic *diagnostic) {
    /* Never overwrite a caller's still-owned diagnostic storage. */
    if (diagnostic && (diagnostic->source.snapshot || diagnostic->source.source_path))
        return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileDiagnostic detail = {0};
    XrSourceCompileStatus status = XR_SOURCE_COMPILE_BAD_ARGUMENT;
    if (!request || !request->context || !request->context->resources || !output || *output ||
        !request->absolute_entry_path || !request->absolute_stdlib_path || !source_catalog_matches(request) ||
        (request->session && xr_compile_session_resources(request->session) != request->context->resources))
        goto rejected;
    if (request->target.architecture != XR_XIR_ARCH_X86_64 ||
        request->target.abi_version != XR_XIR_VALUE_ABI_VERSION) {
        status = XR_SOURCE_COMPILE_UNSUPPORTED; goto rejected;
    }
    XrOsIoPolicy policy = xr_compile_io_policy(request->context->resources);
    XrIoContext io = {&policy, XR_OS_IO_OK};
    detail.stage = XR_SOURCE_COMPILE_PATH;
    if (!source_absolute(&io, request->absolute_entry_path) ||
        !source_absolute(&io, request->absolute_stdlib_path)) {
        status = io.status == XR_OS_IO_OK ? XR_SOURCE_COMPILE_BAD_ARGUMENT : source_io_status(io.status);
        goto rejected;
    }
    char *entry = NULL, *stdlib = NULL;
    XrSourceAuthority *authority = NULL;
    XrCompilerSession *session = request->session;
    bool owns_session = !session;
    XrXirSourceProduct *product = NULL;
    XrXirLibraryCatalog *owned_catalog = NULL;
    const XrXirLibraryCatalog *libraries = request->libraries;
    io_status(&io, xr_realpath_owned(&policy, request->absolute_entry_path, &entry));
    if (io.status == XR_OS_IO_OK)
        io_status(&io, xr_realpath_owned(&policy, request->absolute_stdlib_path, &stdlib));
    if (io.status == XR_OS_IO_OK) {
        XrFsStat info = {0};
        io_status(&io, xr_os_io_stat(&policy, stdlib, &info));
        if (io.status == XR_OS_IO_OK && info.kind != XR_FS_DIR) io_status(&io, XR_OS_IO_BAD_ARGUMENT);
    }
    status = source_io_status(io.status);
    if (status == XR_SOURCE_COMPILE_OK && !libraries) {
        detail.stage = XR_SOURCE_COMPILE_LIBRARY;
        status = source_xir_status(xir_stdlib_library_catalog_new(request->context,stdlib,&owned_catalog));
        libraries = owned_catalog;
    }
    if (status == XR_SOURCE_COMPILE_OK) {
        detail.stage = XR_SOURCE_COMPILE_AUTHORITY;
        detail.authority_status = xr_source_authority_open(request->context, entry,
            libraries, &request->manifest_limits, &authority, &detail.authority);
        status = source_manifest_status(detail.authority_status);
    }
    if (status == XR_SOURCE_COMPILE_OK) {
        detail.stage = XR_SOURCE_COMPILE_SESSION;
        if (owns_session) detail.session_status = xr_compile_session_new(request->context->resources, &session);
        else {
            XrCompileResourceStatus state = xr_compile_session_resource_status(session);
            detail.session_status = state == XR_COMPILE_RESOURCE_OK ? XR_COMPILER_SESSION_OK :
                state == XR_COMPILE_RESOURCE_BUDGET ? XR_COMPILER_SESSION_BUDGET :
                state == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_COMPILER_SESSION_OUT_OF_MEMORY : XR_COMPILER_SESSION_BAD_ARGUMENT;
        }
        status = source_session_status(detail.session_status);
    }
    if (status == XR_SOURCE_COMPILE_OK) {
        detail.stage = XR_SOURCE_COMPILE_PRODUCT;
        XrXirSourceProductRequest source = {
            {session, entry, xr_source_authority_entry(authority), request->context,
             stdlib, xr_source_authority_lockfile(authority), XR_XIR_PROGRAM, libraries},
            request->target};
        status = source_xir_status(xr_xir_compile_source_product_build(&source, &product, &detail.source));
        if(detail.source.status==XR_XIR_BAD_STRUCTURE &&
            (detail.source.source.origin==XR_XIR_SOURCE_DIAGNOSTIC_PARSE ||
             detail.source.source.origin==XR_XIR_SOURCE_DIAGNOSTIC_INPUT))status=XR_SOURCE_COMPILE_INVALID;
        if (status == XR_SOURCE_COMPILE_OK) {
            XrXirStatus admitted = xr_xir_compile_source_product_public_admit(product, &detail.source.source);
            status = source_xir_status(admitted);
            if (admitted != XR_XIR_OK) {
                detail.source.stage = XR_XIR_SOURCE_PRODUCT_FACTS;
                detail.source.status = admitted;
                xr_xir_compile_source_product_free(product);
                product = NULL;
            }
        }
    }
    if (owns_session) xr_compile_session_free(session);
    xr_source_authority_close(authority);
    xr_xir_compile_library_catalog_free(owned_catalog);
    xr_compile_resources_free(stdlib);
    xr_compile_resources_free(entry);
    if (status == XR_SOURCE_COMPILE_OK) *output = product;
rejected:
    if (request && request->session &&
        (status == XR_SOURCE_COMPILE_BUDGET || status == XR_SOURCE_COMPILE_OUT_OF_MEMORY)) {
        XrCompileResourceStatus failed = xr_compile_state_fail(xr_compile_session_compile_state(request->session),
            status == XR_SOURCE_COMPILE_BUDGET ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        status = failed == XR_COMPILE_RESOURCE_BUDGET ? XR_SOURCE_COMPILE_BUDGET : XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    }
    detail.status = status;
    if (diagnostic) *diagnostic = detail;
    else xr_source_compile_diagnostic_free(&detail);
    return status;
}
XR_FUNC bool xr_source_compile_diagnostic_format(
    const XrSourceCompileDiagnostic *diagnostic, char *out, size_t size) {
    if (!diagnostic || !out || !size) return false;
    const XrXirSourceDiagnostic *source = &diagnostic->source.source;
    const XrXirSourceView *view = xr_xir_compile_source_snapshot_view(diagnostic->source.snapshot);
    const char *path = diagnostic->source.source_path;
    if (!path && view && view->modules && source->module < view->module_count)
        path = view->modules[source->module].path;
    if (path && path[0] && source->line > 0) {
        snprintf(out, size, "%s:%d:%d: error: %s", path, source->line, source->column,
            source->message[0] ? source->message : xr_source_compile_status_name(diagnostic->status));
        return true;
    }
    int written = snprintf(out, size, "source build failed (stage=%u status=%s)",
        (unsigned)diagnostic->stage, xr_source_compile_status_name(diagnostic->status));
    if (source->message[0] && written > 0 && (size_t)written < size)
        snprintf(out + written, size - (size_t)written, ": %s", source->message);
    return false;
}
XR_FUNC const char *xr_source_compile_status_name(XrSourceCompileStatus status) {
    switch (status) {
    case XR_SOURCE_COMPILE_OK: return "ok";
    case XR_SOURCE_COMPILE_BAD_ARGUMENT: return "bad-argument";
    case XR_SOURCE_COMPILE_NOT_FOUND: return "not-found";
    case XR_SOURCE_COMPILE_INVALID: return "invalid";
    case XR_SOURCE_COMPILE_LIMIT: return "limit";
    case XR_SOURCE_COMPILE_BUDGET: return "budget";
    case XR_SOURCE_COMPILE_OUT_OF_MEMORY: return "out-of-memory";
    case XR_SOURCE_COMPILE_IO: return "io";
    case XR_SOURCE_COMPILE_UNSUPPORTED: return "unsupported";
    case XR_SOURCE_COMPILE_REJECTED: return "source-rejected";
    default: return "invalid-status";
    }
}
