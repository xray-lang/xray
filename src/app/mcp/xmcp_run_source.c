/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_run_source.c - Checked memory execution with a closed import policy
 *
 * KEY CONCEPT:
 *   Import spelling is checked before resolution; the complete resolved
 *   dependency graph is checked again before any Program is transferred.
 */
#include "xmcp_run_source.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../api/xsource_compiler.h"
#include "../../api/xsource_paths.h"
#include <stdio.h>
#include <string.h>

static const char *const allowed_modules[] = {
    "math", "json", "string", "regex", "encoding", "base64", "url", "datetime",
    "time", "log", "csv", "toml", "xml", "yaml", "types", "test_yield"
};
static bool module_allowed(const char *name) {
    if (!name) return false;
    for (size_t i = 0; i < sizeof(allowed_modules) / sizeof(allowed_modules[0]); ++i)
        if (!strcmp(name, allowed_modules[i])) return true;
    return false;
}
static XrXirStatus request_poll(const XmcpSourceRequest *request) {
    return request->control ? xmcp_source_resource_status(xr_compile_request_control_poll(request->control)) : XR_XIR_OK;
}
static XrXirStatus parse_status(XrParseStatus status) {
    switch (status) {
    case XR_PARSE_OK: return XR_XIR_OK;
    case XR_PARSE_BUDGET: return XR_XIR_BUDGET;
    case XR_PARSE_OUT_OF_MEMORY: return XR_XIR_OUT_OF_MEMORY;
    case XR_PARSE_MEMORY_LIMIT: return XR_XIR_MEMORY_LIMIT;
    case XR_PARSE_OVERFLOW: return XR_XIR_OVERFLOW;
    case XR_PARSE_CANCELLED: return XR_XIR_CANCELLED;
    case XR_PARSE_DEADLINE: return XR_XIR_DEADLINE;
    case XR_PARSE_CLOCK_FAILURE: return XR_XIR_CLOCK_FAILURE;
    default: return XR_XIR_BAD_STRUCTURE;
    }
}
static XrXirStatus source_status(XrSourceCompileStatus status, XrXirStatus detail) {
    switch (status) {
    case XR_SOURCE_COMPILE_OK: return XR_XIR_OK;
    case XR_SOURCE_COMPILE_BUDGET: return XR_XIR_BUDGET;
    case XR_SOURCE_COMPILE_MEMORY_LIMIT: return XR_XIR_MEMORY_LIMIT;
    case XR_SOURCE_COMPILE_OUT_OF_MEMORY: return XR_XIR_OUT_OF_MEMORY;
    case XR_SOURCE_COMPILE_OVERFLOW: return XR_XIR_OVERFLOW;
    case XR_SOURCE_COMPILE_CANCELLED: return XR_XIR_CANCELLED;
    case XR_SOURCE_COMPILE_DEADLINE: return XR_XIR_DEADLINE;
    case XR_SOURCE_COMPILE_CLOCK_FAILURE: return XR_XIR_CLOCK_FAILURE;
    case XR_SOURCE_COMPILE_IO: return XR_XIR_IO;
    default: return detail ? detail : XR_XIR_BAD_STRUCTURE;
    }
}
static void parse_diagnostic(void *context, int line, int column, int end_line,
    int end_column, const char *message) {
    (void)end_line; (void)end_column;
    XrXirSourceProductDiagnostic *diagnostic = context;
    if (!diagnostic || diagnostic->source.message[0]) return;
    diagnostic->source.line = line; diagnostic->source.column = column;
    snprintf(diagnostic->source.message, sizeof(diagnostic->source.message), "%s", message);
}
static XrXirStatus import_preflight(const XmcpSourceRequest *request, XrCompilerSession *session,
    XrXirSourceProductDiagnostic *diagnostic) {
    XrCompileResources *resources = request->context->resources;
    if (request->length == SIZE_MAX) return XR_XIR_OVERFLOW;
    char *text = NULL;
    XrXirStatus status = xmcp_source_resource_status(xr_compile_resources_alloc(resources, request->length + 1,
        (void **)&text));
    if (status != XR_XIR_OK) return status;
    if (status == XR_XIR_OK) {
        if (memchr(request->text, 0, request->length)) status = XR_XIR_BAD_STRUCTURE;
        else { memcpy(text, request->text, request->length); text[request->length] = 0; }
    }
    AstNode *ast = NULL;
    if (status == XR_XIR_OK) {
        XrParseDiagnostics capture = {parse_diagnostic, diagnostic, 1};
        XrParseStatus parsed = xr_compile_parse_with_trivia(session, text, NULL, &capture, &ast);
        status = parse_status(parsed);
    }
    if (status == XR_XIR_OK && (!ast || ast->type != AST_PROGRAM)) status = XR_XIR_BAD_STRUCTURE;
    for (int i = 0; status == XR_XIR_OK && i < ast->as.program.count; ++i) {
        status = request_poll(request);
        if (status != XR_XIR_OK) break;
        const AstNode *node = ast->as.program.statements[i];
        const char *module = NULL;
        bool quoted = false;
        if (node && node->type == AST_IMPORT_STMT) {
            module = node->as.import_stmt.module_name; quoted = node->as.import_stmt.is_quoted;
        } else if (node && node->type == AST_EXPORT_STMT) {
            module = node->as.export_stmt.from_path; quoted = node->as.export_stmt.from_is_quoted;
        }
        if (module && (quoted || !module_allowed(module))) {
            status = XR_XIR_BAD_STRUCTURE;
            if (diagnostic) {
                diagnostic->source.line = node->line; diagnostic->source.column = node->column;
                snprintf(diagnostic->source.message, sizeof(diagnostic->source.message),
                    "MCP runner does not allow import '%s'", module);
            }
        }
    }
    xr_program_destroy(ast); xr_compile_resources_free(text);
    return status;
}
static XrXirStatus dependencies_admit(const XmcpSourceRequest *request,
    const XrXirSourceProduct *product, XrXirSourceProductDiagnostic *diagnostic) {
    const XrXirSourceDependencies *dependencies = xr_xir_compile_source_product_dependencies(product);
    if (!dependencies || dependencies->entry >= dependencies->count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < dependencies->count; ++i) {
        XrXirStatus status = request_poll(request);
        if (status != XR_XIR_OK) return status;
        const XrXirSourceDependency *entry = &dependencies->entries[i];
        if (i == dependencies->entry && entry->kind == XR_XIR_DEPENDENCY_MEMORY) continue;
        /* Native type methods depend on the compiler's implicit prelude.
         * This admits only its resolved stdlib identity; explicit imports
         * still pass the separate spelling policy before resolution. */
        bool allowed = module_allowed(entry->path) || (entry->path && !strcmp(entry->path, "prelude"));
        if (entry->kind != XR_XIR_DEPENDENCY_STDLIB || !allowed) {
            if (diagnostic) snprintf(diagnostic->source.message, sizeof(diagnostic->source.message),
                "MCP runner does not allow resolved dependency '%s'", entry->path ? entry->path : "");
            return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xmcp_run_source_product(const XmcpSourceRequest *request,
    XrXirSourceProduct **output, XrXirSourceProductDiagnostic *diagnostic) {
    if (!request || !request->text || !request->module_id || !request->context ||
        !request->context->resources || !output || *output ||
        (diagnostic && (diagnostic->snapshot || !xr_xir_compile_source_failure_empty(&diagnostic->failure) || diagnostic->status)))
        return XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, request->module_id, NULL};
    if (!xr_module_identity_authority_valid(&authority)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus admitted = request_poll(request);
    if (admitted != XR_XIR_OK) {
        if (diagnostic) { diagnostic->status = admitted; diagnostic->source.status = admitted; }
        return admitted;
    }
    XrStdlibPath path = {0}; XrSourcePathsDiagnostic located = {0};
    XrSourceCompileDiagnostic detail = {0}; XrSourceCompiler *compiler = NULL;
    XrSourceCompileStatus built = xr_source_stdlib_path(request->context->resources, &path, &located);
    xr_source_paths_diagnostic_free(&located);
    if (built == XR_SOURCE_COMPILE_OK) {
        XrSourceCompilerConfig config = {*request->context,
            {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}, path.path,
            xr_source_compile_default_manifest_limits()};
        built = xr_source_compiler_new(&config,request->control, &compiler, &detail);
    }
    XrXirStatus status = source_status(built, XR_XIR_IO);
    if (status == XR_XIR_OK)
        status = import_preflight(request, xr_source_compiler_session(compiler), diagnostic);
    XrXirSourceProduct *product = NULL;
    if (status == XR_XIR_OK) {
        XrSourceCompilerText text = {NULL, request->module_id, request->text, request->length};
        built = xr_source_compiler_build_text(compiler, &text, &product, &detail);
        status = source_status(built, detail.source.status);
    }
    if (diagnostic && (detail.source.snapshot || !xr_xir_compile_source_failure_empty(&detail.source.failure) || detail.source.status)) {
        *diagnostic = detail.source; memset(&detail.source, 0, sizeof(detail.source));
    }
    if (status == XR_XIR_OK) status = dependencies_admit(request, product, diagnostic);
    xr_source_compile_diagnostic_free(&detail);
    xr_source_compiler_free(compiler); xr_source_stdlib_path_free(&path);
    if (status == XR_XIR_OK) status = request_poll(request);
    if (status == XR_XIR_OK) *output = product;
    else {
        xr_xir_compile_source_product_free(product);
        if (diagnostic) { diagnostic->status = status; diagnostic->source.status = status; }
    }
    return status;
}
