/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_symbol.h - Symbol and scope definitions for type analysis
 *
 * KEY CONCEPT:
 *   Symbols represent named entities (variables, functions, classes) with
 *   their types and locations. Scopes form a tree structure for name lookup.
 *
 * WHY THIS DESIGN:
 *   - Separate Symbol from SymbolLinks for lazy type computation
 *   - Hierarchical scopes for proper name resolution
 *   - Support for type narrowing in control flow
 */

#ifndef XANALYZER_SYMBOL_H
#define XANALYZER_SYMBOL_H

#include "../../runtime/value/xtype.h"
#include "../../runtime/class/xclass_info.h"
#include "../../base/xdefs.h"
#include "../../base/xhashmap.h"
#include "../../base/xlocation.h"

// Symbol kinds
typedef enum XaSymbolKind {
    XA_SYM_VARIABLE,    // let/const variable (is_const=true for const)
    XA_SYM_FUNCTION,    // Function definition
    XA_SYM_CLASS,       // Class definition
    XA_SYM_FIELD,       // Class field
    XA_SYM_PROPERTY,    // Class property
    XA_SYM_METHOD,      // Class method
    XA_SYM_PARAMETER,   // Function parameter
    XA_SYM_IMPORT,      // Imported symbol
    XA_SYM_MODULE,      // Module namespace
    XA_SYM_ENUM,        // Enum declaration
    XA_SYM_TYPE_ALIAS,  // Type alias (type Point = {x: int, y: int})
} XaSymbolKind;

// Move state for shared let variables (Move semantics)
typedef enum XaMoveState {
    XA_MOVE_NOT_MOVED,    // Variable is owned, can be used
    XA_MOVE_MOVED,        // Variable has been moved, cannot be used
    XA_MOVE_MAYBE_MOVED,  // Variable may have been moved (conditional branch)
} XaMoveState;

// Type stability for JIT optimization
typedef enum XaTypeStability {
    XA_TYPE_STABLE,       // Type never changes after init (JIT can specialize)
    XA_TYPE_NARROWED,     // Type changes via narrowing only (predictable)
    XA_TYPE_POLYMORPHIC,  // Type changes across assignments (needs guard)
} XaTypeStability;

// JIT certainty level for type information
typedef enum XaJitCertainty {
    XA_JIT_UNKNOWN = 0,   // No type info, JIT must use generic path
    XA_JIT_INFERRED = 1,  // Analyzer inferred, may be imprecise, JIT needs guard
    XA_JIT_DECLARED = 2,  // User annotated, JIT can trust with assert
    XA_JIT_PROVEN = 3,    // Proven invariant (e.g. const literal), no guard needed
} XaJitCertainty;

// Forward declarations (XrLocation/XrClassInfo/XaMethodSlot live in base/runtime layers)
typedef struct XaSymbol XaSymbol;
typedef struct XaScope XaScope;
typedef struct XaSymbolLinks XaSymbolLinks;

// Reference location (for Find References)
typedef struct XaRefLocation {
    uint32_t line;
    uint32_t column;
    uint32_t end_column;
    bool is_write;  // true if write access (assignment)
    struct XaRefLocation *next;
} XaRefLocation;

// Symbol links - type information stored inline inside XaSymbol.
// Access via sym->links (no intmap lookup required).
struct XaSymbolLinks {
    XrType *type;           // Computed type (NULL = not computed)
    XrType *declared_type;  // Explicitly declared type (from annotation)
    bool type_computed;     // Has type computation been attempted

    // Definite assignment tracking
    bool is_definitely_assigned;  // true if variable has been assigned a value

    // Move state for shared let variables (Move semantics)
    XaMoveState move_state;  // Current ownership state
    uint32_t moved_line;     // Line where variable was moved (for error message)
    uint32_t moved_column;   // Column where variable was moved

    // For functions
    XrType **param_types;
    const char **param_names;  // Parameter names (for inlay hints)
    int param_count;
    XrType *return_type;
    bool return_type_inferred;

    // Call-site inferred parameter types (for unannotated params)
    // Populated by xa_visit_call when callee has unannotated parameters.
    // NULL entry = not yet observed; non-NULL = inferred type from call-site.
    // If two call sites provide incompatible types, entry is set to unknown (conflict).
    XrType **inferred_param_types;
    int inferred_param_count;

    // For generic functions/classes
    const char **type_param_names;    // Type parameter names (e.g., "T", "U")
    XrType **type_param_constraints;  // Constraints (e.g., Comparable), NULL if none
    int type_param_count;

    // For classes
    struct XrClassInfo *class_info;

    // For enum symbols (XA_SYM_ENUM)
    const char **enum_member_names;  // Enum member names (for exhaustiveness checking)
    int enum_member_count;

    // For module symbols (XA_SYM_MODULE)
    const char *module_name;  // Actual module name (may differ from variable name due to alias)

    // File ownership (for multi-file support)
    const char *file_path;  // File where this symbol is defined

    // JIT/AOT metadata
    XaTypeStability type_stability;  // How stable is this variable's type
    XaJitCertainty jit_certainty;    // How confident is the type info
    int assign_count;                // Number of assignments (for stability)
    bool is_const_foldable;          // const with literal init, can inline
    bool is_loop_variable;           // Defined/mutated inside a loop

    // Reference tracking (for LSP Find References)
    XaRefLocation *references;  // List of usage locations
    int ref_count;
};

// Symbol structure
struct XaSymbol {
    const char *name;     // Symbol name
    XaSymbolKind kind;    // Symbol kind
    uint32_t id;          // Unique ID (for symbol registry / LSP)
    XrLocation location;  // Definition location

