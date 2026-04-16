/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_ast_visitor.h - Generic AST visitor framework
 *
 * KEY CONCEPT:
 *   Provides a reusable visitor pattern for AST traversal.
 *   Used by both analyzer and LSP to avoid duplicating switch statements.
 */

#ifndef XANALYZER_AST_VISITOR_H
#define XANALYZER_AST_VISITOR_H

#include "../parser/xast_nodes.h"
#include <stdbool.h>
#include "../../base/xdefs.h"

// Visitor callback function type
typedef void (*XaVisitFn)(AstNode *node, void *ctx);

// Visitor structure with optional callbacks for different node types
typedef struct XaAstVisitor {
    // General callbacks (called for all nodes)
    XaVisitFn visit_pre;    // Called before visiting children
    XaVisitFn visit_post;   // Called after visiting children
    
    // Declaration callbacks
    XaVisitFn visit_function_decl;
    XaVisitFn visit_var_decl;
    XaVisitFn visit_const_decl;
    XaVisitFn visit_class_decl;
    XaVisitFn visit_method_decl;
    XaVisitFn visit_field_decl;
    XaVisitFn visit_interface_decl;
    
    // Expression callbacks
    XaVisitFn visit_variable;
    XaVisitFn visit_assignment;
    XaVisitFn visit_call_expr;
    XaVisitFn visit_member_access;
    XaVisitFn visit_index_get;
    XaVisitFn visit_function_expr;
    XaVisitFn visit_literal;
    
    // Statement callbacks
    XaVisitFn visit_block;
    XaVisitFn visit_if_stmt;
    XaVisitFn visit_for_stmt;
    XaVisitFn visit_for_in_stmt;
    XaVisitFn visit_while_stmt;
    XaVisitFn visit_return_stmt;
    XaVisitFn visit_import_stmt;
    
    // Control flag: set to true in callback to skip children
    bool skip_children;
    
    // User context passed to callbacks
    void *user_ctx;
} XaAstVisitor;

// Main visitor function - walks entire AST
XR_FUNC void xa_ast_visit(AstNode *root, XaAstVisitor *visitor);

// Convenience: visit with just pre/post callbacks
XR_FUNC void xa_ast_walk(AstNode *root, XaVisitFn pre, XaVisitFn post, void *ctx);

// Convenience: visit only declarations (functions, vars, classes)
XR_FUNC void xa_ast_visit_decls(AstNode *root, XaVisitFn fn, void *ctx);

// Convenience: visit only variable references
XR_FUNC void xa_ast_visit_refs(AstNode *root, XaVisitFn fn, void *ctx);

// Convenience: visit only function/method calls
XR_FUNC void xa_ast_visit_calls(AstNode *root, XaVisitFn fn, void *ctx);

#endif // XANALYZER_AST_VISITOR_H
