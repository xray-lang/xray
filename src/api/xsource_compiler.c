/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_compiler.c - One resource ledger and parser owner across source builds
 *
 * KEY CONCEPT:
 *   Owned products are published only after ordinary public admission.
 */
#include "xsource_compiler.h"
#include "xsource_authority.h"
#include "../base/xfileio.h"
#include "../base/xio_policy.inc.h"
#include "../os/os_fs.h"
#include "../xir/xxir_compile_memory.h"
#include "../xir/xxir_stdlib_internal.h"

struct XrSourceCompiler {
    XrXirCompileContext context;
    XrXirTarget target;
    XrTomlParseLimits manifest_limits;
    XrCompilerSession *session;
    XrXirLibraryCatalog *libraries;
    char *stdlib_path;
};
static XrSourceCompileStatus compiler_resource(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_SOURCE_COMPILE_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_SOURCE_COMPILE_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_SOURCE_COMPILE_OUT_OF_MEMORY : XR_SOURCE_COMPILE_BAD_ARGUMENT;
}
static XrSourceCompileStatus compiler_io(XrOsIoStatus status) {
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
static XrSourceCompileStatus compiler_xir(XrXirStatus status) {
    switch (status) {
    case XR_XIR_OK: return XR_SOURCE_COMPILE_OK;
    case XR_XIR_BAD_STRUCTURE: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    case XR_XIR_BUDGET: return XR_SOURCE_COMPILE_BUDGET;
    case XR_XIR_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_XIR_IO: return XR_SOURCE_COMPILE_IO;
    case XR_XIR_UNRESOLVED: return XR_SOURCE_COMPILE_NOT_FOUND;
    default: return XR_SOURCE_COMPILE_REJECTED;
    }
}
static XrSourceCompileStatus compiler_manifest(XrManifestStatus status) {
    switch (status) {
    case XR_MANIFEST_OK: return XR_SOURCE_COMPILE_OK;
    case XR_MANIFEST_BAD_ARGUMENT: return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    case XR_MANIFEST_NOT_FOUND: return XR_SOURCE_COMPILE_NOT_FOUND;
    case XR_MANIFEST_LIMIT: return XR_SOURCE_COMPILE_LIMIT;
    case XR_MANIFEST_BUDGET: return XR_SOURCE_COMPILE_BUDGET;
    case XR_MANIFEST_OUT_OF_MEMORY: return XR_SOURCE_COMPILE_OUT_OF_MEMORY;
    case XR_MANIFEST_IO: return XR_SOURCE_COMPILE_IO;
    case XR_MANIFEST_UNSUPPORTED: return XR_SOURCE_COMPILE_UNSUPPORTED;
    default: return XR_SOURCE_COMPILE_INVALID;
    }
}
static bool compiler_diagnostic_empty(const XrSourceCompileDiagnostic *diagnostic) {
    return !diagnostic || (!diagnostic->source.snapshot && !diagnostic->source.source_path);
}
static XrSourceCompileStatus compiler_publish(XrSourceCompileDiagnostic *out,
    XrSourceCompileDiagnostic *detail, XrSourceCompileStatus status) {
    detail->status = status;
    if (out) *out = *detail;
    else xr_source_compile_diagnostic_free(detail);
    return status;
}
XR_FUNC void xr_source_compiler_free(XrSourceCompiler *compiler) {
    if (!compiler) return;
    xr_xir_compile_library_catalog_free(compiler->libraries);
    xr_compile_session_free(compiler->session);
    xr_compile_resources_free(compiler->stdlib_path);
    xr_compile_resources_free(compiler);
}
static bool compiler_absolute(XrIoContext *io, const char *path) {
    unsigned char prefix[3] = {0};
    for (size_t i = 0; i < sizeof(prefix); ++i) {
        if (!io_work(io, 1)) return false;
        prefix[i] = (unsigned char)path[i]; if (!prefix[i]) break;
    }
#ifdef XR_OS_WINDOWS
    bool sep0 = prefix[0] == '/' || prefix[0] == '\\';
    bool sep1 = prefix[1] == '/' || prefix[1] == '\\';
    bool letter = (prefix[0] >= 'A' && prefix[0] <= 'Z') || (prefix[0] >= 'a' && prefix[0] <= 'z');
    return (sep0 && sep1) || (letter && prefix[1] == ':' && (prefix[2] == '/' || prefix[2] == '\\'));
#else
    return prefix[0] == '/';
#endif
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_new(const XrSourceCompilerConfig *config,
    XrSourceCompiler **output, XrSourceCompileDiagnostic *diagnostic) {
    if (!compiler_diagnostic_empty(diagnostic)) return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileDiagnostic detail = {0};
    if (!config || !output || *output || !xir_compile_context_valid(&config->context) ||
        !config->absolute_stdlib_path || !config->manifest_limits.input_bytes ||
        !config->manifest_limits.depth || config->manifest_limits.depth > 128)
        return compiler_publish(diagnostic, &detail, XR_SOURCE_COMPILE_BAD_ARGUMENT);
    if (config->target.architecture != XR_XIR_ARCH_X86_64 || config->target.abi_version != XR_XIR_VALUE_ABI_VERSION)
        return compiler_publish(diagnostic, &detail, XR_SOURCE_COMPILE_UNSUPPORTED);
    XrOsIoPolicy input_policy = xr_compile_io_policy(config->context.resources);
    XrIoContext input_io = {&input_policy, XR_OS_IO_OK};
    detail.stage = XR_SOURCE_COMPILE_PATH;
    if (!compiler_absolute(&input_io, config->absolute_stdlib_path))
        return compiler_publish(diagnostic, &detail, input_io.status == XR_OS_IO_OK ?
            XR_SOURCE_COMPILE_BAD_ARGUMENT : compiler_io(input_io.status));
    XrSourceCompiler *compiler = NULL;
    XrSourceCompileStatus status = compiler_resource(xr_compile_resources_calloc(config->context.resources,
        1, sizeof(*compiler), (void **)&compiler));
    if (status != XR_SOURCE_COMPILE_OK) return compiler_publish(diagnostic, &detail, status);
    compiler->context = config->context; compiler->target = config->target;
    compiler->manifest_limits = config->manifest_limits;
    detail.stage = XR_SOURCE_COMPILE_SESSION;
    detail.session_status = xr_compile_session_new(config->context.resources, &compiler->session);
    if (detail.session_status != XR_COMPILER_SESSION_OK) {
        status = detail.session_status == XR_COMPILER_SESSION_BUDGET ? XR_SOURCE_COMPILE_BUDGET :
            detail.session_status == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_SOURCE_COMPILE_OUT_OF_MEMORY : XR_SOURCE_COMPILE_BAD_ARGUMENT;
    } else {
        detail.stage = XR_SOURCE_COMPILE_PATH;
        XrOsIoPolicy policy = xr_compile_io_policy(config->context.resources);
        XrOsIoStatus located = xr_realpath_owned(&policy, config->absolute_stdlib_path, &compiler->stdlib_path);
        XrFsStat info = {0};
        if (located == XR_OS_IO_OK) located = xr_os_io_stat(&policy, compiler->stdlib_path, &info);
        if (located == XR_OS_IO_OK && info.kind != XR_FS_DIR) located = XR_OS_IO_BAD_ARGUMENT;
        status = compiler_io(located);
        if (status == XR_SOURCE_COMPILE_OK) {
            detail.stage = XR_SOURCE_COMPILE_LIBRARY;
            status = compiler_xir(xir_stdlib_library_catalog_new(&compiler->context, compiler->stdlib_path, &compiler->libraries));
        }
    }
    if (status == XR_SOURCE_COMPILE_OK) *output = compiler;
    else xr_source_compiler_free(compiler);
    return compiler_publish(diagnostic, &detail, status);
}
XR_FUNC const XrXirCompileContext *xr_source_compiler_context(const XrSourceCompiler *compiler) {
    return compiler ? &compiler->context : NULL;
}
XR_FUNC XrCompilerSession *xr_source_compiler_session(const XrSourceCompiler *compiler) {
    return compiler ? compiler->session : NULL;
}
XR_FUNC XrCompileState *xr_source_compiler_state(const XrSourceCompiler *compiler) {
    return compiler ? xr_compile_session_compile_state(compiler->session) : NULL;
}
XR_FUNC XrCompileResources *xr_source_compiler_resources(const XrSourceCompiler *compiler) {
    return compiler ? compiler->context.resources : NULL;
}
static XrSourceCompileStatus compiler_capture(XrSourceCompiler *compiler, XrSourceCompileStatus status) {
    if (status != XR_SOURCE_COMPILE_BUDGET && status != XR_SOURCE_COMPILE_OUT_OF_MEMORY) return status;
    return compiler_resource(xr_compile_state_fail(xr_source_compiler_state(compiler),
        status == XR_SOURCE_COMPILE_BUDGET ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OUT_OF_MEMORY));
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_file(XrSourceCompiler *compiler, const char *absolute_path,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic) {
    if (!compiler || !compiler_diagnostic_empty(diagnostic)) return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileStatus available = compiler_resource(xr_compile_session_resource_status(compiler->session));
    if (available != XR_SOURCE_COMPILE_OK) {
        XrSourceCompileDiagnostic detail = {0}; detail.stage = XR_SOURCE_COMPILE_SESSION;
        return compiler_publish(diagnostic, &detail, available);
    }
    XrSourceCompileRequest request = {&compiler->context, absolute_path, compiler->stdlib_path,
        compiler->libraries, compiler->manifest_limits, compiler->target, compiler->session};
    return xr_source_compile_build(&request, output, diagnostic);
}
static XrSourceCompileStatus compiler_build_input(XrSourceCompiler *compiler, const XrSourceCompilerText *input,
    struct AstNode *program, XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic) {
    if (!compiler_diagnostic_empty(diagnostic)) return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    XrSourceCompileDiagnostic detail = {0};
    if (!compiler || !input || !input->text || !output || *output ||
        (input->absolute_path ? input->namespace_id != NULL : !input->namespace_id))
        return compiler_publish(diagnostic, &detail, XR_SOURCE_COMPILE_BAD_ARGUMENT);
    XrSourceCompileStatus status = compiler_resource(xr_compile_session_resource_status(compiler->session));
    if (status != XR_SOURCE_COMPILE_OK) return compiler_publish(diagnostic, &detail, status);
    XrSourceAuthority *authority = NULL;
    XrModuleIdentityAuthority memory = {XR_MODULE_IDENTITY_MEMORY, input->namespace_id, NULL};
    const XrModuleIdentityAuthority *identity = &memory;
    XrXirSourceText text = {NULL, input->text, input->length};
    const char *entry = NULL; XrLockfile *lockfile = NULL;
    if (input->absolute_path) {
        detail.stage = XR_SOURCE_COMPILE_AUTHORITY;
        XrSourceEntryInput rooted = {XR_SOURCE_ENTRY_TEXT, input->absolute_path, input->text, input->length};
        detail.authority_status = xr_source_authority_open_input(&compiler->context, &rooted, compiler->libraries,
            &compiler->manifest_limits, &authority, &detail.authority);
        status = compiler_manifest(detail.authority_status);
        if (status == XR_SOURCE_COMPILE_OK) {
            const XrModuleOverlayInput *overlay = xr_source_authority_overlay(authority);
            identity = xr_source_authority_entry(authority); entry = xr_source_authority_source_path(authority);
            lockfile = xr_source_authority_lockfile(authority);
            text = (XrXirSourceText){overlay->logical_path, overlay->text, overlay->length};
        }
    }
    XrXirSourceProduct *product = NULL;
    if (status == XR_SOURCE_COMPILE_OK) {
        detail.stage = XR_SOURCE_COMPILE_PRODUCT;
        XrXirSourceProductRequest request = {{compiler->session, entry, identity, &compiler->context,
            compiler->stdlib_path, lockfile, XR_XIR_PROGRAM, compiler->libraries}, compiler->target};
        if (program) {
            XrXirSourceAst syntax = {text, program};
            status = compiler_xir(xr_xir_compile_source_product_build_ast(&request, &syntax, &product, &detail.source));
        } else {
            status = compiler_xir(xr_xir_compile_source_product_build_text(&request, &text, &product, &detail.source));
        }
        if (status == XR_SOURCE_COMPILE_OK) {
            XrXirStatus admitted = xr_xir_compile_source_product_public_admit(product, &detail.source.source);
            status = compiler_xir(admitted);
            if (admitted != XR_XIR_OK) {
                detail.source.stage = XR_XIR_SOURCE_PRODUCT_FACTS; detail.source.status = admitted;
                xr_xir_compile_source_product_free(product); product = NULL;
            }
        }
    }
    xr_source_authority_close(authority);
    status = compiler_capture(compiler, status);
    if (status == XR_SOURCE_COMPILE_OK) *output = product;
    return compiler_publish(diagnostic, &detail, status);
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_text(XrSourceCompiler *compiler, const XrSourceCompilerText *input,
    XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic) {
    return compiler_build_input(compiler, input, NULL, output, diagnostic);
}
XR_FUNC XrSourceCompileStatus xr_source_compiler_build_ast(XrSourceCompiler *compiler, const XrSourceCompilerText *input,
    struct AstNode *program, XrXirSourceProduct **output, XrSourceCompileDiagnostic *diagnostic) {
    if (!program) return XR_SOURCE_COMPILE_BAD_ARGUMENT;
    return compiler_build_input(compiler, input, program, output, diagnostic);
}
