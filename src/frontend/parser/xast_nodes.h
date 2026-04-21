/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_nodes.h - AST node data structures
 *
 * KEY CONCEPT:
 *   Defines all AST node structures used by the parser.
 */

#ifndef XAST_NODES_H
#define XAST_NODES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "xast_types.h"
#include "../lexer/xlex.h"

typedef struct AstNode AstNode;
typedef struct XrType XrType;

typedef struct BinaryNode {
    AstNode *left;
    AstNode *right;
} BinaryNode;

typedef struct UnaryNode {
    AstNode *operand;
} UnaryNode;

// Literal node
typedef struct LiteralNode {
    LiteralKind kind;
    union {
        int64_t int_val;
        double float_val;
        const char *bigint_val;
        const char *string_val;
        bool bool_val;
        struct {
            const char *pattern;
            const char *flags;
        } regex;
    } raw_value;
} LiteralNode;

// Attribute node (test framework)
typedef struct XrAttribute {
    AttributeKind kind;
    int timeout;
} XrAttribute;

// Destructuring pattern structure (flat only, no nesting)
typedef struct XrDestructurePattern {
    PatternType type;

    union {
        struct {
            struct XrDestructurePattern **elements;
            int element_count;
        } array;

        struct {
            char **field_names;
            struct XrDestructurePattern **patterns;
            int field_count;
            bool use_shorthand;
        } object;

        struct {
            char *name;
            XrType *type;
        } identifier;
    } as;
} XrDestructurePattern;

// Program node
// arena: owns all AST memory for this program (set by xr_parse_*).
//        When owns_arena is true, xr_program_destroy destroys it.
//        When false (e.g. xr_parse_recoverable with caller-provided arena),
//        xr_program_destroy is a no-op.
struct XrArena;
typedef struct ProgramNode {
    AstNode **statements;
    int count;
    int capacity;
    struct XrArena *arena;
    bool owns_arena;
} ProgramNode;

// print statement node
typedef struct PrintNode {
    AstNode **exprs;
    int expr_count;
} PrintNode;

// Block node
typedef struct BlockNode {
    AstNode **statements;
    int count;
    int capacity;
} BlockNode;

// Variable declaration node
//
// Storage mode (storage_mode):
//   0 = normal variable (deep copy across coroutines)
//   1 = shared variable (stored in global heap, passed by reference)
//
// shared variable features:
//   - shared const: can be directly read concurrently by coroutine closures
//   - shared let: can only be accessed serially through Channel
typedef struct VarDeclNode {
    char *name;
    AstNode *initializer;
    bool is_const;
    uint8_t storage_mode;       // 0=normal, 1=shared
    XrType *type_annotation;
} VarDeclNode;

// Storage mode constants
#define XR_STORAGE_NORMAL  0
#define XR_STORAGE_SHARED  1

// Variable reference node
typedef struct VariableNode {
    char *name;
} VariableNode;

// Assignment node
typedef struct AssignmentNode {
    char *name;
    AstNode *value;
} AssignmentNode;

// Compound assignment node
typedef struct CompoundAssignmentNode {
    char *name;
    TokenType op;
    AstNode *value;
    AstNode *object;
} CompoundAssignmentNode;

// Increment/decrement node
typedef struct IncDecNode {
    char *name;
} IncDecNode;

// Destructure declaration node
typedef struct DestructureDeclNode {
    XrDestructurePattern *pattern;
    AstNode *initializer;
    bool is_const;
} DestructureDeclNode;

// Destructure assignment node
typedef struct DestructureAssignNode {
    XrDestructurePattern *pattern;
    AstNode *value;
} DestructureAssignNode;

// Multi-value declaration node
typedef struct MultiVarDeclNode {
    char **names;
    int name_count;
    AstNode **values;
    int value_count;
    bool is_const;
} MultiVarDeclNode;

// Multi-value assignment node
typedef struct MultiAssignNode {
    AstNode **targets;
    int target_count;
    AstNode **values;
    int value_count;
} MultiAssignNode;

// if statement node
typedef struct IfStmtNode {
    AstNode *condition;
    AstNode *then_branch;
    AstNode *else_branch;
} IfStmtNode;

// while loop node
typedef struct WhileStmtNode {
    AstNode *condition;
    AstNode *body;
} WhileStmtNode;

