/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_nodes.h - AST aggregate header (P-03)
 *
 * KEY CONCEPT (P-03):
 *   Pulls in the four topic headers and then defines `struct AstNode`
 *   (the discriminated union over every node payload). Splitting
 *   xast_nodes.h was forced by the 800-line hard limit; downstream
 *   continues to include this file unchanged.
 *
 *   File budgets after the split:
 *     xast_nodes_common.h ~ 120 lines
 *     xast_nodes_expr.h    ~ 200 lines
 *     xast_nodes_stmt.h    ~ 200 lines
 *     xast_nodes_decl.h    ~ 230 lines
 *     xast_nodes.h         ~ 130 lines (this file)
 */

#ifndef XAST_NODES_H
#define XAST_NODES_H

#include "xast_nodes_common.h"
#include "xast_nodes_expr.h"
#include "xast_nodes_stmt.h"
#include "xast_nodes_decl.h"

/* ========== AstNode ==========
 *
 * Source location contract (all 1-indexed; 0 means "not set"):
 *   (line, column)         — node start. For declaration nodes this is the
 *                            identifier's position, not the keyword, so the
 *                            LSP can use it directly as selectionRange.
 *   (end_line, end_column) — exclusive end of the node. For block-bodied
 *                            declarations this is the position just past the
 *                            closing brace. Parsers are responsible for
 *                            filling these; LSP treats 0 as a hard error
 *                            (caught by XR_DCHECK in release-asserts builds).
 */
struct AstNode {
    AstNodeType type;
    uint32_t node_id;  // stable monotonic ID, unique per compilation unit
    int line;
    int column;      // 1-indexed column number (for LSP)
    int end_line;    // 1-indexed end line, 0 = unset
    int end_column;  // 1-indexed exclusive end column, 0 = unset
    // The inline compile_type field has been
    // removed. Inferred types live in XaAnalyzer's side table; access
    // via xa_analyzer_get_node_type(analyzer, node). Type-alias-
    // specific resolved types live in TypeAliasNode::resolved_type.
    XrTrivia *leading_comments;  // Comments before this node (for formatter)
    // Trailing inline comment captured by the lexer on the LAST
    // token consumed for this node (commonly `;` or `}` for stmts /
    // block-bodied decls, the final expression token for expr stmts).
    // Owned by the AST; freed with the node. NULL when the source had
    // no inline comment on the same source line as the closing token.
    XrTrivia *trailing_comments;

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

// Safe AST union accessors — split into separate header
#include "xast_accessors.h"

#endif  // XAST_NODES_H
