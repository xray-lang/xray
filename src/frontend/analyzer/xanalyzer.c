/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer.c - Static type analyzer implementation
 */

#include "xanalyzer.h"
#include "../../runtime/xisolate_internal.h"
#include "xanalyzer_visitor.h"
#include "../../base/xchecks.h"
#include "xanalyzer_infer.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_incremental.h"
#include "xanalyzer_builtin_interfaces.h"
#include "xanalyzer_jit.h"
#include "xa_node_table.h"
#include "../../base/xintmap.h"
#include "../../base/xhashmap.h"
#include "../../base/xmalloc.h"
#include <string.h>
#include <stdio.h>

// Register a builtin function symbol in analyzer scope
static void register_builtin_func(XaAnalyzer *analyzer, const char *name, XrType *type) {
    XR_DCHECK(analyzer != NULL, "register_builtin_func: NULL analyzer");
    XR_DCHECK(name != NULL, "register_builtin_func: NULL name");
    XR_DCHECK(type != NULL, "register_builtin_func: NULL type");
    XaSymbol *sym = xa_symbol_new(name, XA_SYM_FUNCTION);
    sym->location.line = 0;
    sym->is_builtin = true;
    xa_scope_add_symbol(analyzer->global_scope, sym);
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links) {
        links->type = type;
        links->declared_type = type;
        links->is_definitely_assigned = true;
    }
}

// Register a builtin module namespace (XA_SYM_MODULE triggers member signature lookup)
static void register_builtin_module(XaAnalyzer *analyzer, const char *name) {
    XR_DCHECK(analyzer != NULL, "register_builtin_module: NULL analyzer");
    XR_DCHECK(name != NULL, "register_builtin_module: NULL name");
    XaSymbol *sym = xa_symbol_new(name, XA_SYM_MODULE);
    sym->location.line = 0;
    sym->is_builtin = true;
    xa_scope_add_symbol(analyzer->global_scope, sym);
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links) {
        links->type = xr_type_new_unknown(NULL);
        links->declared_type = links->type;
        links->is_definitely_assigned = true;
        links->module_name = name;
    }
}

// Register a builtin variable symbol in analyzer scope
static void register_builtin_var(XaAnalyzer *analyzer, const char *name, XrType *type,
                                 bool is_const) {
    XR_DCHECK(analyzer != NULL, "register_builtin_var: NULL analyzer");
    XR_DCHECK(name != NULL, "register_builtin_var: NULL name");
    XaSymbol *sym = xa_symbol_new(name, XA_SYM_VARIABLE);
    sym->location.line = 0;
    sym->is_builtin = true;
    sym->is_const = is_const;
    xa_scope_add_symbol(analyzer->global_scope, sym);
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links) {
        links->type = type;
        links->declared_type = type;
        links->is_definitely_assigned = true;
    }
}

