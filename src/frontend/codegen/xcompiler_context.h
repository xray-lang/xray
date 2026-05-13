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
#include "../../base/xarena.h"
#include "../../runtime/object/xstring.h"
#include "../analyzer/xanalyzer.h"

typedef struct XrClassDescriptor XrClassDescriptor;

typedef enum {
    CONST_INT,
    CONST_FLOAT,
    CONST_STRING,
    CONST_BOOL,
    CONST_NULL
} ConstValueType;

typedef struct ConstEntry {
    XrString *name;
    ConstValueType type;
    union {
        int64_t int_val;
        double float_val;
        XrString *str_val;
        bool bool_val;
    } value;
} ConstEntry;

#include "../../base/xglobal_indices.h"
#include "../../base/xdefs.h"

struct XrCompilerContext {
    XrayIsolate *X;

    int current_line;
    int current_column;
    const char *source_file;
    XrGlobalVar *global_vars;
    int global_var_count;

    // Shared variables (unified global variable storage with per-module offset)
    XrSharedVar *shared_vars;
    int shared_var_count;
    int shared_var_capacity;
    int shared_offset;  // base offset for this compilation unit in isolate->vm.shared

    bool had_error;
    bool panic_mode;
    bool repl_mode;  // REPL incremental mode: allow redefine, shared_offset=0
    int max_globals;

    // Enum type names for type inference
    char **enum_type_names;
    int enum_type_count;
    int enum_type_capacity;

    struct XrShapeCache *shape_cache;

    XaAnalyzer *analyzer;  // Static type analyzer (unified type system)
    XrArena arena;

    const char *current_operator;        // Current operator method being compiled
    uint8_t current_storage_mode;        // 0=normal, 1=shared
    uint8_t current_elem_tid;            // XrTypeId for container element/value type (0=any)
    uint8_t current_key_tid;             // XrTypeId for Map key type (0=any)
    struct XrType *current_object_type;  // Target type for object literal field reordering

    XrClassDescriptor *current_class_desc;
    void *current_class_node;
    bool is_compiling_struct;  // true when compiling AST_STRUCT_DECL

    ConstEntry *const_entries;
    int const_entry_count;
    int const_entry_capacity;

    /* Analyzer ownership.  When true, context_free destroys the analyzer
     * and its type pool; when false the analyzer outlives the context
     * (REPL uses an isolate-scoped analyzer so type and symbol state
     * persists across compilation units). */
    bool owns_analyzer;
};

/* Create a compiler context with a fresh, owned analyzer. */
XR_FUNC XrCompilerContext *xr_compiler_context_new(XrayIsolate *X);

/* Create a compiler context that borrows an externally-owned analyzer.
 * The caller retains ownership; context_free leaves the analyzer alone.
 * Intended for REPL/LSP where analyzer state must survive per-input
 * compilation units. */
XR_FUNC XrCompilerContext *xr_compiler_context_new_with_analyzer(XrayIsolate *X,
                                                                 XaAnalyzer *analyzer);
XR_FUNC void xr_compiler_context_free(XrCompilerContext *ctx);
XR_FUNC void xr_compiler_context_reset(XrCompilerContext *ctx);

XR_FUNC int xr_compiler_ctx_get_or_add_global(XrCompilerContext *ctx, XrString *name);
XR_FUNC int xr_compiler_ctx_find_global(XrCompilerContext *ctx, XrString *name);

XR_FUNC void xr_compiler_ctx_set_error(XrCompilerContext *ctx);
XR_FUNC bool xr_compiler_ctx_has_error(XrCompilerContext *ctx);

XR_FUNC void xr_compiler_ctx_register_enum_type(XrCompilerContext *ctx, const char *enum_name);
XR_FUNC bool xr_compiler_ctx_is_enum_type(XrCompilerContext *ctx, const char *var_name);

XR_FUNC void xr_compiler_ctx_add_const_int(XrCompilerContext *ctx, XrString *name, int64_t value);
XR_FUNC void xr_compiler_ctx_add_const_float(XrCompilerContext *ctx, XrString *name, double value);
XR_FUNC void xr_compiler_ctx_add_const_string(XrCompilerContext *ctx, XrString *name,
                                              XrString *value);
XR_FUNC ConstEntry *xr_compiler_ctx_find_const(XrCompilerContext *ctx, XrString *name);

#endif  // XCOMPILER_CONTEXT_H
