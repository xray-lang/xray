/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_context.h - Compiler context (eliminates global variables)
 *
 * KEY CONCEPT:
 *   Encapsulates all compiler global state for thread-safe compilation.
 */

#ifndef XCOMPILER_CONTEXT_H
#define XCOMPILER_CONTEXT_H

#include "xcompiler.h"
#include "../../runtime/object/xstring.h"
#include "../analyzer/xanalyzer.h"

typedef struct XrClassDescriptor XrClassDescriptor;
struct XiModule;
struct XrModuleGraph;

typedef void (*XrCompilerPostAnalyzeHook)(struct XrCompilerContext *ctx, AstNode *ast,
                                          void *user_data);

#include "../../base/xglobal_indices.h"
#include "../../base/xdefs.h"

struct XrCompilerContext {
    struct XrCompilerSession *compiler_session;
    XrVMRuntime *X;  // Borrowed VM host, for bytecode VM shared/global state.

    int current_line;
    int current_column;
    const char *source_file;
    const char *source_content;  // Borrowed exact source bytes for standalone evidence identity.
    XrGlobalVar *global_vars;
    int global_var_count;

    // Shared variables (unified global variable storage with per-module offset)
    XrSharedVar *shared_vars;
    int shared_var_count;
    int shared_var_capacity;

    bool had_error;
    bool panic_mode;
    bool repl_mode;  // REPL: emit OP_GETGLOBAL/OP_SETGLOBAL, skip shared slot allocation
    XrCompilerPostAnalyzeHook post_analyze_hook;
    void *post_analyze_user_data;
    const struct XrModuleGraph *module_graph;
    struct XiModule **graph_modules;
    int graph_module_count;
    int max_globals;

    XaAnalyzer *analyzer;  // Static type analyzer (unified type system)

    const char *current_operator;        // Current operator method being compiled
    uint8_t current_storage_mode;        // 0=normal, 1=shared
    uint8_t current_elem_tid;            // XrTypeId for container element/value type (0=any)
    uint8_t current_key_tid;             // XrTypeId for Map key type (0=any)
    struct XrType *current_object_type;  // Target type for object literal field reordering

    XrClassDescriptor *current_class_desc;
    void *current_class_node;
    bool is_compiling_struct;  // true when compiling AST_STRUCT_DECL

    /* Analyzer ownership.  When true, context_free destroys the analyzer
     * and its type pool; when false the analyzer outlives the context
     * (REPL uses a session-owned analyzer so type and symbol state
     * persists across compilation units). */
    bool owns_analyzer;
};

/* Create a compiler context with a fresh, owned analyzer. */
XR_FUNC XrCompilerContext *xr_compiler_context_new(struct XrCompilerSession *session);

/* Create a compiler context that borrows an externally-owned analyzer.
 * The caller retains ownership; context_free leaves the analyzer alone.
 * Intended for REPL/LSP where analyzer state must survive per-input
 * compilation units. */
XR_FUNC XrCompilerContext *xr_compiler_context_new_with_analyzer(struct XrCompilerSession *session,
                                                                 XaAnalyzer *analyzer);
XR_FUNC void xr_compiler_context_free(XrCompilerContext *ctx);
XR_FUNC void xr_compiler_context_reset(XrCompilerContext *ctx);

XR_FUNC int xr_compiler_ctx_get_or_add_global(XrCompilerContext *ctx, XrString *name);
XR_FUNC int xr_compiler_ctx_find_global(XrCompilerContext *ctx, XrString *name);

XR_FUNC void xr_compiler_ctx_set_error(XrCompilerContext *ctx);
XR_FUNC bool xr_compiler_ctx_has_error(XrCompilerContext *ctx);

#endif  // XCOMPILER_CONTEXT_H