// Register all Codegen builtin functions/constructors that are available at runtime
static void xa_register_codegen_builtins(XaAnalyzer *analyzer) {
    // Reusable param types
    XrType *p_any = xr_type_new_unknown(NULL);
    XrType *t_void = xr_type_new_void(NULL);
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_float = xr_type_new_float(NULL);
    XrType *t_string = xr_type_new_string(NULL);
    XrType *t_bool = xr_type_new_bool(NULL);

    // Test framework: fn(...any) -> void
    XrType *fn_assert = xr_type_new_function(analyzer->isolate, NULL, 0, t_void, true);
    fn_assert->function.min_params = 1;
    register_builtin_func(analyzer, "assert", fn_assert);
    XrType *fn_assert2 = xr_type_new_function(analyzer->isolate, NULL, 0, t_void, true);
    fn_assert2->function.min_params = 2;
    register_builtin_func(analyzer, "assert_eq", fn_assert2);
    register_builtin_func(analyzer, "assert_ne", fn_assert2);
    register_builtin_func(analyzer, "assert_true", fn_assert);
    register_builtin_func(analyzer, "assert_false", fn_assert);
    register_builtin_func(analyzer, "assert_throws", fn_assert);

    // Type conversion: fn(any) -> T (precise return type)
    XrType *fn_to_int = xr_type_new_function(analyzer->isolate, &p_any, 1, t_int, false);
    register_builtin_func(analyzer, "int", fn_to_int);
    XrType *fn_to_float = xr_type_new_function(analyzer->isolate, &p_any, 1, t_float, false);
    register_builtin_func(analyzer, "float", fn_to_float);
    XrType *fn_to_string = xr_type_new_function(analyzer->isolate, &p_any, 1, t_string, false);
    register_builtin_func(analyzer, "string", fn_to_string);
    XrType *fn_to_bool = xr_type_new_function(analyzer->isolate, &p_any, 1, t_bool, false);
    register_builtin_func(analyzer, "bool", fn_to_bool);

    // Type constructors: fn(...any) -> Container
    XrType *fn_array = xr_type_new_function(analyzer->isolate, NULL, 0,
                                            xr_type_new_array(analyzer->isolate, p_any), true);
    fn_array->function.min_params = 0;
    register_builtin_func(analyzer, "Array", fn_array);
    XrType *fn_map = xr_type_new_function(analyzer->isolate, NULL, 0,
                                          xr_type_new_map(analyzer->isolate, p_any, p_any), true);
    fn_map->function.min_params = 0;
    register_builtin_func(analyzer, "Map", fn_map);
    XrType *fn_set = xr_type_new_function(analyzer->isolate, NULL, 0,
                                          xr_type_new_set(analyzer->isolate, p_any), true);
    fn_set->function.min_params = 0;
    register_builtin_func(analyzer, "Set", fn_set);
    XrType *fn_bytes = xr_type_new_function(
        analyzer->isolate, NULL, 0,
        xr_type_new_array(analyzer->isolate,
                          xr_type_new_int_width(analyzer->isolate, XR_NATIVE_U8)),
        true);
    fn_bytes->function.min_params = 0;
    register_builtin_func(analyzer, "Bytes", fn_bytes);
    XrType *fn_weakmap = xr_type_new_function(
        analyzer->isolate, NULL, 0, xr_type_new_map(analyzer->isolate, p_any, p_any), true);
    fn_weakmap->function.min_params = 0;
    register_builtin_func(analyzer, "WeakMap", fn_weakmap);
    XrType *fn_weakset = xr_type_new_function(analyzer->isolate, NULL, 0,
                                              xr_type_new_set(analyzer->isolate, p_any), true);
    fn_weakset->function.min_params = 0;
    register_builtin_func(analyzer, "WeakSet", fn_weakset);

    // typeof: fn(any) -> int (returns XrTypeId)
    XrType *fn_typeof = xr_type_new_function(analyzer->isolate, &p_any, 1, t_int, false);
    register_builtin_func(analyzer, "typeof", fn_typeof);
    // typename: fn(any) -> string
    XrType *fn_typename = xr_type_new_function(analyzer->isolate, &p_any, 1, t_string, false);
    register_builtin_func(analyzer, "typename", fn_typename);
    // chr: fn(int) -> string
    XrType *fn_chr = xr_type_new_function(analyzer->isolate, &t_int, 1, t_string, false);
    register_builtin_func(analyzer, "chr", fn_chr);
    // copy: fn(any) -> any (preserves unknown type)
    XrType *fn_copy = xr_type_new_function(analyzer->isolate, &p_any, 1, p_any, false);
    register_builtin_func(analyzer, "copy", fn_copy);
    // dump: fn(any, ...) -> void
    XrType *fn_dump = xr_type_new_function(analyzer->isolate, &p_any, 1, t_void, true);
    register_builtin_func(analyzer, "dump", fn_dump);
    // print/println: fn(...any) -> void
    XrType *fn_print = xr_type_new_function(analyzer->isolate, NULL, 0, t_void, true);
    fn_print->function.min_params = 0;
    register_builtin_func(analyzer, "print", fn_print);
    register_builtin_func(analyzer, "println", fn_print);
    // sleep: fn(int) -> void (milliseconds)
    XrType *fn_sleep = xr_type_new_function(analyzer->isolate, &t_int, 1, t_void, false);
    register_builtin_func(analyzer, "sleep", fn_sleep);
    // spawn: fn(fn) -> any (returns coroutine task)
    XrType *fn_spawn = xr_type_new_function(analyzer->isolate, &p_any, 1, p_any, true);
    register_builtin_func(analyzer, "spawn", fn_spawn);

    // Modules/namespaces (XA_SYM_MODULE enables member signature lookup)
    register_builtin_module(analyzer, "Json");
    register_builtin_module(analyzer, "Coro");
    register_builtin_module(analyzer, "CoroPool");
    register_builtin_module(analyzer, "Channel");
    register_builtin_module(analyzer, "Reflect");
    register_builtin_module(analyzer, "Type");

    // Runtime global variables (set by xray_isolate_set_script_info)
    register_builtin_var(analyzer, "process", p_any, true);
    register_builtin_var(analyzer, "__file__", t_string, true);
    register_builtin_var(analyzer, "__dir__", t_string, true);
}

// Create analyzer
XaAnalyzer *xa_analyzer_new(XrayIsolate *X) {
    // Ensure process-level type singletons are initialized (idempotent)
    xr_type_global_init();

    XaAnalyzer *analyzer = xr_calloc(1, sizeof(XaAnalyzer));
    if (!analyzer)
        return NULL;

    // Store owning isolate (explicit, no TLS)
    analyzer->isolate = X;

    // Initialize type pool (per-analyzer, no global state)
    analyzer->type_pool = xr_type_pool_new();
    if (!analyzer->type_pool) {
        xr_free(analyzer);
        return NULL;
    }

    // Set pool on isolate so type_alloc can reach it via X
    if (X)
        X->current_type_pool = analyzer->type_pool;

    // Legacy TLS path (will be removed once all callers pass X)
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->type_pool->next_type_id);
    xa_symbol_set_id_counter(&analyzer->next_symbol_id);

    // Initialize symbol ID counter (starts at 1)
    analyzer->next_symbol_id = 1;

    // Symbol registry for O(1) ID lookup (must be set before any scope_add_symbol)
    analyzer->symbols_by_id = xr_intmap_new();
    xa_symbol_set_registry(analyzer->symbols_by_id);

    analyzer->global_scope = xa_scope_new(XA_SCOPE_GLOBAL, NULL);
    analyzer->current_scope = analyzer->global_scope;
    analyzer->files_map = xr_hashmap_new();

    // Register built-in interfaces (Iterable, Comparable, etc.)
    xa_register_builtin_interfaces(X, analyzer->global_scope);

    // Register Codegen builtin functions/constructors
    xa_register_codegen_builtins(analyzer);

    // Default options
    analyzer->strict_null_checks = false;
    analyzer->strict_mode = false;
    analyzer->infer_return_types = true;

    // Initialize incremental analysis support
    analyzer->incremental = xa_incremental_new();

    // AST -> inferred-type side table.
    analyzer->node_table = xa_node_table_new();

    return analyzer;
}