// for loop node
typedef struct ForStmtNode {
    AstNode *initializer;
    AstNode *condition;
    AstNode *increment;
    AstNode *body;
} ForStmtNode;

// for-in loop node
typedef struct ForInStmtNode {
    char *item_name;
    char *value_name;
    bool is_keyvalue;
    XrType *item_type;
    AstNode *collection;
    AstNode *body;
} ForInStmtNode;

// break statement node
typedef struct BreakStmtNode {
    int placeholder;
} BreakStmtNode;

// continue statement node
typedef struct ContinueStmtNode {
    int placeholder;
} ContinueStmtNode;

// try-catch-finally statement node
typedef struct TryCatchNode {
    AstNode *try_body;
    char *catch_var;
    int catch_var_line;      // Line of catch variable (1-indexed)
    int catch_var_column;    // Column of catch variable (1-indexed)
    AstNode *catch_body;
    AstNode *finally_body;
} TryCatchNode;

// throw statement node
typedef struct ThrowStmtNode {
    AstNode *expression;
} ThrowStmtNode;

// Generic type parameter (for <T: Constraint> syntax)
typedef struct XrGenericParam {
    char *name;                    // Type parameter name: T, U, K, V
    XrType *constraint;            // Constraint type (can be NULL)
} XrGenericParam;

// Function parameter node - each parameter has its own position info for LSP
// Parameter passing mode for struct value types
#define XR_PARAM_VALUE  0   // Default: deep copy at function entry
#define XR_PARAM_IN     1   // Readonly reference: no copy, no mutation allowed
#define XR_PARAM_REF    2   // Mutable reference: no copy, mutation reflected to caller

typedef struct XrParamNode {
    char *name;                    // Parameter name
    int line;                      // Line number (1-indexed)
    int column;                    // Column number (1-indexed, for LSP)
    uint8_t passing_mode;          // XR_PARAM_VALUE / XR_PARAM_IN / XR_PARAM_REF
    XrType *type;                  // Type annotation (can be NULL)
    AstNode *default_value;        // Default value expression (can be NULL)
    XrDestructurePattern *pattern; // Destructure pattern (can be NULL)
    bool is_rest;                  // Is this a rest parameter (...args)?
} XrParamNode;

// Function declaration node
typedef struct FunctionDeclNode {
    char *name;
    XrParamNode **params;          // Array of parameter nodes
    int param_count;
    int required_count;            // Number of required params (without defaults)
    XrType *return_type;
    AstNode *body;
    bool is_generator;
    XrAttribute **attributes;
    int attr_count;
    XrGenericParam **type_params;
    int type_param_count;
} FunctionDeclNode;

// Function call node
// Supports generic call syntax: foo<int, string>(arg1, arg2)
typedef struct CallExprNode {
    AstNode *callee;
    AstNode **arguments;
    int arg_count;
    // Generic type arguments (e.g. foo<int, string>(...))
    XrType **type_args;
    int type_arg_count;
} CallExprNode;

// return statement node
typedef struct ReturnStmtNode {
    AstNode **values;
    int value_count;
} ReturnStmtNode;

// yield expression node
typedef struct YieldExprNode {
    AstNode *value;
} YieldExprNode;

// is expression node (runtime type check)
typedef struct IsExprNode {
    AstNode *expr;                    // Expression to check
    struct XrType *type;       // Type to check against
} IsExprNode;

typedef struct AsExprNode {
    AstNode *expr;             // Expression to cast
    struct XrType *type;       // Target type
    bool is_safe;              // true = safe cast (returns null on failure)
} AsExprNode;

// Array literal node
typedef struct ArrayLiteralNode {
    AstNode **elements;
    int count;
} ArrayLiteralNode;

// Index access node
typedef struct IndexGetNode {
    AstNode *array;
    AstNode *index;
} IndexGetNode;

// Index assignment node
typedef struct IndexSetNode {
    AstNode *array;
    AstNode *index;
    AstNode *value;
} IndexSetNode;

// Slice expression node
typedef struct SliceExprNode {
    AstNode *source;
    AstNode *start;
    AstNode *end;
} SliceExprNode;

// Member access node
typedef struct MemberAccessNode {
    AstNode *object;
    char *name;
} MemberAccessNode;

// Template string node
typedef struct TemplateStringNode {
    AstNode **parts;
    int part_count;
} TemplateStringNode;

