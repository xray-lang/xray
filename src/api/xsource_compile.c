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
#include "xsource_paths.h"
#include "../base/xresource_limits.h"
#include "../base/xfileio.h"
#include "../base/xio_policy.inc.h"
#include "../os/os_fs.h"
#include "../xir/xxir_library_catalog.h"
#include "../xir/xxir_stdlib_internal.h"
#include <stdarg.h>
#include <stdio.h>

XR_FUNC XrCompileResourceLimits xr_source_compile_default_resource_limits(void) {
    return (XrCompileResourceLimits)XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
}
XR_FUNC XrTomlParseLimits xr_source_compile_default_manifest_limits(void) {
    return (XrTomlParseLimits){UINT64_C(1) << 20, 64};
}

static XrSourceCompileStatus source_resource_status(XrCompileResourceStatus status) {
    switch(status) {
    case XR_COMPILE_RESOURCE_OK:return XR_SOURCE_COMPILE_OK;
    case XR_COMPILE_RESOURCE_MEMORY_LIMIT:return XR_SOURCE_COMPILE_MEMORY_LIMIT;
    case XR_COMPILE_RESOURCE_OVERFLOW:return XR_SOURCE_COMPILE_OVERFLOW;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY:return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_COMPILE_RESOURCE_CANCELLED:return XR_SOURCE_COMPILE_CANCELLED;
    case XR_COMPILE_RESOURCE_DEADLINE:return XR_SOURCE_COMPILE_DEADLINE;
    case XR_COMPILE_RESOURCE_CLOCK_FAILURE:return XR_SOURCE_COMPILE_CLOCK_FAILURE;
    default:return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    }
}
static XrSourceCompileStatus source_io_status(XrOsIoStatus status) {
    switch (status) {
    case XR_OS_IO_OK: return XR_SOURCE_COMPILE_OK;
    case XR_OS_IO_NOT_FOUND: return XR_SOURCE_COMPILE_NOT_FOUND;
    case XR_OS_IO_BAD_ARGUMENT: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    case XR_OS_IO_OVERFLOW: return XR_SOURCE_COMPILE_OVERFLOW;
    case XR_OS_IO_MEMORY_LIMIT: return XR_SOURCE_COMPILE_MEMORY_LIMIT;
    case XR_OS_IO_CANCELLED: return XR_SOURCE_COMPILE_CANCELLED;
    case XR_OS_IO_DEADLINE: return XR_SOURCE_COMPILE_DEADLINE;
    case XR_OS_IO_CLOCK_FAILURE: return XR_SOURCE_COMPILE_CLOCK_FAILURE;
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
    case XR_MANIFEST_OVERFLOW: return XR_SOURCE_COMPILE_OVERFLOW;
    case XR_MANIFEST_MEMORY_LIMIT: return XR_SOURCE_COMPILE_MEMORY_LIMIT;
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
    case XR_XIR_MEMORY_LIMIT: return XR_SOURCE_COMPILE_MEMORY_LIMIT;
    case XR_XIR_OVERFLOW: return XR_SOURCE_COMPILE_OVERFLOW;
    case XR_XIR_CANCELLED: return XR_SOURCE_COMPILE_CANCELLED;
    case XR_XIR_DEADLINE: return XR_SOURCE_COMPILE_DEADLINE;
    case XR_XIR_CLOCK_FAILURE: return XR_SOURCE_COMPILE_CLOCK_FAILURE;
    case XR_XIR_IO: return XR_SOURCE_COMPILE_IO;
    case XR_XIR_UNRESOLVED: return XR_SOURCE_COMPILE_NOT_FOUND;
    default: return XR_SOURCE_COMPILE_REJECTED;
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
        if (!((io)->status == XR_OS_IO_OK)) return false;
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
XR_FUNC XrCompileResourceStatus xr_source_compile_manifest_report(XrSourceCompileDiagnostic *detail,
    XrCompileResources *resources,const XrManifestDiagnostic *fact) {
    if(!detail||!fact)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if(detail->source.failure.report||!fact->message[0])return XR_COMPILE_RESOURCE_OK;
    XrFrontendReport *report=NULL;uint32_t source=0;
    XrCompileResourceStatus status=xr_frontend_report_new(resources,&report);
    if(status==XR_COMPILE_RESOURCE_OK)status=xr_frontend_report_source(report,NULL,NULL,0,NULL,&source);
    if(status==XR_COMPILE_RESOURCE_OK)status=xr_frontend_report_add(report,0,fact->message,
        xr_frontend_report_location(report,source,0,0,NULL),NULL,0);
    if(status!=XR_COMPILE_RESOURCE_OK){xr_frontend_report_free(report);return status;}
    xr_frontend_report_seal(report,false);detail->source.failure.report=report;return status;
}
XR_FUNC XrSourceCompileStatus xr_source_compile_build(
    const XrSourceCompileRequest *request, XrXirSourceProduct **output,
    XrSourceCompileDiagnostic *diagnostic) {
    /* Never overwrite a caller's still-owned diagnostic storage. */
    if (diagnostic && (diagnostic->source.snapshot || !xr_xir_compile_source_failure_empty(&diagnostic->source.failure)))
        return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileDiagnostic detail = {0};XrManifestDiagnostic authority_fact={0};
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
            libraries, &request->manifest_limits, &authority, &authority_fact);
        (void)xr_source_compile_manifest_report(&detail,request->context->resources,&authority_fact);
        status = source_manifest_status(detail.authority_status);
    }
    if (status == XR_SOURCE_COMPILE_OK) {
        detail.stage = XR_SOURCE_COMPILE_SESSION;
        detail.session_status=owns_session ? xr_compile_session_new(request->context->resources,&session) :
            xr_compile_session_resource_status(session);
        status=source_resource_status(detail.session_status);
    }
    if (status == XR_SOURCE_COMPILE_OK) {
        detail.stage = XR_SOURCE_COMPILE_PRODUCT;
        XrXirSourceProductRequest source = {
            {session, entry, xr_source_authority_entry(authority), request->context,
             stdlib, xr_source_authority_lockfile(authority), XR_XIR_PROGRAM, libraries},
            request->target};
        status = source_xir_status(xr_xir_compile_source_product_build(&source, &product, &detail.source));
        if(detail.source.status==XR_XIR_BAD_STRUCTURE &&
            (detail.source.failure.origin==XR_XIR_SOURCE_DIAGNOSTIC_PARSE ||
             detail.source.failure.origin==XR_XIR_SOURCE_DIAGNOSTIC_INPUT))status=XR_SOURCE_COMPILE_INVALID;
        if (status == XR_SOURCE_COMPILE_OK) {
            XrXirSourceDiagnostic immediate={0};
            XrXirStatus admitted = xr_xir_compile_source_product_public_admit(product, &immediate);
            detail.source.source=(XrXirSourceDiagnosticFact){immediate.status,immediate.module};
            if(admitted!=XR_XIR_OK)(void)xr_xir_compile_source_failure_record(request->context->resources,
                &detail.source.failure,&immediate,NULL,NULL,0);
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
    detail.status = status;
    if (diagnostic) *diagnostic = detail;
    else xr_source_compile_diagnostic_free(&detail);
    return status;
}
XR_FUNC XrSourceCompileStatus xr_source_compile_diagnostic_render(
    const XrSourceCompileDiagnostic *d,XrCompileResources *resources,char **output) {
    if(!d||!resources||!output||*output)return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrFrontendReportInfo info={0};
    if(xr_frontend_report_info(d->source.failure.report,&info)&&info.roots) {
        bool located=false;
        for(uint32_t i=0;i<info.roots;++i) {
            XrFrontendRoot root={0};
            if(!xr_frontend_report_root(d->source.failure.report,i,&root))return XR_SOURCE_COMPILE_BAD_ARGUMENT;
            located|=root.primary.path&&*root.primary.path&&(root.primary.flags&XR_FRONTEND_LOCATION_LINE);
        }
        char *rendered=NULL;
        XrSourceCompileStatus status=source_resource_status(xr_frontend_report_render(d->source.failure.report,&rendered));
        if(status!=XR_SOURCE_COMPILE_OK)return status;
        if(located){*output=rendered;return XR_SOURCE_COMPILE_OK;}
        int prefix=snprintf(NULL,0,"source build failed (stage=%u status=%s)\n",
            (unsigned)d->stage,xr_source_compile_status_name(d->status));
        size_t length=strlen(rendered);
        if(prefix<0 || length>SIZE_MAX-(size_t)prefix-1){xr_compile_resources_free(rendered);return XR_SOURCE_COMPILE_OVERFLOW;}
        char *text=NULL;status=source_resource_status(xr_compile_resources_alloc(resources,(size_t)prefix+length+1,(void **)&text));
        if(status==XR_SOURCE_COMPILE_OK) {
            /* An unavailable location must not hide the actual first root. */
            memcpy(text,rendered,length);
            (void)snprintf(text+length,(size_t)prefix+1,"source build failed (stage=%u status=%s)\n",
                (unsigned)d->stage,xr_source_compile_status_name(d->status));
            *output=text;
        }
        xr_compile_resources_free(rendered);return status;
    }
    int size=snprintf(NULL,0,"source build failed (stage=%u status=%s)\n",
        (unsigned)d->stage,xr_source_compile_status_name(d->status));
    if(size<0)return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    char *text=NULL;XrSourceCompileStatus status=source_resource_status(xr_compile_resources_alloc(resources,(size_t)size+1,(void **)&text));
    if(status!=XR_SOURCE_COMPILE_OK)return status;
    (void)snprintf(text,(size_t)size+1,"source build failed (stage=%u status=%s)\n",
        (unsigned)d->stage,xr_source_compile_status_name(d->status));
    *output=text;return XR_SOURCE_COMPILE_OK;
}
static void paths_append(char *out, size_t size, size_t *used, const char *format, ...)
    XR_PRINTF_FMT(4, 5);
static void paths_append(char *out, size_t size, size_t *used, const char *format, ...) {
    if (*used + 1 >= size) return;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(out + *used, size - *used, format, arguments);
    va_end(arguments);
    if (written < 0) { out[*used] = '\0'; return; }
    *used = (size_t)written < size - *used ? *used + (size_t)written : size - 1;
}
/* Lives beside the build formatter so the path selector itself carries no
 * dependency on the compile status vocabulary. */
XR_FUNC void xr_source_paths_diagnostic_format(const XrSourcePathsDiagnostic *diagnostic,
    char *out, size_t size) {
    if (!out || !size) return;
    out[0] = '\0';
    if (!diagnostic) return;
    size_t used = 0;
    paths_append(out, size, &used, "source path lookup failed (stage=%u status=%s io=%u)",
        (unsigned)diagnostic->stage, xr_source_compile_status_name(diagnostic->status),
        (unsigned)diagnostic->io_status);
    if (diagnostic->stage < XR_SOURCE_PATH_ENVIRONMENT) return;
    uint32_t count = diagnostic->candidate_count < XR_SOURCE_STDLIB_CANDIDATE_LIMIT ?
        diagnostic->candidate_count : XR_SOURCE_STDLIB_CANDIDATE_LIMIT;
    if (count) paths_append(out, size, &used, "\n  standard library locations tried:");
    for (uint32_t i = 0; i < count; ++i)
        if (diagnostic->candidates[i]) paths_append(out, size, &used, "\n    %s", diagnostic->candidates[i]);
    paths_append(out, size, &used, "\n  hint: set XRAY_STDLIB_PATH to the standard library directory");
}
XR_FUNC const char *xr_source_compile_status_name(XrSourceCompileStatus status) {
    switch (status) {
    case XR_SOURCE_COMPILE_OK: return "ok";
    case XR_SOURCE_COMPILE_BAD_ARGUMENT: return "bad-argument";
    case XR_SOURCE_COMPILE_NOT_FOUND: return "not-found";
    case XR_SOURCE_COMPILE_INVALID: return "invalid";
    case XR_SOURCE_COMPILE_LIMIT: return "limit";
    case XR_SOURCE_COMPILE_BUDGET: return "budget";
    case XR_SOURCE_COMPILE_MEMORY_LIMIT: return "memory_limit";
    case XR_SOURCE_COMPILE_OVERFLOW: return "overflow";
    case XR_SOURCE_COMPILE_CANCELLED: return "cancelled";
    case XR_SOURCE_COMPILE_DEADLINE: return "deadline";
    case XR_SOURCE_COMPILE_CLOCK_FAILURE: return "clock_failure";
    case XR_SOURCE_COMPILE_OUT_OF_MEMORY: return "out-of-memory";
    case XR_SOURCE_COMPILE_IO: return "io";
    case XR_SOURCE_COMPILE_UNSUPPORTED: return "unsupported";
    case XR_SOURCE_COMPILE_REJECTED: return "source-rejected";
    default: return "invalid-status";
    }
}