// Free analyzer
void xa_analyzer_free(XaAnalyzer *analyzer) {
    if (!analyzer)
        return;

    xa_scope_free(analyzer->global_scope);

    // Free symbol registry (values are XaSymbol* owned by scopes, don't free them)
    if (analyzer->symbols_by_id) {
        xr_intmap_free((XrIntMap *) analyzer->symbols_by_id);
    }
    xa_symbol_set_registry(NULL);
    xa_symbol_set_id_counter(NULL);

    // Free incremental analysis context
    if (analyzer->incremental) {
        xa_incremental_free(analyzer->incremental);
    }

    // Free the AST -> inferred-type side table.
    if (analyzer->node_table) {
        xa_node_table_free((XaNodeTable *) analyzer->node_table);
        analyzer->node_table = NULL;
    }

    // Free JIT metadata
    if (analyzer->jit_metadata) {
        xa_jit_metadata_free(analyzer->jit_metadata);
    }

    // Free type pool
    if (analyzer->type_pool) {
        xr_type_pool_free(analyzer->type_pool);
    }

    // Free files map (entries already freed via linked list above)
    if (analyzer->files_map) {
        xr_hashmap_free((XrHashMap *) analyzer->files_map);
    }

    // Free diagnostics
    XaDiagnostic *diag = analyzer->diagnostics;
    while (diag) {
        XaDiagnostic *next = diag->next;
        if (diag->message)
            xr_free((void *) diag->message);
        // Note: diag->code is an int (error code), not a pointer - no free needed
        xr_free(diag);
        diag = next;
    }

    xr_free(analyzer);
}

// Configuration
void xa_analyzer_set_strict_null(XaAnalyzer *analyzer, bool enable) {
    if (analyzer)
        analyzer->strict_null_checks = enable;
}

void xa_analyzer_set_strict_mode(XaAnalyzer *analyzer, bool enable) {
    if (analyzer)
        analyzer->strict_mode = enable;
}

// Symbol lookup
XaSymbol *xa_analyzer_lookup(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return NULL;
    return xa_scope_lookup(analyzer->current_scope, name);
}

XaSymbol *xa_analyzer_lookup_in_scope(XaAnalyzer *analyzer, const char *name, XaScope *scope) {
    if (!analyzer || !name)
        return NULL;
    return xa_scope_lookup(scope ? scope : analyzer->current_scope, name);
}

// Recursive deep search with position awareness
// Collects all matches, caller picks the best one
static void lookup_deep_collect(XaScope *scope, const char *name, XaSymbol **results, int *count,
                                int max) {
    if (!scope || *count >= max)
        return;

    XaSymbol *local = xa_scope_lookup_local(scope, name);
    if (local) {
        results[*count] = local;
        (*count)++;
    }

    for (int i = 0; i < scope->child_count && *count < max; i++) {
        lookup_deep_collect(scope->children[i], name, results, count, max);
    }
}

XaSymbol *xa_analyzer_lookup_deep(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name || !analyzer->global_scope)
        return NULL;

    XaSymbol *results[32];
    int count = 0;
    lookup_deep_collect(analyzer->global_scope, name, results, &count, 32);

    if (count == 0)
        return NULL;
    if (count == 1)
        return results[0];

    // Multiple matches: return the one with the highest line number
    // (innermost / latest declaration is most likely the intended one)
    XaSymbol *best = results[0];
    for (int i = 1; i < count; i++) {
        if (results[i]->location.line > best->location.line) {
            best = results[i];
        }
    }
    return best;
}

// Get type of symbol (lazy computation)
XrType *xa_analyzer_get_type(XaAnalyzer *analyzer, XaSymbol *symbol) {
    if (!analyzer || !symbol)
        return NULL;

    // Ensure type pool is set for this thread
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->next_symbol_id);

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, symbol);
    if (!links)
        return xr_type_new_unknown(NULL);

    // Return cached type
    if (links->type)
        return links->type;

    // Return declared type if available
    if (links->declared_type) {
        links->type = links->declared_type;
        return links->type;
    }

    // Fallback: some symbols may not have type after analysis (e.g. forward refs).
    // Return unknown as safe default.
    return xr_type_new_unknown(NULL);
}

// Get class info
XrClassInfo *xa_analyzer_get_class(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return NULL;

    XaSymbol *sym = xa_scope_lookup(analyzer->global_scope, name);
    if (!sym || sym->kind != XA_SYM_CLASS)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    return links ? links->class_info : NULL;
}

// Get members of a type (for LSP completions)
XaSymbol **xa_analyzer_get_members(XaAnalyzer *analyzer, XrType *type, int *count) {
    *count = 0;
    if (!type)
        return NULL;
    (void) analyzer;  // May be NULL for builtin lookups

    // For class instances, return class members (including inherited)
    if (XR_TYPE_IS_INSTANCE(type) && type->instance.class_ref) {
        XrClassInfo *info = type->instance.class_ref;

        // Count total members including inherited (walk inheritance chain)
        int total = 0;
        for (XrClassInfo *c = info; c != NULL; c = c->base) {
            total += c->field_count + c->method_count;
        }
        if (total == 0)
            return NULL;

        XaSymbol **members = xr_malloc(sizeof(XaSymbol *) * total);
        if (!members)
            return NULL;
        int idx = 0;

        // Collect members from current class and all base classes
        for (XrClassInfo *c = info; c != NULL; c = c->base) {
            for (int i = 0; i < c->field_count; i++) {
                members[idx++] = c->fields[i];
            }
            for (int i = 0; i < c->method_count; i++) {
                members[idx++] = c->methods[i];
            }
        }

        *count = idx;
        return members;
    }

    // Handle builtin types (Array, Map, String, etc.)
    return xa_builtin_get_members(type, count);
}

// Get symbols in scope (includes parent scopes)
XaSymbol **xa_analyzer_get_scope_symbols(XaAnalyzer *analyzer, XaScope *scope, int *count) {
    *count = 0;
    if (!analyzer)
        return NULL;

    XaScope *s = scope ? scope : analyzer->current_scope;
    if (!s)
        return NULL;

    // Single-pass: collect symbols with dynamic array growth
    int capacity = 64;
    int idx = 0;
    XaSymbol **result = xr_malloc(sizeof(XaSymbol *) * capacity);
    if (!result)
        return NULL;

    for (XaScope *p = s; p; p = p->parent) {
        int sc = 0;
        XaSymbol **syms = xa_scope_get_all_symbols(p, &sc);
        if (syms) {
            // Grow if needed
            if (idx + sc > capacity) {
                while (idx + sc > capacity)
                    capacity *= 2;
                XaSymbol **tmp = xr_realloc(result, sizeof(XaSymbol *) * capacity);
                if (!tmp) {
                    xr_free(result);
                    xr_free(syms);
                    return NULL;
                }
                result = tmp;
            }
            for (int i = 0; i < sc; i++) {
                result[idx++] = syms[i];
            }
            xr_free(syms);
        }
    }

    if (idx == 0) {
        xr_free(result);
        return NULL;
    }

    *count = idx;
    return result;
}