// Object literal node
typedef struct ObjectLiteralNode {
    AstNode **keys;
    AstNode **values;
    bool *computed;
    int count;
} ObjectLiteralNode;

// Map literal node
typedef struct MapLiteralNode {
    AstNode **keys;
    AstNode **values;
    int count;
} MapLiteralNode;

// Set literal node
typedef struct SetLiteralNode {
    AstNode **elements;
    int count;
} SetLiteralNode;

// Type alias node
typedef struct TypeAliasNode {
    char *name;
    char **field_names;
    XrType **field_types;
    bool *field_optional;
    int field_count;
} TypeAliasNode;

// Struct literal node: Point{x: 1.0, y: 2.0} or Pair<int, string>{first: 1, second: "a"}
typedef struct StructLiteralNode {
    char *struct_name;
    char **field_names;
    AstNode **field_values;
    int field_count;
    // Generic type arguments (for monomorphization)
    XrType **type_args;
    int type_arg_count;
} StructLiteralNode;

// Class declaration node
typedef struct ClassDeclNode {
    char *name;
    char *super_name;
    char *super_module;  // Parent class module (supports extends module.Class syntax)
    char **interfaces;
    int interface_count;
    AstNode **fields;
    int field_count;
    AstNode **methods;
    int method_count;
    bool is_abstract;
    bool is_final;
    // Generic type parameters (e.g. class Box<T: Serializable>)
    XrGenericParam **type_params;
    int type_param_count;
} ClassDeclNode;

// Interface method signature node
typedef struct InterfaceMethodNode {
    char *name;
    char **parameters;
    XrType **param_types;
    int param_count;
    XrType *return_type;
} InterfaceMethodNode;

// Interface declaration node
typedef struct InterfaceDeclNode {
    char *name;
    char **extends;
    int extends_count;
    AstNode **methods;
    int method_count;
} InterfaceDeclNode;

// Field declaration node
typedef struct FieldDeclNode {
    char *name;
    XrType *field_type;
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
    XrType **param_types;
    uint8_t *param_passing_modes;  // XR_PARAM_VALUE / XR_PARAM_IN / XR_PARAM_REF per param
    int param_count;
    XrType *return_type;
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
    // Generic type parameters (e.g., add<T, U>(...))
    char **type_param_names;
    int type_param_count;
} MethodDeclNode;

// new expression node
// Supports two forms:
//   new ClassName()           - module_name = NULL
//   new module.ClassName()    - module_name = "module"
// Also supports generic type arguments: new Box<int>(42)
typedef struct NewExprNode {
    char *module_name;      // Module name (optional, for new module.Class())
    char *class_name;
    AstNode **arguments;
    int arg_count;
    // Generic type arguments (e.g. new Box<int>(...))
    XrType **type_args;
    int type_arg_count;
} NewExprNode;

// this expression node
typedef struct ThisExprNode {
    int placeholder;
} ThisExprNode;

// super call node
typedef struct SuperCallNode {
    char *method_name;
    AstNode **arguments;
    int arg_count;
} SuperCallNode;

// Member assignment node
typedef struct MemberSetNode {
    AstNode *object;
    char *member;
    AstNode *value;
} MemberSetNode;

// Enum member node
typedef struct EnumMemberNode {
    char *name;
    AstNode *value;
} EnumMemberNode;

// Enum declaration node
typedef struct EnumDeclNode {
    char *name;
    char *type_hint;
    AstNode **members;
    int member_count;
} EnumDeclNode;

// Enum access node
typedef struct EnumAccessNode {
    char *enum_name;
    char *member_name;
} EnumAccessNode;

// Enum conversion node
typedef struct EnumConvertNode {
    char *enum_name;
    AstNode *value_expr;
} EnumConvertNode;

// Enum index node (compiler-generated for for-in desugaring)
typedef struct EnumIndexNode {
    AstNode *collection;    // enum type expression
    AstNode *index_expr;    // index expression
} EnumIndexNode;

// Import member (used for selective imports)
typedef struct ImportMember {
    char *name;         // Original name
    char *alias;        // Alias (optional, import { foo as bar })
} ImportMember;

