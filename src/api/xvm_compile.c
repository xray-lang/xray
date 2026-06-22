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
#include "../runtime/value/xchunk.h"
#include "../toolchain/xcompiler_session.h"

/* ========== Compilation API ========== */

struct XiFunc;
void xi_func_free(struct XiFunc *f);

static void free_xi_func_opaque(void *ptr) {
    xi_func_free((struct XiFunc *) ptr);
}

static void ensure_compiler_proto_hooks(void) {
    xr_vm_proto_set_ir_free_fn(free_xi_func_opaque);
}

static void restore_session_type_pool(XrayIsolate *isolate) {
    XrCompilerSession *session = xr_compiler_session_current_for_isolate(isolate);
    if (xr_compiler_session_analyzer_pool(session))
        xr_compiler_session_install_analyzer_pool(session);
}

// Compile AST to bytecode (internal)
//
// The compiler's for-in desugaring creates AST nodes via xr_ast_* helpers.
// Re-enter the program arena through the active compiler session so these
// synthetic nodes share the AST lifetime without mutating VM isolate state.
static XrProto *compile_ast_internal(XrayIsolate *isolate, AstNode *ast, const char *source_file) {
    XR_DCHECK(isolate != NULL, "compile_ast_internal: NULL isolate");
    XR_DCHECK(ast != NULL, "compile_ast_internal: NULL ast");
    ensure_compiler_proto_hooks();

    XrCompilerSessionScope ast_scope;
    bool has_ast_scope =
        ast->type == AST_PROGRAM && ast->as.program.arena &&
        xr_compiler_session_push_arena(isolate, ast->as.program.arena, source_file, &ast_scope);

    XrCompilerContext *ctx = xr_compiler_context_new(isolate);
    if (ctx == NULL) {
        xr_log_warning("vm", "failed to create compiler context");
        if (has_ast_scope)
            xr_compiler_session_pop_arena(&ast_scope);
        return NULL;
    }

    ctx->source_file = source_file;

    ctx->shared_offset = isolate->vm.shared.count;

    XrProto *proto = xr_compile(ctx, ast);

    // Sync shared variable count back to isolate (offset-adjusted)
    int total_shared = ctx->shared_offset + ctx->shared_var_count;
    if (total_shared > isolate->vm.shared.count) {
        isolate->vm.shared.count = total_shared;
        xr_shared_array_ensure(&isolate->vm.shared, total_shared - 1);
    }

    xr_compiler_context_free(ctx);

    // Restore type pool: compiler context freed its analyzer's pool, leaving
    // current_type_pool as a dangling pointer. Fall back to the session-owned
    // long-lived analyzer pool.
    restore_session_type_pool(isolate);

    if (has_ast_scope)
        xr_compiler_session_pop_arena(&ast_scope);

    return proto;
}

// Compile AST to bytecode
XrProto *xr_compile_ast(XrayIsolate *isolate, AstNode *ast) {
    return compile_ast_internal(isolate, ast, NULL);
}

// Compile AST to bytecode (with source file path)
XrProto *xr_compile_ast_with_source(XrayIsolate *isolate, AstNode *ast, const char *source_file) {
    return compile_ast_internal(isolate, ast, source_file);
}

// Compile source code to bytecode (creates compiler context before parsing)
// This ensures type pool is valid during parsing for type annotations
XrProto *xr_compile_source_with_path(XrayIsolate *isolate, const char *source,
                                     const char *source_file) {
    XR_DCHECK(isolate != NULL, "compile_source_with_path: NULL isolate");
    XR_DCHECK(source != NULL, "compile_source_with_path: NULL source");
    ensure_compiler_proto_hooks();
    // Create compiler context FIRST to ensure type pool is valid during parsing
    XrCompilerContext *ctx = xr_compiler_context_new(isolate);
    if (!ctx) {
        xr_log_warning("vm", "failed to create compiler context");
        return NULL;
    }

    ctx->source_file = source_file;
    ctx->shared_offset = isolate->vm.shared.count;

    // Now parse with valid type pool
    AstNode *ast = xr_parse_with_source(isolate, source, source_file);
    if (!ast) {
        xr_compiler_context_free(ctx);
        return NULL;
    }

    // Compile
    XrProto *proto = xr_compile(ctx, ast);
    int total_shared = ctx->shared_offset + ctx->shared_var_count;
    if (total_shared > isolate->vm.shared.count) {
        isolate->vm.shared.count = total_shared;
        xr_shared_array_ensure(&isolate->vm.shared, total_shared - 1);
    }

    xr_compiler_context_free(ctx);

    // Restore type pool: compiler context freed its own pool, leaving
    // current_type_pool as a dangling pointer. Fall back to the session-owned
    // long-lived analyzer pool so post-compile callers can allocate safely.
    restore_session_type_pool(isolate);

    // Free AST (not needed after compilation)
    xr_program_destroy(ast);

    return proto;
}