// Diagnostics
XaDiagnostic *xa_analyzer_get_diagnostics(XaAnalyzer *analyzer, int *count) {
    if (!analyzer) {
        *count = 0;
        return NULL;
    }
    *count = analyzer->diagnostic_count;
    return analyzer->diagnostics;
}

void xa_analyzer_clear_diagnostics(XaAnalyzer *analyzer) {
    if (!analyzer)
        return;

    XaDiagnostic *diag = analyzer->diagnostics;
    while (diag) {
        XaDiagnostic *next = diag->next;
        if (diag->message)
            xr_free((void *) diag->message);
        xr_free(diag);
        diag = next;
    }
    analyzer->diagnostics = NULL;
    analyzer->diagnostics_tail = NULL;
    analyzer->diagnostic_count = 0;
}

void xa_analyzer_add_diagnostic(XaAnalyzer *analyzer, XrDiagSeverity severity, int code,
                                const char *message, XrLocation *loc) {
    if (!analyzer)
        return;

    XaDiagnostic *diag = xr_calloc(1, sizeof(XaDiagnostic));
    if (!diag)
        return;

    diag->severity = severity;
    diag->code = code;
    diag->message = message ? xr_strdup(message) : NULL;
    if (loc) {
        diag->location = *loc;
    }

    // Append to tail: preserves source-order (first error reported = first in list)
    diag->next = NULL;
    if (analyzer->diagnostics_tail) {
        analyzer->diagnostics_tail->next = diag;
    } else {
        analyzer->diagnostics = diag;
    }
    analyzer->diagnostics_tail = diag;
    analyzer->diagnostic_count++;
}

// Scope management
// Reuses existing child scope if ast_node matches (Pass 2 reuses Pass 1 scope)
void xa_analyzer_enter_scope(XaAnalyzer *analyzer, XaScopeKind kind, void *ast_node) {
    if (!analyzer)
        return;

    if (ast_node) {
        for (int i = 0; i < analyzer->current_scope->child_count; i++) {
            XaScope *child = analyzer->current_scope->children[i];
            if (child->ast_node == ast_node) {
                // Reusing Pass 1 scope: verify kind consistency
                XR_DCHECK(child->kind == kind, "enter_scope: scope kind mismatch on reuse");
                analyzer->current_scope = child;
                return;
            }
        }
    }

    XaScope *scope = xa_scope_new(kind, analyzer->current_scope);
    scope->ast_node = ast_node;
    analyzer->current_scope = scope;
}

void xa_analyzer_exit_scope(XaAnalyzer *analyzer) {
    if (!analyzer)
        return;
    XR_DCHECK(analyzer->current_scope->parent != NULL,
              "exit_scope: already at root scope (unbalanced enter/exit)");
    if (!analyzer->current_scope->parent)
        return;
    analyzer->current_scope = analyzer->current_scope->parent;
}

// Type checking
static bool typecheck_source_precise_for_target(XrType *target, XrType *source) {
    XR_DCHECK(target != NULL, "typecheck_source_precise_for_target: NULL target");
    XR_DCHECK(source != NULL, "typecheck_source_precise_for_target: NULL source");
    if (!target || !source)
        return false;

    if (XR_TYPE_IS_UNKNOWN(target))
        return true;
    if (XR_TYPE_IS_UNKNOWN(source))
        return false;
    return true;
}

bool xa_typecheck_assignable(XrType *target, XrType *source) {
    XR_DCHECK(target != NULL, "xa_typecheck_assignable: NULL target");
    XR_DCHECK(source != NULL, "xa_typecheck_assignable: NULL source");
    if (!target || !source)
        return false;
    if (!xr_type_assignable(target, source))
        return false;
    return typecheck_source_precise_for_target(target, source);
}

bool xa_analyzer_check_assignment(XaAnalyzer *analyzer, XrType *target, XrType *source,
                                  XrLocation *loc) {
    if (!analyzer || !target || !source)
        return false;

    if (xa_typecheck_assignable(target, source)) {
        return true;
    }

    // Generate error
    char message[256];
    snprintf(message, sizeof(message), "Type '%s' is not assignable to type '%s'",
             xr_type_to_string(source), xr_type_to_string(target));
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, message,
                               loc);

    return false;
}

bool xa_analyzer_check_call(XaAnalyzer *analyzer, XrType *func_type, XrType **arg_types,
                            int arg_count, XrLocation *loc) {
    if (!analyzer || !func_type)
        return false;

    if (!XR_TYPE_IS_FUNCTION(func_type)) {
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_NOT_CALLABLE,
                                   "Value is not callable", loc);
        return false;
    }

    // Check argument count
    int expected = func_type->function.param_count;
    if (arg_count < expected && !func_type->function.is_variadic) {
        char message[128];
        snprintf(message, sizeof(message), "Expected %d arguments, but got %d", expected,
                 arg_count);
        xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_WRONG_ARG_COUNT,
                                   message, loc);
        return false;
    }

    // Check argument types
    bool ok = true;
    for (int i = 0; i < arg_count && i < expected; i++) {
        if (!xa_typecheck_assignable(func_type->function.param_types[i], arg_types[i])) {
            ok = false;
            char message[256];
            snprintf(message, sizeof(message),
                     "Argument %d: Type '%s' is not assignable to parameter type '%s'", i + 1,
                     xr_type_to_string(arg_types[i]),
                     xr_type_to_string(func_type->function.param_types[i]));
            xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                       message, loc);
        }
    }

    return ok;
}

