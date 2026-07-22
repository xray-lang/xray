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

#include "xconsteval.h"
#include "xa_effect_db.h"
#include "xa_memory_effect_db.h"
#include "xa_alloc_effect.h"
#include "xa_intrinsic_registry.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xenum_layout.h"
#include "../../runtime/class/xclass_info.h"
#include "../../base/xdefs.h"
#include "../../base/xhashmap.h"
#include "../../base/xlocation.h"
#include "../../base/xstorage.h"

// Symbol kinds
typedef enum XaSymbolKind {
    XA_SYM_VARIABLE,    // var/const/shared variable (is_const=true for const)
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

// Move state for explicit ownership transfer of local variables.
typedef enum XaMoveState {
    XA_MOVE_NOT_MOVED,    // Variable is owned, can be used
    XA_MOVE_MOVED,        // Variable has been moved, cannot be used
    XA_MOVE_MAYBE_MOVED,  // Variable may have been moved (conditional branch)
} XaMoveState;

// Forward declarations (XrLocation/XrClassInfo/XaMethodSlot live in base/runtime layers)
typedef struct XaSymbol XaSymbol;
typedef struct XaScope XaScope;
typedef struct XaSymbolLinks XaSymbolLinks;
typedef struct XaEnumInfo XaEnumInfo;
struct AstNode;

typedef struct XaEnumVariantInfo {
    const char *name;
    int symbol;
    uint32_t tag;
    const char **payload_names;
    XrType **payload_types;
    uint16_t payload_count;
} XaEnumVariantInfo;

struct XaEnumInfo {
    const char *name;
    uint32_t variant_count;
    XaEnumVariantInfo *variants;
    bool is_payload_enum;
    XrEnumLayout *layout;
};

// Reference location (for Find References)
typedef struct XaRefLocation {
    uint32_t line;
    uint32_t column;
    uint32_t end_column;
    bool is_write;  // true if write access (assignment)
    struct XaRefLocation *next;
} XaRefLocation;

typedef struct XaOutFieldDaPath {
    char *path;
    struct XaOutFieldDaPath *next;
} XaOutFieldDaPath;

// Symbol links - type information stored inline inside XaSymbol.
// Access via sym->links (no intmap lookup required).
struct XaSymbolLinks {
    XrType *type;           // Computed type (NULL = not computed)
    XrType *declared_type;  // Explicitly declared type (from annotation)
    bool type_computed;     // Has type computation been attempted

    // Definite assignment tracking
    bool is_definitely_assigned;  // true if variable has been assigned a value
    XaOutFieldDaPath *out_field_da_paths;

    // Analyzer-side provenance for Ptr<T>/MutPtr<T> values. This is the
    // source-level lifetime proof consumed by escape checks before either VM
    // or AOT lowering; a raw pointer type alone never grants stable escape.
    XrAddressProvenance pointer_provenance;
    struct XaSymbol *pointer_owner_symbol;
    bool pointer_provenance_known;
    bool pointer_provenance_mixed;

    // Move state for explicit ownership transfer of local variables.
    XaMoveState move_state;  // Current ownership state
    uint32_t moved_line;     // Line where variable was moved (for error message)
    uint32_t moved_column;   // Column where variable was moved

    // For functions
    XrType **param_types;
    const char **param_names;  // Parameter names (for inlay hints)
    // Per-parameter default-value expressions (AstNode*), or NULL when the
    // parameter is required. Used for caller-side default argument filling:
    // an omitted trailing argument is completed at the call site with a
    // session-cloned copy of this expression (evaluated in the caller).
    struct AstNode **param_defaults;
    int param_count;
    uint8_t *param_escapes;  // Per-parameter summary: value may be stored/returned/captured
    int param_escape_count;
    uint8_t *param_mutations;  // Per-parameter summary: callee may write through the value graph
    int param_mutation_count;
    uint8_t *param_storage_requirements;  // XrStorageOwner required by callee body per parameter
    int param_storage_requirement_count;
    XrType *return_type;
    bool return_type_inferred;
    uint8_t return_storage_owner;  // XrStorageOwner for known owned/shared returns
    bool return_storage_known;
    bool return_storage_mixed;
    bool return_storage_scanned;
    bool return_storage_scan_in_progress;
    uint8_t *return_fn_param_escapes;  // Summary for a function value returned by this function
    int return_fn_param_escape_count;
    uint8_t *return_fn_param_mutations;
    int return_fn_param_mutation_count;
    uint8_t *return_fn_param_storage_requirements;
    int return_fn_param_storage_requirement_count;
    bool return_fn_effect_mixed;
    bool return_fn_effect_scanned;
    bool return_fn_effect_scan_in_progress;
    struct AstNode *function_decl_node;
    bool is_extern;    // extern-block foreign function (FFI): calls require unsafe { }
    bool is_c_export;  // @c_export AOT C ABI wrapper
    const char *c_export_symbol;
    XaEffectId effect_id;  // Canonical analyzer-owned effect summary id (0 = not inferred yet)
    XaMemoryEffectId memory_effect_id;  // Canonical root-relative memory effect summary id
    // Error-effect "may-throw" bit (task 216), derived from the effect summary
    // after the effect-DB fixpoint: NO_THROW iff the summary is complete AND its
    // escaping error set is empty; MAY_THROW otherwise (fail-closed default).
    // Mirrored onto the function type (links.type->function.throw_effect) and
    // consumed by IR lowering to emit error checks constructively.
    XrFnThrowEffect throw_effect;
    XaAllocEffectId alloc_effect_id;  // Canonical allocation summary (0 = not inferred yet)
    XaIntrinsicId intrinsic_id;       // canonical source-semantic identity, never name-derived
    /* Stable publication snapshot.  alloc_effect_id is local to one analyzer
     * database; these fields survive symbol cloning/import metadata and are
     * the cross-module contract surface. */
    XaAllocState alloc_state;
    XaAllocReasonSet alloc_reason_bits;
    uint64_t alloc_fingerprint;
    bool alloc_effect_complete;
    bool has_no_alloc_contract;
    // @no_throw assertion (task 216): the definition asserts it raises no error;
    // the analyzer verifies the effect summary is complete with an empty escaping
    // set after the effect-DB fixpoint, else it is a compile error.
    bool has_no_throw_contract;

    // Call-site inferred parameter types (for unannotated params)
    // Populated by xa_visit_call when callee has unannotated parameters.
    // NULL entry = not yet observed; non-NULL = inferred type from call-site.
    // If two call sites provide incompatible types, entry is set to unknown (conflict).
    XrType **inferred_param_types;
    int inferred_param_count;

    // For generic functions/classes
    const char **type_param_names;      // Type parameter names (e.g., "T", "U")
    XrType ***type_param_constraints;   // Per-param intersection constraint lists.
                                        // type_param_constraints[i] is a pointer to an
                                        // array of size type_param_constraint_counts[i].
                                        // NULL when a parameter has no constraints.
    int *type_param_constraint_counts;  // Number of constraints per parameter (0 = none)
    int type_param_count;

    // For classes
    struct XrClassInfo *class_info;

    // For enum symbols (XA_SYM_ENUM)
    XaEnumInfo *enum_info;

    // For module symbols and selective imports.
    const char *module_name;  // Actual module name (may differ from variable name due to alias)
    const char *import_member_name;  // Original exported member for selective imports.

    // File ownership (for multi-file support)
    const char *file_path;  // File where this symbol is defined

    int assign_count;                   // Number of assignments
    bool is_const_foldable;             // const with literal init, can inline
    struct AstNode *const_initializer;  // const initializer expression for compile-time eval
    bool has_ct_value;                  // const initializer proved to a compile-time value
    XrCtValue ct_value;                 // cached compile-time value
    bool is_comptime_local;             // binding exists only in a comptime block
    bool is_loop_variable;              // Defined/mutated inside a loop

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
    bool is_const;              // const declaration / immutable field
    bool is_rebindable;         // binding name may be assigned again
    bool is_readonly_binding;   // binding exposes deep-readonly semantics
    bool is_exported;           // export modifier
    bool is_static;             // static member
    bool is_private;            // private member (class-only visibility)
    bool is_protected;          // protected member (class + subclass visibility)
    bool is_override;           // analyzer-inferred exact-signature method override
    bool is_shared;             // shared variable
    bool is_shared_provenance;  // current value derives from a shared root
    bool is_owned;              // owned unique-root variable
    bool is_imported;           // selective import binding; kind remains the exported semantic kind
    bool is_builtin;            // built-in type member (Array.push, etc.)
    bool mutates_receiver;      // method body writes through `this`
    XrParamMode passing_mode;   // value / in / ref / out parameter contract
    uint32_t borrowed_root_symbol_id;  // local alias root for in/ref parameter borrowing

    // Parent references
    XaScope *scope;    // Containing scope
    XaSymbol *parent;  // Parent symbol (for methods/fields)

    // For type aliases: declaration-backed TypeRef plus optional resolved cache.
    struct AstNode *type_alias_node;
    void *alias_type;  // XrType* for type aliases
    bool alias_resolving;

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

    // Lookup-only aliases (hash map: alias -> existing XaSymbol*). Aliases
    // participate in lexical resolution but are intentionally omitted from
    // symbol enumeration, ownership, completion, and redeclaration storage.
    // REPL `it` uses this to point at the latest versioned result binding.
    void *aliases;  // XrHashMap internally

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
XR_FUNC void xa_scope_set_alias(XaScope *scope, const char *alias, XaSymbol *symbol);
XR_FUNC bool xa_scope_remove_alias(XaScope *scope, const char *alias);
XR_FUNC XaSymbol *xa_scope_lookup(XaScope *scope, const char *name);
XR_FUNC XaSymbol *xa_scope_lookup_local(XaScope *scope, const char *name);
XR_FUNC XaSymbol *xa_scope_lookup_by_id(XaScope *scope, uint32_t id);
XR_FUNC XaSymbol **xa_scope_get_all_symbols(XaScope *scope, int *count);
// Count symbols directly in this scope (non-recursive) without materialising
// the array. Use this when only the count is needed (e.g. progress logging).
XR_FUNC int xa_scope_count_symbols(XaScope *scope);

// API: Scope hierarchy (for LSP rename)
XR_FUNC bool xa_scope_is_descendant(XaScope *child, XaScope *ancestor);
XR_FUNC XaScope *xa_scope_find_definition(XaScope *scope, const char *name);
XR_FUNC XaScope *xa_scope_find_by_node(XaScope *root, void *ast_node);

// API: Reference tracking
XR_FUNC void xa_symbol_add_ref(XaSymbolLinks *links, uint32_t line, uint32_t col, uint32_t end_col,
                               bool is_write);
XR_FUNC XaRefLocation *xa_symbol_get_refs(XaSymbolLinks *links, int *count);
XR_FUNC void xa_symbol_links_mark_out_field_assigned(XaSymbolLinks *links, const char *path);
XR_FUNC bool xa_symbol_links_out_field_assigned(XaSymbolLinks *links, const char *path);
XR_FUNC bool xa_symbol_links_mark_out_whole_assigned_if_all_direct_fields_assigned_for_class(
    XaSymbolLinks *links, const char *root_name, XrClassInfo *info);
XR_FUNC bool xa_symbol_links_mark_out_whole_assigned_if_all_direct_fields_assigned_for_type(
    XaSymbolLinks *links, const char *root_name, XrType *type);
XR_FUNC bool xa_symbol_links_mark_out_field_assigned_if_all_direct_fields_assigned_for_class(
    XaSymbolLinks *links, const char *path_prefix, XrClassInfo *info);
XR_FUNC XaOutFieldDaPath *xa_symbol_links_clone_out_field_da_paths(XaSymbolLinks *links);
XR_FUNC void xa_symbol_links_restore_out_field_da_paths(XaSymbolLinks *links,
                                                        XaOutFieldDaPath *paths);
XR_FUNC XaOutFieldDaPath *xa_symbol_links_intersect_out_field_da_paths(XaOutFieldDaPath *left,
                                                                       XaOutFieldDaPath *right);
XR_FUNC void xa_symbol_links_free_out_field_da_paths(XaOutFieldDaPath *paths);

// API: Class info
XR_FUNC XrClassInfo *xa_class_info_new(const char *name);
XR_FUNC void xa_class_info_free(XrClassInfo *info);
XR_FUNC void xa_class_info_add_field(XrClassInfo *info, XaSymbol *field);
XR_FUNC void xa_class_info_add_method(XrClassInfo *info, XaSymbol *method);
XR_FUNC XaSymbol *xa_class_info_lookup_member(XrClassInfo *info, const char *name);
XR_FUNC XaSymbol *xa_class_info_lookup_instance_member(XrClassInfo *info, const char *name);
XR_FUNC XaSymbol *xa_class_info_lookup_static_member(XrClassInfo *info, const char *name);

// Same as xa_class_info_lookup_member but also reports which class in the base
// chain actually declares the member (used for private/protected visibility).
XR_FUNC XaSymbol *xa_class_info_lookup_member_owner(XrClassInfo *info, const char *name,
                                                    XrClassInfo **owner_out);
XR_FUNC XaSymbol *xa_class_info_lookup_instance_member_owner(XrClassInfo *info, const char *name,
                                                             XrClassInfo **owner_out);

// API: Enum metadata
XR_FUNC XaEnumInfo *xa_enum_info_new(const char *name, uint32_t variant_count);
XR_FUNC bool xa_enum_info_finalize_layout(XaEnumInfo *info);
XR_FUNC XaEnumInfo *xa_enum_info_clone(const XaEnumInfo *src);
XR_FUNC void xa_enum_info_free(XaEnumInfo *info);
XR_FUNC int xa_enum_info_find_variant(const XaEnumInfo *info, const char *name);
XR_FUNC XaSymbol *xa_class_info_lookup_static_member_owner(XrClassInfo *info, const char *name,
                                                           XrClassInfo **owner_out);

// API: Function signature helpers (integrated into XaSymbolLinks)
XR_FUNC void xa_symbol_links_set_function_sig(XaSymbolLinks *links, XrType **param_types,
                                              const char **param_names, int param_count,
                                              XrType *return_type);
// Record per-parameter default-value expressions (AstNode*, or NULL when the
// parameter is required) for caller-side default argument completion. The array
// is copied; passing all-NULL clears any stored defaults.
XR_FUNC void xa_symbol_links_set_param_defaults(XaSymbolLinks *links, struct AstNode **defaults,
                                                int count);
XR_FUNC XrType *xa_symbol_links_get_return_type(XaSymbolLinks *links);
XR_FUNC XrType **xa_symbol_links_get_param_types(XaSymbolLinks *links, int *count);
XR_FUNC const char **xa_symbol_links_get_param_names(XaSymbolLinks *links, int *count);
XR_FUNC bool xa_symbol_is_function(XaSymbol *symbol);
XR_FUNC void xa_symbol_links_copy_param_effect_summaries(XaSymbolLinks *dst,
                                                         const XaSymbolLinks *src);
XR_FUNC void xa_symbol_links_clear_return_function_effect_summary(XaSymbolLinks *links);
XR_FUNC void xa_symbol_links_set_return_function_effect_summary(XaSymbolLinks *dst,
                                                                const XaSymbolLinks *src);
XR_FUNC void xa_symbol_links_copy_return_function_effect_summary(XaSymbolLinks *dst,
                                                                 const XaSymbolLinks *src);
XR_FUNC void
xa_symbol_links_copy_return_function_effect_to_param_summaries(XaSymbolLinks *dst,
                                                               const XaSymbolLinks *src);

// API: Generic type parameters
//
// Each generic parameter (e.g. T, U) carries an intersection-style constraint
// list: a type satisfies the parameter only if it satisfies every constraint
// in the list.  An empty list means the parameter is unconstrained.
XR_FUNC void xa_symbol_links_set_type_params(XaSymbolLinks *links, const char **names,
                                             XrType ***constraint_lists,
                                             const int *constraint_counts, int count);
XR_FUNC int xa_symbol_links_get_type_param_count(XaSymbolLinks *links);
XR_FUNC const char *xa_symbol_links_get_type_param_name(XaSymbolLinks *links, int index);
XR_FUNC XrType **xa_symbol_links_get_type_param_constraints(XaSymbolLinks *links, int index,
                                                            int *out_count);
XR_FUNC void xa_symbol_links_copy_export_metadata(XaSymbolLinks *dst, const XaSymbolLinks *src);

// Type alias helpers
XR_FUNC XaSymbol *xa_scope_define_type_alias(XaScope *scope, const char *name, void *type);
XR_FUNC void *xa_scope_resolve_type_alias(XaScope *scope, const char *name);
XR_FUNC bool xa_symbol_is_type_alias(XaSymbol *symbol);

#endif  // XANALYZER_SYMBOL_H
