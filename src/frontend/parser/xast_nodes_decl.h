/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_nodes_decl.h - Declaration / OOP / module-system AST nodes
 *
 * KEY CONCEPT (P-03):
 *   Topic header for declaration-shaped AST node payloads. Pull in only
 *   via xast_nodes.h.
 */

#ifndef XAST_NODES_DECL_H
#define XAST_NODES_DECL_H

#include "xast_nodes_common.h"
#include "../../shared/xr_param_mode.h"

/* ========== Generic / Function Param Helpers ========== */

// Generic type parameter (for <T: Constraint1 & Constraint2> syntax)
typedef struct XrGenericParam {
    char *name;               // Type parameter name: T, U, K, V
    XrTypeRef **constraints;  // Constraint types joined by '&' (NULL if none)
    int constraint_count;     // Number of constraints (0 if unconstrained)
} XrGenericParam;

// Function parameter node — each parameter has its own position info for LSP.
typedef struct XrParamNode {
    char *name;                     // Parameter name
    int line;                       // Line number (1-indexed)
    int column;                     // Column number (1-indexed, for LSP)
    XrParamMode passing_mode;       // read / ref / move parameter contract
    XrTypeRef *type;                // Type annotation (can be NULL)
    AstNode *default_value;         // Default value expression (can be NULL)
    XrDestructurePattern *pattern;  // Destructure pattern (can be NULL)
    bool is_rest;                   // Is this a rest parameter (...args)?
    uint32_t symbol_id;             // Unique ID from analyzer; 0 = unresolved
} XrParamNode;

/* ========== Function Declarations ========== */

typedef struct FunctionDeclNode {
    char *name;
    XrParamNode **params;
    int param_count;
    int required_count;  // Number of required params (without defaults)
    XrTypeRef *return_type;
    AstNode *body;
    bool is_generator;
    bool is_extern;          // bodyless declaration from an extern "C" block
    const char *extern_abi;  // dedicated ABI metadata; never an attribute
    XrAttribute **attributes;
    int attr_count;
    XrGenericParam **type_params;
    int type_param_count;
    uint32_t symbol_id;        // Unique ID from analyzer; 0 = unresolved / anonymous
    XrTypeRef **throws_types;  // Optional `throws E1 | E2` error type annotations
    int throws_count;          // Number of throws type refs (0 = not annotated)
} FunctionDeclNode;

/* ========== OOP: class / struct / interface / methods ========== */

typedef struct ClassDeclNode {
    char *name;
    char *super_name;
    char *super_module;      // Parent class module (extends module.Class)
    XrTypeRef **interfaces;  // Implemented interfaces (e.g. Iterable<int>, Comparable)
    int interface_count;
    AstNode **fields;
    int field_count;
    AstNode **methods;
    int method_count;
    bool explicit_final;       // User-visible final class contract; not inferred-final evidence
    bool is_packed;            // struct-only: `packed struct`
    uint32_t explicit_align;   // struct-only: `struct S align(N)`, 0 = natural
    XrAttribute **attributes;  // Declaration attributes
    int attr_count;
    XrGenericParam **type_params;  // Generic type parameters
    int type_param_count;
    uint32_t symbol_id;  // Unique ID from analyzer; 0 = unresolved
    /* Monomorphization metadata (set by xa_mono_pass) */
    bool is_monomorphized;             // true for cloned generic instances (e.g. Box$i64)
    bool is_generic_skeleton;          // true for original generic decl kept as skeleton
    bool mono_types_rewritten;         // mono pass rewrote a member type annotation
                                       // (Box<int> -> Box$i64); the class info collected
                                       // before the mono pass is stale and must be
                                       // re-collected by the post-mono analysis pass
    char *generic_origin_name;         // Original generic class name (e.g. "Box"), NULL if not mono
    char *display_name;                // User-visible name without mangling suffix
    const char **mono_type_arg_names;  // Concrete type display names (e.g. ["int","string"]), NULL
                                       // if not mono
    int mono_type_arg_count;           // Element count of mono_type_arg_names
} ClassDeclNode;

// Method signature inside an interface body. Supports generic methods with
// their own constraints: `wrap<T: Hashable>(x: T) -> int`.
typedef struct InterfaceMethodNode {
    char *name;
    XrParamNode **params;
    int param_count;
    XrTypeRef *return_type;
    XrAttribute **attributes;
    int attr_count;
    XrGenericParam **type_params;  // Method-local generic type parameters (with constraints)
    int type_param_count;
} InterfaceMethodNode;

// Property signature inside an interface body, e.g. `length: int` or
// `const fd: int`. The `is_readonly` flag is set when the declaration is
// prefixed with `const`, mirroring object-type field syntax.
typedef struct InterfacePropertyNode {
    char *name;
    XrTypeRef *prop_type;
    bool is_readonly;
} InterfacePropertyNode;

typedef struct InterfaceDeclNode {
    char *name;
    XrTypeRef **extends;  // Parent interfaces (e.g. Pair<K, V>)
    int extends_count;
    AstNode **methods;  // AST_INTERFACE_METHOD nodes
    int method_count;
    AstNode **properties;  // AST_INTERFACE_PROPERTY nodes
    int property_count;
    XrGenericParam **type_params;  // Generic type parameters (e.g. `interface Iterable<T>`)
    int type_param_count;
} InterfaceDeclNode;