// Full AST analysis
// Find or create file entry
static XaFileEntry *find_or_create_file(XaAnalyzer *analyzer, const char *file) {
    if (!analyzer || !file)
        return NULL;

    // O(1) hash lookup
    XrHashMap *fmap = (XrHashMap *) analyzer->files_map;
    if (fmap) {
        XaFileEntry *found = (XaFileEntry *) xr_hashmap_get(fmap, file);
        if (found)
            return found;
    }

    // Create new entry
    XaFileEntry *entry = xr_calloc(1, sizeof(XaFileEntry));
    if (!entry)
        return NULL;

    entry->path = xr_strdup(file);
    entry->dirty = true;
    entry->next = analyzer->files;
    analyzer->files = entry;
    analyzer->file_count++;

    // Register in hash map
    if (fmap) {
        xr_hashmap_set(fmap, entry->path, entry);
    }

    return entry;
}

// Clear all base pointers that reference a specific class_info (before freeing it)
static void clear_base_references(XaScope *scope, XrClassInfo *target, XaAnalyzer *analyzer) {
    if (!scope || !target)
        return;

    int count = 0;
    XaSymbol **syms = xa_scope_get_all_symbols(scope, &count);
    if (syms) {
        for (int i = 0; i < count; i++) {
            if (syms[i]->kind != XA_SYM_CLASS)
                continue;
            XaSymbolLinks *links = xa_analyzer_get_links(analyzer, syms[i]);
            if (links && links->class_info && links->class_info->base == target) {
                links->class_info->base = NULL;
            }
        }
        xr_free(syms);
    }

    for (int i = 0; i < scope->child_count; i++) {
        clear_base_references(scope->children[i], target, analyzer);
    }
}

// Remove symbols owned by a specific file from scope
static void remove_file_symbols(XaScope *scope, const char *file, XaAnalyzer *analyzer) {
    if (!scope || !file)
        return;

    // Get all symbols and check ownership
    int count = 0;
    XaSymbol **syms = xa_scope_get_all_symbols(scope, &count);
    if (syms) {
        for (int i = 0; i < count; i++) {
            XaSymbol *sym = syms[i];
            XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
            if (links && links->file_path && strcmp(links->file_path, file) == 0) {
                // Clear class_info - first clear all base references to prevent dangling pointers
                if (links->class_info) {
                    clear_base_references(analyzer->global_scope, links->class_info, analyzer);
                    xa_class_info_free(links->class_info);
                    links->class_info = NULL;
                }

                // Actually remove symbol from scope
                if (sym->name) {
                    xa_scope_remove_symbol(scope, sym->name);
                }

                // Free the symbol (xa_symbol_free also releases inline links content)
                xa_symbol_free(sym);
            }
        }
        xr_free(syms);
    }

    // Process child scopes
    for (int i = 0; i < scope->child_count; i++) {
        remove_file_symbols(scope->children[i], file, analyzer);
    }
}

void xa_analyzer_analyze(XaAnalyzer *analyzer, const char *file, XrAstNode *ast) {
    if (!analyzer || !ast)
        return;

    // Set current type pool and symbol ID counter (eliminates global state)
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->next_symbol_id);
    xa_symbol_set_id_counter(&analyzer->next_symbol_id);

    // Track file
    XaFileEntry *entry = find_or_create_file(analyzer, file);
    if (entry) {
        entry->dirty = false;
    }

    // Set current file for symbol ownership tracking
    analyzer->current_file = file;

    // Use the visitor-based analysis
    xa_analyze_ast(analyzer, ast);

    analyzer->current_file = NULL;

    // Keep pool set - type_check may call xa_analyzer_infer_expr_type after analyze
    // Pool will be cleared when analyzer is freed
}

void xa_analyzer_update(XaAnalyzer *analyzer, const char *file, XrAstNode *ast) {
    if (!analyzer || !ast)
        return;

    // Set current type pool and symbol ID counter
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->next_symbol_id);
    xa_symbol_set_id_counter(&analyzer->next_symbol_id);

    // Find file entry
    XaFileEntry *entry = find_or_create_file(analyzer, file);

    // Check if content changed using hash (incremental optimization)
    XaIncrementalCtx *incr = (XaIncrementalCtx *) analyzer->incremental;
    if (incr && entry) {
        uint64_t new_hash = xa_hash_ast_block((AstNode *) ast);
        if (entry->content_hash == new_hash && !entry->dirty) {
            // Content unchanged - skip re-analysis
            return;
        }
        entry->content_hash = new_hash;
    }

    // Clear diagnostics for this file
    xa_analyzer_clear_diagnostics(analyzer);

    // Remove old symbols from this file
    if (file) {
        remove_file_symbols(analyzer->global_scope, file, analyzer);
    }

    // Mark as dirty then re-analyze
    if (entry)
        entry->dirty = true;
    xa_analyzer_analyze(analyzer, file, ast);

    // Mark as clean after analysis
    if (entry)
        entry->dirty = false;
}

