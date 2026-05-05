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

/* ========== Generic / Function Param Helpers ========== */

// Generic type parameter (for <T: Constraint> syntax)
typedef struct XrGenericParam {
    char *name;          // Type parameter name: T, U, K, V
    XrTypeRef *constraint;  // Constraint type (can be NULL)
} XrGenericParam;

// Function parameter passing mode for struct value types
#define XR_PARAM_VALUE 0  // Default: deep copy at function entry
#define XR_PARAM_IN 1     // Readonly reference: no copy, no mutation allowed
#define XR_PARAM_REF 2    // Mutable reference: no copy, mutation reflected to caller

// Function parameter node — each parameter has its own position info for LSP.
typedef struct XrParamNode {
    char *name;                     // Parameter name
    int line;                       // Line number (1-indexed)
    int column;                     // Column number (1-indexed, for LSP)
    uint8_t passing_mode;           // XR_PARAM_VALUE / XR_PARAM_IN / XR_PARAM_REF
    XrTypeRef *type;                   // Type annotation (can be NULL)
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
    XrAttribute **attributes;
    int attr_count;
    XrGenericParam **type_params;
    int type_param_count;
    uint32_t symbol_id;  // Unique ID from analyzer; 0 = unresolved / anonymous
} FunctionDeclNode;

/* ========== OOP: class / struct / interface / methods ========== */

typedef struct ClassDeclNode {
    char *name;
    char *super_name;
    char *super_module;  // Parent class module (extends module.Class)
    char **interfaces;
    int interface_count;
    AstNode **fields;
    int field_count;
    AstNode **methods;
    int method_count;
    bool is_abstract;
    bool is_final;
    XrGenericParam **type_params;  // Generic type parameters
    int type_param_count;
    uint32_t symbol_id;  // Unique ID from analyzer; 0 = unresolved
} ClassDeclNode;

typedef struct InterfaceMethodNode {
    char *name;
    char **parameters;
    XrTypeRef **param_types;
    int param_count;
    XrTypeRef *return_type;
} InterfaceMethodNode;

typedef struct InterfaceDeclNode {
    char *name;
    char **extends;
    int extends_count;
    AstNode **methods;
    int method_count;
} InterfaceDeclNode;

typedef struct FieldDeclNode {
    char *name;
    XrTypeRef *field_type;
    bool is_private;
    bool is_static;
    bool is_final;
    AstNode *initializer;
} FieldDeclNode;

// Method declaration node
// Supports generic methods: add<T>(item: T): void { ... }
typedef struct MethodDeclNode {
    char *name;
    char **parameters;
    XrTypeRef **param_types;
    uint8_t *param_passing_modes;  // XR_PARAM_VALUE / XR_PARAM_IN / XR_PARAM_REF per param
    int param_count;
    XrTypeRef *return_type;
    AstNode *body;
    bool is_constructor;
    bool is_static;
    bool is_private;
    bool is_getter;
    bool is_setter;
    bool is_abstract;
    bool is_final;
    bool is_static_constructor;
    AstNode **base_args;
    int base_arg_count;
    AstNode **default_values;
    bool is_operator;
    OperatorType op_type;
    char **type_param_names;  // Generic type parameters
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
    int arg_count;
    XrTypeRef **type_args;
    int type_arg_count;
} NewExprNode;

typedef struct ThisExprNode {
    int placeholder;
} ThisExprNode;

typedef struct SuperCallNode {
    char *method_name;
    AstNode **arguments;
    int arg_count;
} SuperCallNode;

/* ========== Type Alias ========== */

typedef struct TypeAliasNode {
    char *name;
    char **field_names;
    XrTypeRef **field_types;
    bool *field_optional;
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
    AstNode *value;
} EnumMemberNode;

typedef struct EnumDeclNode {
    char *name;
    char *type_hint;
    AstNode **members;
    int member_count;
} EnumDeclNode;

typedef struct EnumAccessNode {
    char *enum_name;
    char *member_name;
} EnumAccessNode;

typedef struct EnumConvertNode {
    char *enum_name;
    AstNode *value_expr;
} EnumConvertNode;

// Enum index node (compiler-generated for for-in desugaring)
typedef struct EnumIndexNode {
    AstNode *collection;  // enum type expression
    AstNode *index_expr;  // index expression
} EnumIndexNode;

/* ========== Module System ========== */

// Import member (selective imports)
typedef struct ImportMember {
    char *name;   // Original name
    char *alias;  // Alias (optional, import { foo as bar })
    uint32_t symbol_id;  // Analyzer-assigned unique ID (for upvalue capture)
} ImportMember;

// Import statement node — supports two forms:
//   1. import "module" as name      (whole module import)
//   2. import { a, b as c } from "module"  (selective import)
typedef struct ImportStmtNode {
    char *module_name;  // Module path/name
    char *alias;        // Alias for whole module import
    ImportType import_type;
    ImportMember *members;  // Selective import member list
    int member_count;       // 0 means whole module import
    uint32_t symbol_id;     // Analyzer-assigned unique ID (whole-module import)
} ImportStmtNode;

// Re-export member structure: export { a, b as c } from "./file"
typedef struct ReexportMember {
    char *name;
    char *alias;
} ReexportMember;

// Export statement node — three forms:
//   1. export let/const/fn/class ...
//   2. export a, b, c
//   3. export { a, b as c } from "./file"
typedef struct ExportStmtNode {
    AstNode *declaration;  // Declaration export (export let x = 1)
    char *export_name;     // Single export name (declaration style)
    char **export_names;   // Export name list (list style: export a, b)
    int export_count;

    // Re-export support
    char *from_path;  // Source module path (e.g. "./user")
    ReexportMember *reexport_members;
    int reexport_count;
    bool is_reexport_all;  // Whether it's `export * from "..."`
} ExportStmtNode;

#endif  // XAST_NODES_DECL_H
