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
#include "../base/xlog.h"
#include "../frontend/codegen/xcompiler.h"
#include "../frontend/codegen/xcompiler_context.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../ir/xi.h"
#include "../ir/xi_module.h"
#include "../module/xmodule_graph.h"
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
    xr_vm_proto_set_ir_free_fn(free_xi_func_opaque);
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

// Compile AST to bytecode (internal)
//
// The compiler's for-in desugaring creates AST nodes via xr_ast_* helpers.
// Re-enter the program arena through the active compiler session so these
// synthetic nodes share the AST lifetime without mutating VM isolate state.
static XrProto *compile_ast_internal(XrCompilerSession *session, AstNode *ast,
                                     const char *source_file, const XrModuleGraph *graph,
                                     XiModule **graph_modules, int graph_module_count,
                                     XiModule **out_module) {
    if (out_module)
        *out_module = NULL;
    if (!compile_session_available(session, "compile_ast_internal"))
        return NULL;
    XR_DCHECK(ast != NULL, "compile_ast_internal: NULL ast");
    ensure_compiler_proto_hooks();

    XrCompilerSessionScope ast_scope;
    bool has_ast_scope =
        ast->type == AST_PROGRAM && ast->as.program.arena &&
        xr_compiler_session_push_arena(session, ast->as.program.arena, source_file, &ast_scope);

    XrCompilerContext *ctx = xr_compiler_context_new(session);
    if (ctx == NULL) {
        xr_log_warning("vm", "failed to create compiler context");
        if (has_ast_scope)
            xr_compiler_session_pop_arena(&ast_scope);
        return NULL;
    }
    xa_analyzer_set_graph(ctx->analyzer, xr_compiler_session_module_graph(session));

    ctx->source_file = source_file;
    ctx->module_graph = graph;
    ctx->graph_modules = graph_modules;
    ctx->graph_module_count = graph_module_count;

    XrProto *proto = xr_compile(ctx, ast);
    if (proto && !xr_vm_entry_plan_derive(proto)) {
        xr_vm_proto_free(proto);
        proto = NULL;
    }
    if (proto && out_module) {
        XiFunc *ir = (XiFunc *) proto->xi_func;
        *out_module = ir ? ir->module : NULL;
        if (!*out_module) {
            xr_vm_proto_free(proto);
            proto = NULL;
        }
    }

    xr_compiler_context_free(ctx);

    // Restore type pool: compiler context freed its analyzer-owned pool, so
    // TLS must fall back to the session-owned long-lived analyzer pool.
    restore_session_type_pool(session);

    if (has_ast_scope)
        xr_compiler_session_pop_arena(&ast_scope);

    return proto;
}

XrProto *xr_compile_ast_with_source(XrCompilerSession *session, AstNode *ast,
                                    const char *source_file) {
    return compile_ast_internal(session, ast, source_file, NULL, NULL, 0, NULL);
}

XrProto *xr_compile_ast_in_graph(XrCompilerSession *session, AstNode *ast, const char *source_file,
                                 const XrModuleGraph *graph, XiModule **graph_modules,
                                 int graph_module_count, XiModule **out_module) {
    if (!graph || !graph_modules || graph_module_count <= 0 || !out_module)
        return NULL;
    return compile_ast_internal(session, ast, source_file, graph, graph_modules, graph_module_count,
                                out_module);
}

XrProto *xr_compile_source_with_path(XrCompilerSession *session, const char *source,
                                     const char *source_file) {
    if (!compile_session_available(session, "compile_source_with_path"))
        return NULL;
    XR_DCHECK(source != NULL, "compile_source_with_path: NULL source");
    ensure_compiler_proto_hooks();
    // Create compiler context FIRST to ensure type pool is valid during parsing
    XrCompilerContext *ctx = xr_compiler_context_new(session);
    if (!ctx) {
        xr_log_warning("vm", "failed to create compiler context");
        return NULL;
    }
    xa_analyzer_set_graph(ctx->analyzer, xr_compiler_session_module_graph(session));

    ctx->source_file = source_file;

    // Now parse with valid type pool
    AstNode *ast = xr_parse_with_source(session, source, source_file);
    if (!ast) {
        xr_compiler_context_free(ctx);
        return NULL;
    }

    // Compile
    XrProto *proto = xr_compile(ctx, ast);
    if (proto && !xr_vm_entry_plan_derive(proto)) {
        xr_vm_proto_free(proto);
        proto = NULL;
    }

    xr_compiler_context_free(ctx);

    // Restore type pool: compiler context freed its analyzer-owned pool, so
    // TLS falls back to the session-owned long-lived analyzer pool for
    // post-compile allocations.
    restore_session_type_pool(session);

    // Free AST (not needed after compilation)
    xr_program_destroy(ast);

    return proto;
}
