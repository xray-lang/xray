/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor.h - AST visitor for type analysis
 *
 * KEY CONCEPT:
 *   Two-pass analysis:
 *   1. Collect pass: gather all declarations (symbols)
 *   2. Infer pass: compute types for all expressions
 */

#ifndef XANALYZER_VISITOR_H
#define XANALYZER_VISITOR_H

#include "xanalyzer.h"
#include "xanalyzer_infer.h"
#include "../parser/xast.h"
#include "../../base/xdefs.h"

// Pass 1: Symbol collection
XR_FUNC void xa_visit_collect(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_collect_program(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_collect_function(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_collect_class(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_collect_interface(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_collect_var_decl(XaInferContext *ctx, AstNode *node);

// Pass 1.5: Link class inheritance chains (after all classes collected)
XR_FUNC void xa_link_class_inheritance(XaAnalyzer *analyzer);

// Pass 2: Type inference
XR_FUNC XrType *xa_visit_infer(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_infer_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_infer_stmt(XaInferContext *ctx, AstNode *node);

// Expression inference by node type
XR_FUNC XrType *xa_visit_variable(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_binary(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_unary(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_call(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_member_access(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_index_get(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_array_literal(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_map_literal(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_object_literal(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_new_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_ternary(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_function_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_go_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_await_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_move_expr(XaInferContext *ctx, AstNode *node);

// Statement inference (updates flow graph)
XR_FUNC void xa_visit_var_decl_stmt(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_assignment_stmt(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_if_stmt(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_while_stmt(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_for_stmt(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_return_stmt(XaInferContext *ctx, AstNode *node);
XR_FUNC void xa_visit_block_stmt(XaInferContext *ctx, AstNode *node);

// Full analysis entry point
XR_FUNC void xa_analyze_ast(XaAnalyzer *analyzer, AstNode *ast);

#endif  // XANALYZER_VISITOR_H
