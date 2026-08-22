/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_compile.c - Source/AST to VM bytecode compilation API
 *
 * KEY CONCEPT:
 *   AST/source compilation into bytecode. VM execution and lifecycle live in
 *   xvm_exec.c so bytecode-only embedders do not link compiler objects.
 */

#include "../runtime/xisolate_internal.h"
#include "../runtime/xisolate_api.h"
#include "../base/xchecks.h"
#include "../base/xfileio.h"
#include "../base/xlog.h"
#include "../frontend/codegen/xcompiler.h"
#include "../frontend/codegen/xcompiler_context.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../ir/xi.h"
#include "../ir/xi_module.h"
#include "../module/xmodule_graph.h"
#include "../module/xmodule_identity.h"
#include "../os/os_thread.h"
#include "../runtime/value/xchunk.h"
#include "../toolchain/xcompiler_session.h"

/* ========== Compilation API ========== */

struct XiFunc;
void xi_func_free(struct XiFunc *f);

static void free_xi_func_opaque(void *ptr) {
    xi_func_free((struct XiFunc *) ptr);
}

static xr_once_t compiler_proto_hooks_once = XR_ONCE_INITIALIZER;

static void init_compiler_proto_hooks(void) {
    xr_instruction_unit_set_ir_free_fn(free_xi_func_opaque);
}

static void ensure_compiler_proto_hooks(void) {
    xr_once_call(&compiler_proto_hooks_once, init_compiler_proto_hooks);
}

static void restore_session_type_pool(XrCompilerSession *session) {
    if (xr_compiler_session_analyzer_pool(session))
        xr_compiler_session_install_analyzer_pool(session);
}

static bool compile_session_available(XrCompilerSession *session, const char *who) {
    if (!session) {
        xr_log_warning("vm", "%s: compiler session is required", who);
        return false;
    }
    if (!xr_compiler_session_vm_host(session)) {
        xr_log_warning("vm", "%s: compiler session has no VM host", who);
        return false;
    }
    return true;
}

static bool build_compile_unit_identity(const XrModuleIdentityAuthority *authority,
                                        const char *source_file, char **owned_identity,
                                        XrCompileUnitIdentity *identity) {
    *owned_identity = NULL;
    *identity = (XrCompileUnitIdentity) {0};
    XrModuleIdentityKind kind = authority ? authority->kind : 0;
    bool valid = false;
    if (kind == XR_MODULE_IDENTITY_MEMORY) {
        valid = xr_module_identity_from_logical(authority, NULL, owned_identity);
    } else if (kind == XR_MODULE_IDENTITY_STDLIB) {
        char logical_path[512];
        int logical_length = authority && authority->namespace_id
                                 ? snprintf(logical_path, sizeof(logical_path), "%s/%s.xr",
                                            authority->namespace_id, authority->namespace_id)
                                 : -1;
        valid = logical_length > 0 && (size_t) logical_length < sizeof(logical_path) &&
                xr_module_identity_from_logical(authority, logical_path, owned_identity);
    } else {
        char *absolute_source = source_file ? xr_realpath(source_file) : NULL;
        char *logical_path = NULL;
        valid = absolute_source && xr_module_identity_from_source(
                                       authority, absolute_source, owned_identity, &logical_path);
        xr_free(logical_path);
        xr_free(absolute_source);
    }
    if (!valid)
        return false;
    identity->kind = kind == XR_MODULE_IDENTITY_STDLIB
                         ? XR_COMPILE_UNIT_STDLIB
                         : (kind == XR_MODULE_IDENTITY_MEMORY ? XR_COMPILE_UNIT_MEMORY
                                                              : XR_COMPILE_UNIT_USER);
    identity->module_identity = *owned_identity;
    identity->stdlib_module_name =
        kind == XR_MODULE_IDENTITY_STDLIB ? authority->namespace_id : NULL;
    return true;
}

