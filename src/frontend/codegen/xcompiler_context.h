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
struct XrTargetProfile;

struct XrReplSymbolTable;

typedef struct XrCompilerPolicy {
    const struct XrTargetProfile *target_profile;
    bool runtime_imports;
    const struct XrReplSymbolTable *repl_symbols; /* Borrowed through context lifetime. */
} XrCompilerPolicy;

typedef struct XrCompilerInput {
    XaAnalyzerInput analysis;
    XrCompilerPolicy policy;
} XrCompilerInput;

typedef void (*XrCompilerPostAnalyzeHook)(struct XrCompilerContext *ctx, AstNode *ast,
                                          void *user_data);

#include "../../base/xglobal_indices.h"
#include "../../base/xdefs.h"

struct XrCompilerContext {
    struct XrCompilerSession *compiler_session;
    XrVMRuntime *X;  // Borrowed VM host, for bytecode VM shared/global state.
    struct XrTargetProfile *target_profile; /* retained immutable authority, optional when unused */
    bool runtime_imports;

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
    const struct XrReplSymbolTable *repl_symbols; /* Explicit caller-owned incremental history. */
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
     * (REPL uses a caller-owned analyzer so type and symbol state
     * persists across compilation units). */
    bool owns_analyzer;
};

/* Create a compiler context with a fresh, owned analyzer. */
XR_FUNC XrCompilerContext *xr_compiler_context_new(const XrCompilerInput *input);

/* Create a compiler context that borrows an externally-owned analyzer.
 * The caller retains ownership; context_free leaves the analyzer alone.
 * Intended for REPL/LSP where analyzer state must survive per-input
 * compilation units. */
XR_FUNC XrCompilerContext *xr_compiler_context_new_with_analyzer(XaAnalyzer *analyzer,
                                                               const XrCompilerPolicy *policy);
XR_FUNC void xr_compiler_context_free(XrCompilerContext *ctx);
XR_FUNC void xr_compiler_context_reset(XrCompilerContext *ctx);

XR_FUNC int xr_compiler_ctx_get_or_add_global(XrCompilerContext *ctx, XrString *name);
XR_FUNC int xr_compiler_ctx_find_global(XrCompilerContext *ctx, XrString *name);

XR_FUNC void xr_compiler_ctx_set_error(XrCompilerContext *ctx);
XR_FUNC bool xr_compiler_ctx_has_error(XrCompilerContext *ctx);

#endif  // XCOMPILER_CONTEXT_H
