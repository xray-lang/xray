/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_internal.h - Internal shared declarations for analyzer visitor
 */

#ifndef XANALYZER_VISITOR_INTERNAL_H
#define XANALYZER_VISITOR_INTERNAL_H

#include "xanalyzer_visitor.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_incremental.h"
#include "xanalyzer_jit.h"
#include "../../base/xmalloc.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../base/xdefs.h"

// Main dispatch (defined in xanalyzer_visitor.c)
XR_FUNC XrType *xa_visit_infer(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_infer_expr(XaInferContext *ctx, AstNode *node);

// Utility functions (defined in xanalyzer_visitor.c)
XR_FUNC bool xa_check_null_safety(XaAnalyzer *analyzer, XrType *target, XrType *source,
                          const char *context_msg, XrLocation *loc);
XR_FUNC XrType *xa_infer_type_param_from_arg(XrType *param_type, XrType *arg_type,
                                     const char *tp_name, int depth);
XR_FUNC XrType *xa_substitute_generic_call(XaInferContext *ctx,
                                   XaSymbolLinks *links,
                                   XrType *callee_type,
                                   XrType *return_type,
                                   CallExprNode *call,
                                   int arg_count);
XR_FUNC XrType *xa_infer_function_return_type(XaInferContext *ctx, AstNode *body);
XR_FUNC bool xa_body_has_return_expr(AstNode *node);

// Expression visitors (defined in xanalyzer_visitor_expr.c)
XR_FUNC XrType *xa_visit_struct_literal(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_match_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_nullish_coalesce(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_optional_chain(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_force_unwrap(XaInferContext *ctx, AstNode *node);
XR_FUNC XrType *xa_visit_as_expr(XaInferContext *ctx, AstNode *node);
XR_FUNC void check_closure_capture(XaInferContext *ctx, AstNode *node, int line);
XR_FUNC void check_coro_capture(XaInferContext *ctx, AstNode *node, int line);

// Unified function body visitor (collect + direct traversal)
XR_FUNC void xa_visit_function_body_unified(XaInferContext *ctx, AstNode *body);

// Shared between expr and stmt visitors
XR_FUNC const char *get_typeof_arg_name(AstNode *node);

#endif // XANALYZER_VISITOR_INTERNAL_H