typedef struct FieldDeclNode {
    char *name;
    XrTypeRef *field_type;
    bool is_private;
    bool is_protected;
    bool is_static;
    bool is_final;
    bool is_const;     // immutable field (assignable only in the constructor)
    bool is_flexible;  // extern-layout unsized tail: `name: flex T`
    // `weak parent: Node?` — the slot does not keep its target alive. A
    // STORAGE modifier, not a type: it says how this slot holds the value, so
    // there is no Weak<T> in the type table and no second unwrap at the use
    // site. The only mechanism that breaks a reference cycle (spec 16.8).
    bool is_weak;
    AstNode *initializer;
} FieldDeclNode;

// Method declaration node
// Supports generic methods: add<T: Hashable>(item: T): void { ... }
typedef struct MethodDeclNode {
    char *name;
    XrParamNode **params;
    int param_count;
    XrTypeRef *return_type;
    AstNode *body;
    bool is_constructor;
    bool is_static;
    bool is_private;
    bool is_protected;
    bool is_getter;
    bool is_setter;
    bool is_static_constructor;
    bool is_variadic;
    XrAttribute **attributes;
    int attr_count;
    int required_count;
    AstNode **base_args;
    int base_arg_count;
    bool is_operator;
    OperatorType op_type;
    XrGenericParam **type_params;  // Method-local generic type parameters (with constraints)
    int type_param_count;
} MethodDeclNode;

// new expression node
//   new ClassName()           - module_name = NULL
//   new module.ClassName()    - module_name = "module"
//   new Box<int>(42)          - generic type arguments
typedef struct NewExprNode {
    char *module_name;
    char *class_name;
    AstNode **arguments;
    XrCallArgAccess *arg_accesses;
    int arg_count;
    XrTypeRef **type_args;
    int type_arg_count;
    /* Parser-confirmed generic type namespace (`Result<int>.variants`).
     * This stays distinct from zero-argument construction (`Result<int>()`). */
    bool is_type_namespace;
} NewExprNode;

typedef struct ThisExprNode {
    int placeholder;
} ThisExprNode;

typedef struct SuperCallNode {
    char *method_name;
    AstNode **arguments;
    XrCallArgAccess *arg_accesses;
    int arg_count;
} SuperCallNode;

/* ========== Type Alias ========== */

typedef struct TypeAliasNode {
    char *name;
    uint32_t symbol_id; /* Analyzer-assigned unique ID */
    XrGenericParam **type_params;
    int type_param_count;
    char **field_names;
    XrTypeRef **field_types;
    int field_count;
    // Parser stores the fully-resolved RHS type here so the analyzer
    // can pick it up without re-resolving. May be NULL when the alias
    // body is anonymous-object only (in which case field_names /
    // field_types describe the shape).
    XrTypeRef *resolved_type;
} TypeAliasNode;

/* ========== Enum ========== */

typedef struct EnumMemberNode {
    char *name;
    /* ADT variant payload fields (positional or named) */
    char **payload_names;      /* field names; NULL entry = positional */
    XrTypeRef **payload_types; /* type annotations per field */
    int payload_count;         /* 0 = no payload (simple variant) */
} EnumMemberNode;

typedef struct EnumDeclNode {
    char *name;
    AstNode **members; /* AST_ENUM_MEMBER variants */
    int member_count;
    AstNode **methods; /* AST_METHOD_DECL nodes inside enum body */
    int method_count;
    XrAttribute **attributes; /* Declaration attributes */
    int attr_count;
    XrGenericParam **type_params; /* <T, E> generic type parameters */
    int type_param_count;
    XrTypeRef **interfaces; /* implements Foo, Bar */
    int interface_count;
    uint32_t symbol_id; /* Analyzer-assigned unique ID */
} EnumDeclNode;

typedef struct EnumAccessNode {
    char *enum_name;
    char *member_name;
} EnumAccessNode;

// Enum index node (compiler-generated for for-in desugaring)
typedef struct EnumIndexNode {
    AstNode *collection;  // enum type expression
    AstNode *index_expr;  // index expression
} EnumIndexNode;

/* ========== Module System ========== */

// Import member (selective imports)
typedef struct ImportMember {
    char *name;          // Original name
    char *alias;         // Alias (optional, import { foo as bar })
    uint32_t symbol_id;  // Analyzer-assigned unique ID (for upvalue capture)
} ImportMember;

// Import statement node — supports two forms:
//   1. import "module" as name      (whole module import)
//   2. import { a, b as c } from "module"  (selective import)
typedef struct ImportStmtNode {
    char *module_name;      // Module path/name (without quotes)
    char *alias;            // Alias for whole module import
    bool is_quoted;         // true if the specifier was a string literal (needs quotes in output)
    ImportMember *members;  // Selective import member list
    int member_count;       // 0 means whole module import
    uint32_t symbol_id;     // Analyzer-assigned unique ID (whole-module import)
} ImportStmtNode;

// Re-export member structure: export { a, b as c } from "./file"
typedef struct ReexportMember {
    char *name;
    char *alias;
} ReexportMember;

// Re-export declaration. Direct declaration visibility lives on AstNode.
typedef struct ExportStmtNode {
    char *from_path;      // Source module path (e.g. "./user")
    bool from_is_quoted;  // false for a bare stdlib module identity
    ReexportMember *reexport_members;
    int reexport_count;
    bool is_reexport_all;  // Whether it's `export * from "..."`
} ExportStmtNode;

typedef struct GlobalAsmNode {
    char *text;  // Decoded assembly template text.
} GlobalAsmNode;

#endif  // XAST_NODES_DECL_H