// Import statement node
// Supports two forms:
// 1. import "module" as name      (whole module import)
// 2. import { a, b as c } from "module"  (selective import)
typedef struct ImportStmtNode {
    char *module_name;          // Module path/name
    char *alias;                // Alias for whole module import
    ImportType import_type;     // Import type
    ImportMember *members;      // Selective import member list
    int member_count;           // Member count (0 means whole module import)
} ImportStmtNode;

// Re-export member structure
// Used for export { a, b as c } from "./file"
typedef struct ReexportMember {
    char *name;         // Original name
    char *alias;        // Export alias (optional)
} ReexportMember;

// Export statement node
// Supports three forms:
// 1. export let/const/fn/class ... (with declaration)
// 2. export a, b, c (export already defined variable list)
// 3. export { a, b as c } from "./file" (re-export)
typedef struct ExportStmtNode {
    AstNode *declaration;       // Declaration export (export let x = 1)
    char *export_name;          // Single export name (declaration style)
    char **export_names;        // Export name list (list style: export a, b)
    int export_count;           // Export count

    // Re-export support
    char *from_path;            // Source module path (e.g. "./user")
    ReexportMember *reexport_members;   // Re-export member list
    int reexport_count;         // Re-export member count
    bool is_reexport_all;       // Whether it's export * from "..."
} ExportStmtNode;

// Match expression node
typedef struct MatchExprNode {
    AstNode *expr;
    AstNode **arms;
    int arm_count;
} MatchExprNode;

// Match arm node
typedef struct MatchArmNode {
    AstNode *pattern;
    AstNode *guard;
    AstNode *body;
} MatchArmNode;

// Literal pattern node
typedef struct PatternLiteralNode {
    AstNode *value;
} PatternLiteralNode;

// Range pattern node
typedef struct PatternRangeNode {
    AstNode *start;
    AstNode *end;
} PatternRangeNode;

// Wildcard pattern node
typedef struct PatternWildcardNode {
    int placeholder;
} PatternWildcardNode;

// Multi-value pattern node
typedef struct PatternMultiNode {
    AstNode **patterns;
    int count;
} PatternMultiNode;

// Ternary expression node
typedef struct TernaryNode {
    AstNode *condition;
    AstNode *true_expr;
    AstNode *false_expr;
} TernaryNode;

// Optional chain node
typedef struct OptionalChainNode {
    AstNode *object;
    char *name;
    AstNode *index;
    int chain_type;
} OptionalChainNode;

// Range expression node
typedef struct RangeNode {
    AstNode *start;
    AstNode *end;
} RangeNode;

/* Link mode and scope mode enums are defined in xtask.h and xcoroutine.h.
 * Included here so AST nodes can reference them without circular deps. */
#include "../../coro/xtask.h"

// go expression node
// Supports:
//   go fn()                       - start coroutine
//   go(name: "xxx") fn()          - coroutine with name
//   go(priority: Coro.HIGH) fn()  - coroutine with priority
//   linked go fn()                - bidirectional error propagation
//   monitored go fn()             - one-way completion notification
typedef struct GoExprNode {
    AstNode *expr;              // Expression to execute (function call or closure)
    const char *name;           // Coroutine name (optional, for debugging)
    AstNode *priority;          // Priority expression (optional, supports Coro.LOW/NORMAL/HIGH or number)
    uint8_t link_mode;          // XR_LINK_NONE / XR_LINK_LINKED / XR_LINK_MONITORED
} GoExprNode;

// await expression node
typedef struct AwaitExprNode {
    AstNode *expr;
    AstNode *timeout;
    bool is_any;
    bool is_all;
    bool is_any_success;
} AwaitExprNode;

// Channel creation node
typedef struct ChannelNewNode {
    AstNode *buffer_size;
} ChannelNewNode;

// select case node
typedef struct SelectCaseNode {
    char *var_name;
    AstNode *channel;
    AstNode *value;
    AstNode *body;
    bool is_send;
    bool is_default;
    bool is_timeout;
} SelectCaseNode;

// select statement node
typedef struct SelectStmtNode {
    AstNode **cases;
    int case_count;
} SelectStmtNode;

// defer statement node
typedef struct DeferStmtNode {
    AstNode *expr;
} DeferStmtNode;

// scope block node
typedef struct ScopeBlockNode {
    AstNode *body;
    uint8_t scope_mode;         // XR_SCOPE_WAIT / XR_SCOPE_LINKED / XR_SCOPE_SUPERVISOR
} ScopeBlockNode;

