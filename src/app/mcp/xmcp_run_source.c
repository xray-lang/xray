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
#include "../../toolchain/xcompiler_session.h"
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
static XrXirStatus resource_status(XrCompileResourceStatus status) {
    return status == XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET :
        status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
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
    if (request->length == SIZE_MAX) return XR_XIR_BUDGET;
    char *text = NULL;
    XrXirStatus status = resource_status(xr_compile_resources_alloc(resources, request->length + 1,
        (void **)&text));
    if (status != XR_XIR_OK) return status;
    status = resource_status(xr_compile_resources_work(resources, request->length + 1));
    if (status == XR_XIR_OK) {
        if (memchr(request->text, 0, request->length)) status = XR_XIR_BAD_STRUCTURE;
        else { memcpy(text, request->text, request->length); text[request->length] = 0; }
    }
    AstNode *ast = NULL;
    if (status == XR_XIR_OK) {
        XrParseDiagnostics capture = {parse_diagnostic, diagnostic, 1};
        XrParseStatus parsed = xr_compile_parse_with_trivia(session, text, NULL, &capture, &ast);
        status = parsed == XR_PARSE_OK ? XR_XIR_OK : parsed == XR_PARSE_BUDGET ? XR_XIR_BUDGET :
            parsed == XR_PARSE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
    }
    if (status == XR_XIR_OK && (!ast || ast->type != AST_PROGRAM)) status = XR_XIR_BAD_STRUCTURE;
    for (int i = 0; status == XR_XIR_OK && i < ast->as.program.count; ++i) {
        status = resource_status(xr_compile_resources_work(resources, 1));
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
        XrXirStatus status = resource_status(xr_compile_resources_work(request->context->resources, 1));
        if (status != XR_XIR_OK) return status;
        const XrXirSourceDependency *entry = &dependencies->entries[i];
        if (i == dependencies->entry && entry->kind == XR_XIR_DEPENDENCY_MEMORY) continue;
        if (entry->kind != XR_XIR_DEPENDENCY_STDLIB || !module_allowed(entry->path)) {
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
        (diagnostic && (diagnostic->snapshot || diagnostic->source_path || diagnostic->status)))
        return XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_MEMORY, request->module_id, NULL};
    if (!xr_module_identity_authority_valid(&authority)) return XR_XIR_BAD_STRUCTURE;
    XrCompilerSession *session = NULL;
    XrCompilerSessionStatus opened = xr_compile_session_new(request->context->resources, &session);
    XrXirStatus status = opened == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        opened == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET :
        opened == XR_COMPILER_SESSION_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
    if (status == XR_XIR_OK) status = import_preflight(request, session, diagnostic);
    XrXirSourceProduct *product = NULL;
    if (status == XR_XIR_OK) {
        XrXirSourceProductRequest input = {{session, NULL, &authority, request->context,
            NULL, NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
        XrXirSourceText text = {NULL, request->text, request->length};
        status = xr_xir_compile_source_product_build_text(&input, &text, &product, diagnostic);
    }
    if (status == XR_XIR_OK) status = dependencies_admit(request, product, diagnostic);
    if (status == XR_XIR_OK) status = xr_xir_compile_source_product_public_admit(product,
        diagnostic ? &diagnostic->source : NULL);
    xr_compile_session_free(session);
    if (status == XR_XIR_OK) *output = product;
    else {
        xr_xir_compile_source_product_free(product);
        if (diagnostic) { diagnostic->status = status; diagnostic->source.status = status; }
    }
    return status;
}