// Compile AST to bytecode (internal)
//
// The compiler's for-in desugaring creates AST nodes via xr_ast_* helpers.
// Re-enter the program arena through the active compiler session so these
// synthetic nodes share the AST lifetime without mutating VM isolate state.
static XrProto *compile_ast_internal(XrCompilerSession *session, AstNode *ast,
                                     const char *source_file,
                                     const XrModuleIdentityAuthority *authority,
                                     const XrModuleGraph *graph,
                                     XiModule **graph_modules, int graph_module_count,
                                     XiModule **out_module, XaAnalyzer *shared_analyzer) {
    if (out_module)
        *out_module = NULL;
    if (!compile_session_available(session, "compile_ast_internal"))
        return NULL;
    char *owned_identity = NULL;
    XrCompileUnitIdentity identity = {0};
    if (!ast || !build_compile_unit_identity(authority, source_file, &owned_identity, &identity)) {
        xr_log_warning("vm", "compile_ast_internal: explicit module authority is invalid");
        return NULL;
    }
    XrCompilerSessionOperationScope operation_scope;
    if (!xr_compiler_session_operation_begin(session, &operation_scope)) {
        xr_log_warning("vm", "compile_ast_internal: compiler session is busy");
        xr_free(owned_identity);
        return NULL;
    }
    XrCompileUnitIdentity current = xr_compiler_session_compile_unit_identity(session);
    bool installed_identity = current.module_identity == NULL;
    if ((installed_identity && !xr_compiler_session_set_compile_unit_identity(session, &identity)) ||
        (!installed_identity && strcmp(current.module_identity, identity.module_identity) != 0)) {
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        xr_free(owned_identity);
        return NULL;
    }
    ensure_compiler_proto_hooks();

    XrCompilerSessionScope ast_scope;
    bool has_ast_scope =
        ast->type == AST_PROGRAM && ast->as.program.arena &&
        xr_compiler_session_push_arena(session, ast->as.program.arena, source_file, &ast_scope);

    XrCompilerContext *ctx = shared_analyzer
                                 ? xr_compiler_context_new_with_analyzer(session, shared_analyzer)
                                 : xr_compiler_context_new(session);
    if (ctx == NULL) {
        xr_log_warning("vm", "failed to create compiler context");
        if (has_ast_scope)
            xr_compiler_session_pop_arena(&ast_scope);
        if (installed_identity)
            xr_compiler_session_set_compile_unit_identity(session, NULL);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        xr_free(owned_identity);
        return NULL;
    }
    xa_analyzer_set_graph(ctx->analyzer, xr_compiler_session_module_graph(session));

    ctx->source_file = source_file;
    ctx->module_graph = graph;
    ctx->graph_modules = graph_modules;
    ctx->graph_module_count = graph_module_count;

    XrProto *proto = xr_compile(ctx, ast);
    if (proto && !xr_entry_plan_derive(proto)) {
        xr_instruction_unit_free(proto);
        proto = NULL;
    }
    if (proto && out_module) {
        XiFunc *ir = (XiFunc *) proto->xi_func;
        *out_module = ir ? ir->module : NULL;
        if (!*out_module) {
            xr_instruction_unit_free(proto);
            proto = NULL;
        }
    }

    xr_compiler_context_free(ctx);

    // Restore type pool: compiler context freed its analyzer-owned pool, so
    // TLS must fall back to the session-owned long-lived analyzer pool.
    restore_session_type_pool(session);

    if (has_ast_scope)
        xr_compiler_session_pop_arena(&ast_scope);

    if (installed_identity)
        xr_compiler_session_set_compile_unit_identity(session, NULL);

    bool operation_ok = proto ? xr_compiler_session_operation_succeed(&operation_scope)
                              : xr_compiler_session_operation_fail(
                                    &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
    if (!operation_ok && proto) {
        xr_instruction_unit_free(proto);
        proto = NULL;
    }

    xr_free(owned_identity);

    return proto;
}

XrProto *xr_compile_ast_with_source(XrCompilerSession *session, AstNode *ast,
                                    const char *source_file,
                                    const XrModuleIdentityAuthority *authority) {
    return compile_ast_internal(session, ast, source_file, authority, NULL, NULL, 0, NULL, NULL);
}

XrProto *xr_compile_ast_in_graph(XrCompilerSession *session, XaAnalyzer *shared_analyzer,
                                 AstNode *ast, const char *source_file, const XrModuleGraph *graph,
                                 XiModule **graph_modules, int graph_module_count,
                                 XiModule **out_module,
                                 const XrModuleIdentityAuthority *authority) {
    if (!shared_analyzer || !graph || !graph_modules || graph_module_count <= 0 || !out_module ||
        !authority)
        return NULL;
    return compile_ast_internal(session, ast, source_file, authority, graph, graph_modules,
                                graph_module_count, out_module, shared_analyzer);
}

XrProto *xr_compile_source_with_path(XrCompilerSession *session, const char *source,
                                     const char *source_file,
                                     const XrModuleIdentityAuthority *authority) {
    if (!compile_session_available(session, "compile_source_with_path"))
        return NULL;
    char *owned_identity = NULL;
    XrCompileUnitIdentity identity = {0};
    if (!source || !build_compile_unit_identity(authority, source_file, &owned_identity,
                                                &identity)) {
        xr_log_warning("vm", "compile_source_with_path: explicit module authority is invalid");
        return NULL;
    }
    XrCompilerSessionOperationScope operation_scope;
    if (!xr_compiler_session_operation_begin(session, &operation_scope)) {
        xr_log_warning("vm", "compile_source_with_path: compiler session is busy");
        xr_free(owned_identity);
        return NULL;
    }
    XrCompileUnitIdentity current = xr_compiler_session_compile_unit_identity(session);
    bool installed_identity = current.module_identity == NULL;
    if ((installed_identity && !xr_compiler_session_set_compile_unit_identity(session, &identity)) ||
        (!installed_identity && strcmp(current.module_identity, identity.module_identity) != 0)) {
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        xr_free(owned_identity);
        return NULL;
    }
    ensure_compiler_proto_hooks();
    // Create compiler context FIRST to ensure type pool is valid during parsing
    XrCompilerContext *ctx = xr_compiler_context_new(session);
    if (!ctx) {
        xr_log_warning("vm", "failed to create compiler context");
        if (installed_identity)
            xr_compiler_session_set_compile_unit_identity(session, NULL);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        xr_free(owned_identity);
        return NULL;
    }
    xa_analyzer_set_graph(ctx->analyzer, xr_compiler_session_module_graph(session));

    ctx->source_file = source_file;

    // Now parse with valid type pool
    AstNode *ast = xr_parse_with_source(session, source, source_file);
    if (!ast) {
        xr_compiler_context_free(ctx);
        if (installed_identity)
            xr_compiler_session_set_compile_unit_identity(session, NULL);
        (void) xr_compiler_session_operation_fail(
            &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
        xr_free(owned_identity);
        return NULL;
    }

    // Compile
    XrProto *proto = xr_compile(ctx, ast);
    if (proto && !xr_entry_plan_derive(proto)) {
        xr_instruction_unit_free(proto);
        proto = NULL;
    }

    xr_compiler_context_free(ctx);

    // Restore type pool: compiler context freed its analyzer-owned pool, so
    // TLS falls back to the session-owned long-lived analyzer pool for
    // post-compile allocations.
    restore_session_type_pool(session);

    // Free AST (not needed after compilation)
    xr_program_destroy(ast);

    if (installed_identity)
        xr_compiler_session_set_compile_unit_identity(session, NULL);

    bool operation_ok = proto ? xr_compiler_session_operation_succeed(&operation_scope)
                              : xr_compiler_session_operation_fail(
                                    &operation_scope, XR_COMPILER_SESSION_OPERATION_FATAL);
    if (!operation_ok && proto) {
        xr_instruction_unit_free(proto);
        proto = NULL;
    }

    xr_free(owned_identity);

    return proto;
}