    // Modifiers
    bool is_const;         // const declaration
    bool is_exported;      // export modifier
    bool is_static;        // static member
    bool is_private;       // private member (starts with _)
    bool is_shared;        // shared variable
    bool is_builtin;       // built-in type member (Array.push, etc.)
    uint8_t passing_mode;  // XR_PARAM_VALUE / XR_PARAM_IN / XR_PARAM_REF (for parameters)

    // Parent references
    XaScope *scope;    // Containing scope
    XaSymbol *parent;  // Parent symbol (for methods/fields)

    // For type aliases: direct type storage (avoids analyzer dependency)
    void *alias_type;  // XrType* for type aliases

    // Inline type information (replaces separate XaSymbolLinks + intmap lookup)
    XaSymbolLinks links;
};

// Scope kinds
typedef enum XaScopeKind {
    XA_SCOPE_GLOBAL,    // Global/module scope
    XA_SCOPE_FUNCTION,  // Function body
    XA_SCOPE_BLOCK,     // Block (if/while/for body)
    XA_SCOPE_CLASS,     // Class body
    XA_SCOPE_LOOP,      // Loop body (for break/continue)
} XaScopeKind;

// Scope structure
struct XaScope {
    XaScopeKind kind;
    XaScope *parent;     // Parent scope
    XaScope **children;  // Child scopes
    int child_count;
    int child_capacity;

    // Symbols in this scope (hash map: name -> XaSymbol*)
    void *symbols;  // XrHashMap internally

    // For function scopes
    XaSymbol *function_symbol;  // The function this scope belongs to

    // For class scopes
    XaSymbol *class_symbol;  // The class this scope belongs to

    // AST node association (for LSP rename, go-to-definition, etc.)
    void *ast_node;  // AstNode* that created this scope
};

// XrClassInfo and XaMethodSlot are defined in runtime/class/xclass_info.h
// (included above). Their APIs remain in this header.

// API: Set symbol ID counter (called by XaAnalyzer before analysis)
// This eliminates global state - each analyzer has its own counter
XR_FUNC void xa_symbol_set_id_counter(uint32_t *counter);

// API: Set symbol registry for O(1) ID lookup (called by XaAnalyzer)
XR_FUNC void xa_symbol_set_registry(void *intmap);

// API: Symbol creation
XR_FUNC XaSymbol *xa_symbol_new(const char *name, XaSymbolKind kind);
XR_FUNC void xa_symbol_free(XaSymbol *symbol);

// API: Scope creation
XR_FUNC XaScope *xa_scope_new(XaScopeKind kind, XaScope *parent);
XR_FUNC void xa_scope_free(XaScope *scope);

// API: Scope operations
XR_FUNC void xa_scope_add_symbol(XaScope *scope, XaSymbol *symbol);
XR_FUNC bool xa_scope_remove_symbol(XaScope *scope, const char *name);  // Returns true if removed
XR_FUNC XaSymbol *xa_scope_lookup(XaScope *scope, const char *name);
XR_FUNC XaSymbol *xa_scope_lookup_local(XaScope *scope, const char *name);
XR_FUNC XaSymbol *xa_scope_lookup_by_id(XaScope *scope, uint32_t id);
XR_FUNC XaSymbol **xa_scope_get_all_symbols(XaScope *scope, int *count);

// API: Scope hierarchy (for LSP rename)
XR_FUNC bool xa_scope_is_descendant(XaScope *child, XaScope *ancestor);
XR_FUNC XaScope *xa_scope_find_definition(XaScope *scope, const char *name);
XR_FUNC XaScope *xa_scope_find_by_node(XaScope *root, void *ast_node);

// API: Reference tracking
XR_FUNC void xa_symbol_add_ref(XaSymbolLinks *links, uint32_t line, uint32_t col, uint32_t end_col,
                               bool is_write);
XR_FUNC XaRefLocation *xa_symbol_get_refs(XaSymbolLinks *links, int *count);

// API: Class info
XR_FUNC XrClassInfo *xa_class_info_new(const char *name);
XR_FUNC void xa_class_info_free(XrClassInfo *info);
XR_FUNC void xa_class_info_add_field(XrClassInfo *info, XaSymbol *field);
XR_FUNC void xa_class_info_add_method(XrClassInfo *info, XaSymbol *method);
XR_FUNC XaSymbol *xa_class_info_lookup_member(XrClassInfo *info, const char *name);

// API: Function signature helpers (integrated into XaSymbolLinks)
XR_FUNC void xa_symbol_links_set_function_sig(XaSymbolLinks *links, XrType **param_types,
                                              const char **param_names, int param_count,
                                              XrType *return_type);
XR_FUNC XrType *xa_symbol_links_get_return_type(XaSymbolLinks *links);
XR_FUNC XrType **xa_symbol_links_get_param_types(XaSymbolLinks *links, int *count);
XR_FUNC const char **xa_symbol_links_get_param_names(XaSymbolLinks *links, int *count);
XR_FUNC bool xa_symbol_is_function(XaSymbol *symbol);

// API: Generic type parameters
XR_FUNC void xa_symbol_links_set_type_params(XaSymbolLinks *links, const char **names,
                                             XrType **constraints, int count);
XR_FUNC int xa_symbol_links_get_type_param_count(XaSymbolLinks *links);
XR_FUNC const char *xa_symbol_links_get_type_param_name(XaSymbolLinks *links, int index);
XR_FUNC XrType *xa_symbol_links_get_type_param_constraint(XaSymbolLinks *links, int index);

// Type alias helpers
XR_FUNC XaSymbol *xa_scope_define_type_alias(XaScope *scope, const char *name, void *type);
XR_FUNC void *xa_scope_resolve_type_alias(XaScope *scope, const char *name);
XR_FUNC bool xa_symbol_is_type_alias(XaSymbol *symbol);

#endif  // XANALYZER_SYMBOL_H