void xa_analyzer_refresh_file(XaAnalyzer *analyzer, const char *file, XrAstNode *ast,
                              uint64_t content_hash) {
    if (!analyzer || !ast)
        return;

    // Set current type pool and symbol ID counter
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->next_symbol_id);
    xa_symbol_set_id_counter(&analyzer->next_symbol_id);

    // Find file entry
    XaFileEntry *entry = find_or_create_file(analyzer, file);

    // Check if content changed using provided hash (true incremental check)
    if (entry && entry->content_hash == content_hash && !entry->dirty) {
        // Content unchanged - skip re-analysis entirely
        return;
    }

    // Update hash
    if (entry) {
        entry->content_hash = content_hash;
    }

    XaIncrementalCtx *incr = (XaIncrementalCtx *) analyzer->incremental;

    // Collect symbols that will be removed (for dependency propagation)
    int old_sym_count = 0;
    XaSymbol **old_symbols = xa_scope_get_all_symbols(analyzer->global_scope, &old_sym_count);

    // Build change set from symbols in this file
    XaChangeSet changes = {0};
    if (old_symbols && incr && old_sym_count > 0) {
        changes.modified_symbols = xr_malloc(sizeof(uint32_t) * old_sym_count);
        if (changes.modified_symbols) {
            for (int i = 0; i < old_sym_count; i++) {
                if (old_symbols[i]->location.file &&
                    strcmp(old_symbols[i]->location.file, file) == 0) {
                    changes.modified_symbols[changes.modified_count++] = old_symbols[i]->id;
                }
            }

            // Propagate dirty through dependency graph
            xa_propagate_dirty(incr, &changes);

            // Track statistics
            if (incr->dirty_count > changes.modified_count) {
                // Dependencies were affected
                incr->incremental_updates++;
            }

            xr_free(changes.modified_symbols);
        }
        xr_free(old_symbols);
    } else if (old_symbols) {
        xr_free(old_symbols);
    }

    // Clear diagnostics for this file
    xa_analyzer_clear_diagnostics(analyzer);

    // Remove old symbols from this file
    if (file) {
        remove_file_symbols(analyzer->global_scope, file, analyzer);
    }

    // Re-analyze this file
    xa_analyzer_analyze(analyzer, file, ast);

    // Mark other files as dirty based on affected symbols
    if (incr && incr->dirty_count > 0) {
        for (int i = 0; i < incr->dirty_count; i++) {
            XaSymbol *sym = xa_scope_lookup_by_id(analyzer->global_scope, incr->dirty_symbols[i]);
            if (sym && sym->location.file && strcmp(sym->location.file, file) != 0) {
                // Symbol is in another file - mark that file as dirty
                xa_analyzer_mark_file_dirty(analyzer, sym->location.file);
            }
        }
    }

    // Mark current file as clean
    if (entry)
        entry->dirty = false;
}

void xa_analyzer_mark_file_dirty(XaAnalyzer *analyzer, const char *file) {
    if (!analyzer || !file)
        return;

    XrHashMap *fmap = (XrHashMap *) analyzer->files_map;
    XaFileEntry *entry = fmap ? (XaFileEntry *) xr_hashmap_get(fmap, file) : NULL;
    if (entry) {
        entry->dirty = true;
    }
}

const char **xa_analyzer_get_dirty_files(XaAnalyzer *analyzer, int *count) {
    *count = 0;
    if (!analyzer)
        return NULL;

    // Count dirty files
    int dirty_count = 0;
    XaFileEntry *entry = analyzer->files;
    while (entry) {
        if (entry->dirty)
            dirty_count++;
        entry = entry->next;
    }

    if (dirty_count == 0)
        return NULL;

    // Allocate and fill array
    const char **result = xr_malloc(sizeof(const char *) * dirty_count);
    if (!result)
        return NULL;

    int idx = 0;
    entry = analyzer->files;
    while (entry && idx < dirty_count) {
        if (entry->dirty) {
            result[idx++] = entry->path;
        }
        entry = entry->next;
    }

    *count = idx;
    return result;
}

void xa_analyzer_invalidate_range(XaAnalyzer *analyzer, const char *file, uint32_t start_line,
                                  uint32_t end_line) {
    // Today this degrades to whole-file dirty marking. The
    // (start_line, end_line) range is currently unused but is part of
    // the API contract so a future block-level incremental implementation
    // can use it without breaking call sites.
    //
    // Calling invalidate_range() on an untracked file MUST register it.
    // There was no public file-registration entry before, so an LSP
    // client that issued an edit before any save+analyze would silently
    // lose the dirty signal. find_or_create_file() initialises
    // entry->dirty = true so the next refresh_file() call rebuilds it.
    (void) start_line;
    (void) end_line;
    if (!analyzer || !file)
        return;
    XaFileEntry *entry = find_or_create_file(analyzer, file);
    if (entry) {
        entry->dirty = true;
    }
}

// AST -> inferred type side table convenience wrappers.
// These exist so callers (codegen, LSP, mono) do not need to include
// xa_node_table.h directly -- xanalyzer.h is enough. Both functions
// are NULL-safe in every direction.
void xa_analyzer_set_node_type(XaAnalyzer *analyzer, struct AstNode *node, struct XrType *type) {
    if (!analyzer || !node)
        return;
    xa_node_table_set_type((XaNodeTable *) analyzer->node_table, node, type);
}

struct XrType *xa_analyzer_get_node_type(XaAnalyzer *analyzer, const struct AstNode *node) {
    if (!analyzer || !node)
        return NULL;
    return xa_node_table_get_type((XaNodeTable *) analyzer->node_table, node);
}