// move expression node: move var (explicit ownership transfer)
typedef struct MoveExprNode {
    AstNode *expr;              // must be a variable reference
} MoveExprNode;

// cancelled() expression node
typedef struct CancelledExprNode {
    int placeholder;
} CancelledExprNode;

// AST node common structure.
//
// Source location contract (all 1-indexed; 0 means "not set"):
//   (line, column)         — node start. For declaration nodes this is the
//                            identifier's position, not the keyword, so the
//                            LSP can use it directly as selectionRange.
//   (end_line, end_column) — exclusive end of the node. For block-bodied
//                            declarations this is the position just past the
//                            closing brace. Parsers are responsible for
//                            filling these; LSP treats 0 as a hard error
//                            (caught by XR_DCHECK in release-asserts builds).
struct AstNode {
    AstNodeType type;
    int line;
    int column;                   // 1-indexed column number (for LSP)
    int end_line;                 // 1-indexed end line, 0 = unset
    int end_column;               // 1-indexed exclusive end column, 0 = unset
    struct XrType *compile_type;  // Unified type system (XrType)
    XrTrivia *leading_comments;   // Comments before this node (for formatter)

    union {
        LiteralNode literal;
        BinaryNode binary;
        UnaryNode unary;
        AstNode *grouping;
        AstNode *expr_stmt;
        PrintNode print_stmt;
        BlockNode block;
        VarDeclNode var_decl;
        VariableNode variable;
        AssignmentNode assignment;
        CompoundAssignmentNode compound_assignment;
        IncDecNode inc;
        IncDecNode dec;
        IfStmtNode if_stmt;
        WhileStmtNode while_stmt;
        ForStmtNode for_stmt;
        ForInStmtNode for_in_stmt;
        BreakStmtNode break_stmt;
        ContinueStmtNode continue_stmt;
        FunctionDeclNode function_decl;
        FunctionDeclNode function_expr;
        CallExprNode call_expr;
        ReturnStmtNode return_stmt;
        YieldExprNode yield_expr;
        IsExprNode is_expr;
        AsExprNode as_expr;
        ArrayLiteralNode array_literal;
        IndexGetNode index_get;
        IndexSetNode index_set;
        SliceExprNode slice_expr;
        MemberAccessNode member_access;
        TemplateStringNode template_str;
        ObjectLiteralNode object_literal;
        MapLiteralNode map_literal;
        SetLiteralNode set_literal;
        ClassDeclNode class_decl;
        ClassDeclNode struct_decl;
        StructLiteralNode struct_literal;
        InterfaceDeclNode interface_decl;
        InterfaceMethodNode interface_method;
        FieldDeclNode field_decl;
        MethodDeclNode method_decl;
        NewExprNode new_expr;
        ThisExprNode this_expr;
        SuperCallNode super_call;
        MemberSetNode member_set;
        EnumDeclNode enum_decl;
        EnumMemberNode enum_member;
        EnumAccessNode enum_access;
        EnumConvertNode enum_convert;
        EnumIndexNode enum_index;
        TryCatchNode try_catch;
        ThrowStmtNode throw_stmt;
        ImportStmtNode import_stmt;
        ExportStmtNode export_stmt;
        DestructureDeclNode destructure_decl;
        DestructureAssignNode destructure_assign;
        MultiVarDeclNode multi_var_decl;
        MultiAssignNode multi_assign;
        MatchExprNode match_expr;
        MatchArmNode match_arm;
        PatternLiteralNode pattern_literal;
        PatternRangeNode pattern_range;
        PatternWildcardNode pattern_wildcard;
        PatternMultiNode pattern_multi;
        TernaryNode ternary;
        OptionalChainNode optional_chain;
        RangeNode range;
        TypeAliasNode type_alias;
        GoExprNode go_expr;
        AwaitExprNode await_expr;
        ChannelNewNode channel_new;
        SelectStmtNode select_stmt;
        SelectCaseNode select_case;
        DeferStmtNode defer_stmt;
        ScopeBlockNode scope_block;
        CancelledExprNode cancelled_expr;
        MoveExprNode move_expr;
        ProgramNode program;
    } as;
};

// Safe AST union accessors - split into separate header
#include "xast_accessors.h"

#endif // XAST_NODES_H