void xa_analyzer_remove_file(XaAnalyzer *analyzer, const char *file) {
    if (!analyzer || !file)
        return;

    // This is the unified file-removal entry. It must keep the
    // analyzer's three "size-of-everything" counters self-consistent:
    //
    //   files_map.size  ==  file_count       (one entry per tracked file)
    //   dep_graph edges only reference live symbol IDs
    //   symbol_table holds no symbols owned by `file` after return
    //
    // Order matters: collect dependency-graph edges to drop FIRST (we
    // need the symbols' ids while they are still live), then remove the
    // symbols, then remove the file entry, then clear diagnostics.

    XaIncrementalCtx *incr = (XaIncrementalCtx *) analyzer->incremental;
    int edges_before = (incr && incr->deps) ? incr->deps->edge_count : 0;
    int file_count_before = analyzer->file_count;

    // Step 1: collect symbol IDs owned by `file`. We need them BEFORE
    // remove_file_symbols() destroys the symbols.
    int total_count = 0;
    XaSymbol **all_syms = xa_scope_get_all_symbols(analyzer->global_scope, &total_count);
    uint32_t *file_ids = NULL;
    int file_id_count = 0;
    if (all_syms && total_count > 0) {
        file_ids = xr_malloc(sizeof(uint32_t) * total_count);
        if (file_ids) {
            for (int i = 0; i < total_count; i++) {
                XaSymbol *sym = all_syms[i];
                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
                if (links && links->file_path && strcmp(links->file_path, file) == 0) {
                    file_ids[file_id_count++] = sym->id;
                }
            }
        }
        xr_free(all_syms);
    }

    // Step 2: drop dependency-graph edges that touch any of those symbols.
    if (incr && file_ids && file_id_count > 0) {
        xa_dep_remove_symbols(incr, file_ids, file_id_count);
    }
    xr_free(file_ids);

    // Step 3: remove symbols owned by this file from the symbol table.
    remove_file_symbols(analyzer->global_scope, file, analyzer);

    // Step 4: remove file from hash map and linked list.
    XrHashMap *fmap = (XrHashMap *) analyzer->files_map;
    bool was_tracked = (fmap && xr_hashmap_get(fmap, file) != NULL);
    if (fmap)
        xr_hashmap_delete(fmap, file);

    XaFileEntry **pp = &analyzer->files;
    while (*pp) {
        if ((*pp)->path && strcmp((*pp)->path, file) == 0) {
            XaFileEntry *to_free = *pp;
            *pp = to_free->next;
            xr_free((void *) to_free->path);
            xr_free(to_free);
            analyzer->file_count--;
            break;
        }
        pp = &(*pp)->next;
    }

    // Step 5: clear diagnostics for any subsequent re-analysis.
    xa_analyzer_clear_diagnostics(analyzer);

    // Step 6: invariants. file_count drops by exactly one when the file
    // was tracked, otherwise stays put. Dep-graph edge count never grows
    // during a removal.
    XR_DCHECK(analyzer->file_count == (was_tracked ? file_count_before - 1 : file_count_before),
              "xa_analyzer_remove_file: file_count drift");
    XR_DCHECK(!incr || !incr->deps || incr->deps->edge_count <= edges_before,
              "xa_analyzer_remove_file: dep edge_count grew during removal");
    (void) was_tracked;
    (void) file_count_before;
    (void) edges_before;
}

// Helper: find symbol at position with file filter
static XaSymbol *find_symbol_at_position_in_file(XaScope *scope, const char *file, uint32_t line,
                                                 uint32_t column) {
    if (!scope)
        return NULL;

    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(scope, &count);

    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym)
            continue;

        // Filter by file if specified
        if (file && sym->location.file && strcmp(sym->location.file, file) != 0) {
            continue;
        }

        // Check if position is within symbol
        if (sym->location.line == line) {
            uint32_t sym_end = sym->location.column + (sym->name ? strlen(sym->name) : 0);
            if (column >= sym->location.column && column <= sym_end) {
                xr_free(symbols);
                return sym;
            }
        }
    }
    xr_free(symbols);

    // Search child scopes
    for (int i = 0; i < scope->child_count; i++) {
        XaSymbol *found = find_symbol_at_position_in_file(scope->children[i], file, line, column);
        if (found)
            return found;
    }

    return NULL;
}

XaSymbol *xa_analyzer_lookup_at(XaAnalyzer *analyzer, const char *file, uint32_t line,
                                uint32_t column) {
    if (!analyzer || !analyzer->global_scope)
        return NULL;

    return find_symbol_at_position_in_file(analyzer->global_scope, file, line, column);
}

XrType *xa_analyzer_get_type_at(XaAnalyzer *analyzer, const char *file, uint32_t line,
                                uint32_t column) {
    XaSymbol *sym = xa_analyzer_lookup_at(analyzer, file, line, column);
    if (!sym)
        return NULL;

    return xa_analyzer_get_type(analyzer, sym);
}

XrType *xa_analyzer_infer_expr_type(XaAnalyzer *analyzer, XrAstNode *expr) {
    if (!analyzer || !expr)
        return NULL;

    // Ensure type pool is set for this thread
    xr_type_set_current_pool(analyzer->type_pool, &analyzer->next_symbol_id);

    // Create temporary inference context
    XaInferContext *ctx = xa_infer_context_new(analyzer);
    if (!ctx)
        return xr_type_new_unknown(NULL);

    // Infer expression type
    XrType *type = xa_visit_infer_expr(ctx, expr);

    xa_infer_context_free(ctx);
    return type ? type : xr_type_new_unknown(NULL);
}

// Variable operations (compatible with ct_infer API)
XrType *xa_analyzer_lookup_var(XaAnalyzer *analyzer, const char *name) {
    if (!analyzer || !name)
        return NULL;

    XaSymbol *sym = xa_scope_lookup(analyzer->current_scope, name);
    if (!sym)
        return NULL;

    return xa_analyzer_get_type(analyzer, sym);
}

void xa_analyzer_define_var(XaAnalyzer *analyzer, const char *name, XrType *type) {
    if (!analyzer || !name)
        return;

    XaSymbol *sym = xa_symbol_new(name, XA_SYM_VARIABLE);
    xa_scope_add_symbol(analyzer->current_scope, sym);

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links) {
        links->type = type;
        links->declared_type = type;
    }
}

// ============================================================================
// LSP Support: Find References
// ============================================================================

// NOTE: Reference collection is done via links->references (stored during analysis)
// The old collect_refs_in_scope was dead code with incorrect logic (name-based instead of ID-based)

XaSymbolRef *xa_analyzer_find_references(XaAnalyzer *analyzer, const char *name,
                                         bool include_definition, int *count) {
    *count = 0;
    if (!analyzer || !name || !analyzer->global_scope)
        return NULL;

    XaSymbolRef *refs = NULL;

    // First find the symbol definition
    XaSymbol *sym = xa_scope_lookup(analyzer->global_scope, name);
    if (!sym)
        return NULL;

    // Add definition location if requested
    if (include_definition) {
        XaSymbolRef *def_ref = xr_calloc(1, sizeof(XaSymbolRef));
        if (def_ref) {
            def_ref->file = sym->location.file;
            def_ref->line = sym->location.line;
            def_ref->column = sym->location.column;
            def_ref->end_column = sym->location.column + (sym->name ? strlen(sym->name) : 0);
            def_ref->is_definition = true;
            def_ref->is_write = false;
            def_ref->next = refs;
            refs = def_ref;
            (*count)++;
        }
    }

    // Get collected references from symbol links
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    if (links && links->references) {
        XaRefLocation *loc = links->references;
        while (loc) {
            XaSymbolRef *ref = xr_calloc(1, sizeof(XaSymbolRef));
            if (ref) {
                ref->file = sym->location.file;  // Same file as definition
                ref->line = loc->line;
                ref->column = loc->column;
                ref->end_column = loc->end_column;
                ref->is_definition = false;
                ref->is_write = loc->is_write;
                ref->next = refs;
                refs = ref;
                (*count)++;
            }
            loc = loc->next;
        }
    }

    return refs;
}

XaSymbolRef *xa_analyzer_find_references_at(XaAnalyzer *analyzer, const char *file, uint32_t line,
                                            uint32_t column, int *count) {
    *count = 0;
    if (!analyzer)
        return NULL;

    // Find symbol at position
    XaSymbol *sym = xa_analyzer_lookup_at(analyzer, file, line, column);
    if (!sym || !sym->name)
        return NULL;

    // Find all references to this symbol
    return xa_analyzer_find_references(analyzer, sym->name, true, count);
}

void xa_analyzer_free_references(XaSymbolRef *refs) {
    while (refs) {
        XaSymbolRef *next = refs->next;
        xr_free(refs);
        refs = next;
    }
}

bool xa_analyzer_can_rename(XaAnalyzer *analyzer, const char *file, uint32_t line, uint32_t column,
                            char **out_symbol_name) {
    if (!analyzer || !out_symbol_name)
        return false;
    *out_symbol_name = NULL;

    // Find symbol at position
    XaSymbol *sym = xa_analyzer_lookup_at(analyzer, file, line, column);
    if (!sym || !sym->name)
        return false;

    // Check if symbol can be renamed (not builtin)
    if (sym->is_builtin)
        return false;

    // Return symbol name
    *out_symbol_name = xr_strdup(sym->name);
    return true;
}

// ============================================================================
// Iterable/Iterator Structural Type Checking (with analyzer context)
// ============================================================================

// Helper: get method return type from class
static XrType *get_method_return_type(XaAnalyzer *analyzer, XrClassInfo *info,
                                      const char *method_name) {
    if (!analyzer || !info || !method_name)
        return NULL;

    // Search in class and base classes
    for (XrClassInfo *c = info; c != NULL; c = c->base) {
        for (int i = 0; i < c->method_count; i++) {
            XaSymbol *method = c->methods[i];
            if (method && method->name && strcmp(method->name, method_name) == 0) {
                // Get type from symbol links
                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, method);
                if (links && links->return_type) {
                    return links->return_type;
                }
                // Fallback to computed type
                if (links && links->type && (links->type->kind == XR_KIND_FUNCTION)) {
                    return links->type->function.return_type;
                }
            }
        }
    }
    return NULL;
}

// Helper: resolve XrClassInfo from a class or instance type via class_name lookup
static XrClassInfo *resolve_class_info(XaAnalyzer *analyzer, XrType *type) {
    if (!analyzer || !type)
        return NULL;
    if (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS)
        return NULL;
    if (type->instance.class_ref)
        return type->instance.class_ref;
    if (!type->instance.class_name)
        return NULL;
    // Search from current scope up (class may be in module or local scope)
    XaSymbol *sym = xa_analyzer_lookup(analyzer, type->instance.class_name);
    if (!sym || sym->kind != XA_SYM_CLASS)
        return NULL;
    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
    return (links && links->class_info) ? links->class_info : NULL;
}

// Check if type satisfies Iterator<T> (has hasNext(): bool and next(): T)
bool xa_analyzer_is_iterator(XaAnalyzer *analyzer, XrType *type, XrType **out_element_type) {
    if (!analyzer || !type)
        return false;

    // Must be a class or instance type
    if (type->kind != XR_KIND_INSTANCE && type->kind != XR_KIND_CLASS)
        return false;

    XrClassInfo *info = resolve_class_info(analyzer, type);
    if (!info)
        return false;

    // Check hasNext() method returns bool
    XrType *has_next_ret = get_method_return_type(analyzer, info, "hasNext");
    if (!has_next_ret || !(has_next_ret->kind == XR_KIND_BOOL)) {
        return false;
    }

    // Check next() method exists
    XrType *next_ret = get_method_return_type(analyzer, info, "next");
    if (!next_ret) {
        return false;
    }

    // Element type is the return type of next()
    if (out_element_type) {
        *out_element_type = next_ret;
    }
    return true;
}

// Check if type satisfies Iterable<T> (built-in or has iterator() -> Iterator<T>)
bool xa_analyzer_is_iterable(XaAnalyzer *analyzer, XrType *type, XrType **out_element_type) {
    if (!type)
        return false;

    // First check built-in iterable types (doesn't need analyzer)
    if (xr_type_is_iterable(type, out_element_type)) {
        return true;
    }

    // Custom class: check if it has iterator() method returning Iterator<T>
    if (analyzer && (type->kind == XR_KIND_INSTANCE || type->kind == XR_KIND_CLASS)) {
        XrClassInfo *info = resolve_class_info(analyzer, type);
        if (!info)
            return false;

        XrType *iter_ret = get_method_return_type(analyzer, info, "iterator");
        if (iter_ret) {
            // Check if the return type satisfies Iterator<T>
            XrType *elem_type = NULL;
            if (xa_analyzer_is_iterator(analyzer, iter_ret, &elem_type)) {
                if (out_element_type) {
                    *out_element_type = elem_type;
                }
                return true;
            }
        }
    }

    return false;
}
